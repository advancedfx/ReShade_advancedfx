#define ImTextureID unsigned long long

#include <imgui.h>
#include <reshade.hpp>
#include <vector>
#include <queue>

using namespace reshade::api;

struct __declspec(uuid("745350f4-bd58-479b-8ad0-81e872a9952b")) device_data
{
	effect_runtime *main_runtime = nullptr;

	bool block_effects = true;

	bool _hasBackBuffer = false;
	resource_desc _back_buffer_desc;
	uint32_t _width;
	uint32_t _height;
	format _back_buffer_format;
	uint16_t _back_buffer_samples;
	resource _back_buffer_resolved = { 0 };
	resource_view _back_buffer_resolved_srv = { 0 };
	std::vector<resource_view> _back_buffer_targets;
	std::queue<effect_technique> _disabled_techniques;

	resource depth_texture = { 0 };
	resource_view depth_texture_view = { 0 };

	void free_depth_resources() {
		if (main_runtime == 0)
			return;

		const auto& device = main_runtime->get_device();
		if (depth_texture_view != 0) {
			device->destroy_resource_view(depth_texture_view);
			depth_texture_view = { 0 };
		}
		if (depth_texture != 0) {
			device->destroy_resource(depth_texture);
			depth_texture = { 0 };
		}

		update_effect_runtime();
	}

	void update_effect_runtime() const {
		if (main_runtime == 0)
			return;

		main_runtime->update_texture_bindings("DEPTH", depth_texture_view);

		main_runtime->enumerate_uniform_variables(nullptr, [this](effect_runtime* runtime, auto variable) {
			char source[32] = "";
			if (runtime->get_annotation_string_from_uniform_variable(variable, "source", source) && std::strcmp(source, "bufready_depth") == 0)
				runtime->set_uniform_value_bool(variable, depth_texture_view != 0);
			});
	}

	void disable_techniques() {
		main_runtime->enumerate_techniques(nullptr, [this](effect_runtime* runtime, auto technique) {
			if (runtime->get_technique_state(technique)) {
				_disabled_techniques.push(technique);
				runtime->set_technique_state(technique, false);
			}
			});
	}

	void reenable_techniques() {
		while (!_disabled_techniques.empty()) {
			main_runtime->set_technique_state(_disabled_techniques.front(), true);
			_disabled_techniques.pop();
		}
	}

	void free_buffer_resources() {
		if (main_runtime == 0)
			return;

		const auto& device = main_runtime->get_device();

		device->destroy_resource(_back_buffer_resolved);
		_back_buffer_resolved = {};
		device->destroy_resource_view(_back_buffer_resolved_srv);
		_back_buffer_resolved_srv = {};

		for (const auto view : _back_buffer_targets)
			device->destroy_resource_view(view);
		_back_buffer_targets.clear();

		_hasBackBuffer = false;
	}

	bool ensure_buffers(resource back_buffer_resource) {
		if (main_runtime == 0)
			return false;

		const auto& device = main_runtime->get_device();
		const resource_desc back_buffer_desc = device->get_resource_desc(back_buffer_resource);

		if (!_hasBackBuffer
			|| back_buffer_desc.flags != _back_buffer_desc.flags
			|| back_buffer_desc.heap != _back_buffer_desc.heap
			|| back_buffer_desc.texture.depth_or_layers != _back_buffer_desc.texture.depth_or_layers
			|| back_buffer_desc.texture.format != _back_buffer_desc.texture.format
			|| back_buffer_desc.texture.height != _back_buffer_desc.texture.height
			|| back_buffer_desc.texture.levels != _back_buffer_desc.texture.levels
			|| back_buffer_desc.texture.samples != _back_buffer_desc.texture.samples
			|| back_buffer_desc.texture.width != _back_buffer_desc.texture.width
			|| back_buffer_desc.type != _back_buffer_desc.type
			|| back_buffer_desc.usage != _back_buffer_desc.usage
			) {
			if (_hasBackBuffer) {
				free_buffer_resources();
			}

			_back_buffer_desc = back_buffer_desc;

			_width = back_buffer_desc.texture.width;
			_height = back_buffer_desc.texture.height;
			_back_buffer_format = format_to_default_typed(back_buffer_desc.texture.format);
			_back_buffer_samples = back_buffer_desc.texture.samples;

			// Create resolve texture and copy pipeline (do this before creating effect resources, to ensure correct back buffer format is set up)
			if (back_buffer_desc.texture.samples > 1
				// Some effects rely on there being an alpha channel available, so create resolve texture if that is not the case
				|| (_back_buffer_format == format::r8g8b8x8_unorm || _back_buffer_format == format::b8g8r8x8_unorm)
				)
			{
				switch (_back_buffer_format)
				{
				case format::r8g8b8x8_unorm:
					_back_buffer_format = format::r8g8b8a8_unorm;
					break;
				case format::b8g8r8x8_unorm:
					_back_buffer_format = format::b8g8r8a8_unorm;
					break;
				}

				if (!device->create_resource(
					resource_desc(_width, _height, 1, 1, format_to_typeless(_back_buffer_format), 1, memory_heap::gpu_only, resource_usage::shader_resource | resource_usage::render_target | resource_usage::copy_dest | resource_usage::resolve_dest),
					nullptr, back_buffer_desc.texture.samples == 1 ? resource_usage::copy_dest : resource_usage::resolve_dest, &_back_buffer_resolved) ||
					!device->create_resource_view(
						_back_buffer_resolved,
						resource_usage::shader_resource,
						resource_view_desc(_back_buffer_format),
						&_back_buffer_resolved_srv) ||
					!device->create_resource_view(
						_back_buffer_resolved,
						resource_usage::render_target,
						resource_view_desc(format_to_default_typed(_back_buffer_format, 0)),
						&_back_buffer_targets.emplace_back()) ||
					!device->create_resource_view(
						_back_buffer_resolved,
						resource_usage::render_target,
						resource_view_desc(format_to_default_typed(_back_buffer_format, 1)),
						&_back_buffer_targets.emplace_back()))
				{
					free_buffer_resources();
					return false;
				}
			}		
			// Create render targets for the back buffer resources
			else if (!device->create_resource_view(
				back_buffer_resource,
				resource_usage::render_target,
				resource_view_desc(
					back_buffer_desc.texture.samples > 1 ? resource_view_type::texture_2d_multisample : resource_view_type::texture_2d,
					format_to_default_typed(back_buffer_desc.texture.format, 0), 0, 1, 0, 1),
				&_back_buffer_targets.emplace_back()) ||
				!device->create_resource_view(
					back_buffer_resource,
					resource_usage::render_target,
					resource_view_desc(
						back_buffer_desc.texture.samples > 1 ? resource_view_type::texture_2d_multisample : resource_view_type::texture_2d,
						format_to_default_typed(back_buffer_desc.texture.format, 1), 0, 1, 0, 1),
					&_back_buffer_targets.emplace_back()))
			{
				free_buffer_resources();
				return false;
			}

			_hasBackBuffer = true;
		}

		return _hasBackBuffer;
	}
};

command_list* g_MainCommandList = 0;

static void update_effect_runtime(effect_runtime* runtime)
{
	const auto &dev_data = runtime->get_device()->get_private_data<device_data>();
	dev_data.update_effect_runtime();
}

bool supply_depth(void* pDepthTextureResource) {
	if (g_MainCommandList == 0)
		return false;

	device* const device = g_MainCommandList->get_device();
	auto& dev_data = device->get_private_data<device_data>();

	if (pDepthTextureResource == 0) {
		dev_data.free_depth_resources();
		return false;
	}

	if (dev_data.main_runtime == 0)
		return false;

	resource rs_depth{ (uint64_t)pDepthTextureResource };
	const resource_desc rs_depth_desc(device->get_resource_desc(rs_depth));
	const resource_desc rs_depth_target_desc(dev_data.depth_texture != 0 ? device->get_resource_desc(dev_data.depth_texture) : resource_desc());

	if (dev_data.depth_texture != 0 && (rs_depth_desc.texture.width != rs_depth_target_desc.texture.width || rs_depth_desc.texture.height != rs_depth_desc.texture.height)) {
		dev_data.free_depth_resources();
	}

	if (dev_data.depth_texture == 0) {
		resource_desc desc(rs_depth_desc);
		desc.type = resource_type::texture_2d;
		desc.heap = memory_heap::gpu_only;
		desc.usage = resource_usage::shader_resource | resource_usage::copy_dest;
		desc.texture.format = format::r32_float;

		if (device->create_resource(desc, nullptr, resource_usage::copy_dest, &dev_data.depth_texture))
			device->set_resource_name(dev_data.depth_texture, "ReShade advancedfx depth texture");
		else
			return false;

		resource_view_desc view_desc(format_to_default_typed(desc.texture.format));
		if (!device->create_resource_view(dev_data.depth_texture, resource_usage::shader_resource, view_desc, &dev_data.depth_texture_view))
			return false;

		update_effect_runtime(dev_data.main_runtime);
	}
	if (dev_data.depth_texture == 0
		|| dev_data.depth_texture_view == 0)
		return false;

	g_MainCommandList->barrier(dev_data.depth_texture, resource_usage::shader_resource, resource_usage::copy_dest);
	g_MainCommandList->barrier(rs_depth, resource_usage::render_target, resource_usage::copy_source);

	g_MainCommandList->copy_resource(rs_depth, dev_data.depth_texture);

	g_MainCommandList->barrier(rs_depth, resource_usage::copy_source, resource_usage::render_target);
	g_MainCommandList->barrier(dev_data.depth_texture, resource_usage::copy_dest, resource_usage::shader_resource);

	return true;
}

extern "C" bool __declspec(dllexport) AdvancedfxRenderEffects(void* pRenderTargetView, void * pDepthTextureResource) {

	if (pRenderTargetView == 0) {
		// HLAE wants us to render no effects.
		return true;
	}

	if (g_MainCommandList == 0)
		return false;

	device* const device = g_MainCommandList->get_device();
	auto& dev_data = device->get_private_data<device_data>();

	if (dev_data.main_runtime == 0)
		return false;

	resource back_buffer_resource{ (uint64_t)pRenderTargetView };

	if (!dev_data.ensure_buffers(back_buffer_resource))
		return false;

	// Resolve MSAA back buffer if MSAA is active or copy when format conversion is required
	if (dev_data._back_buffer_resolved != 0)
	{
		if (dev_data._back_buffer_samples == 1)
		{
			g_MainCommandList->barrier(back_buffer_resource, resource_usage::present, resource_usage::copy_source);
			g_MainCommandList->copy_texture_region(back_buffer_resource, 0, nullptr, dev_data._back_buffer_resolved, 0, nullptr);
			g_MainCommandList->barrier(dev_data._back_buffer_resolved, resource_usage::copy_dest, resource_usage::render_target);
		}
		else
		{
			g_MainCommandList->barrier(back_buffer_resource, resource_usage::present, resource_usage::resolve_source);
			g_MainCommandList->resolve_texture_region(back_buffer_resource, 0, nullptr, dev_data._back_buffer_resolved, 0, 0, 0, 0, dev_data._back_buffer_format);
			g_MainCommandList->barrier(dev_data._back_buffer_resolved, resource_usage::resolve_dest, resource_usage::render_target);
		}
	}

	supply_depth(pDepthTextureResource);

	if (dev_data._back_buffer_resolved != 0)
	{
		dev_data.block_effects = false;
		dev_data.main_runtime->render_effects(g_MainCommandList, dev_data._back_buffer_targets[0], dev_data._back_buffer_targets[1]);
		dev_data.block_effects = true;
	}
	else
	{
		g_MainCommandList->barrier(back_buffer_resource, resource_usage::present, resource_usage::render_target);
		dev_data.block_effects = false;
		dev_data.main_runtime->render_effects(g_MainCommandList, dev_data._back_buffer_targets[0], dev_data._back_buffer_targets[1]);
		dev_data.block_effects = true;
		g_MainCommandList->barrier(back_buffer_resource, resource_usage::render_target, resource_usage::present);
	}

	// Stretch main render target back into MSAA back buffer if MSAA is active or copy when format conversion is required
	if (dev_data._back_buffer_resolved != 0)
	{
		const resource resources[2] = { back_buffer_resource, dev_data._back_buffer_resolved };
		const resource_usage state_old[2] = { resource_usage::copy_source | resource_usage::resolve_source, resource_usage::render_target };
		const resource_usage state_final[2] = { resource_usage::present, resource_usage::resolve_dest };

		const resource_usage state_new[2] = { resource_usage::copy_dest, resource_usage::copy_source };

		g_MainCommandList->barrier(2, resources, state_old, state_new);
		g_MainCommandList->copy_texture_region(dev_data._back_buffer_resolved, 0, nullptr, back_buffer_resource, 0, nullptr);
		g_MainCommandList->barrier(2, resources, state_new, state_final);
	}

	return true;
}

static void on_init_device(device *device)
{
	device->create_private_data<device_data>();
}
static void on_destroy_device(device *device)
{
	device->destroy_private_data<device_data>();
}

static void on_init_command_list(command_list *cmd_list)
{
	if (g_MainCommandList == 0) g_MainCommandList = cmd_list;
}
static void on_destroy_command_list(command_list *cmd_list)
{
	if (g_MainCommandList == cmd_list) g_MainCommandList = 0;
}

static void on_init_effect_runtime(effect_runtime *runtime)
{
	auto &dev_data = runtime->get_device()->get_private_data<device_data>();
	// Assume last created effect runtime is the main one
	dev_data.main_runtime = runtime;
}
static void on_destroy_effect_runtime(effect_runtime *runtime)
{
	device* const device = runtime->get_device();

	auto &dev_data = device->get_private_data<device_data>();
	if (runtime == dev_data.main_runtime) {
		dev_data.free_depth_resources();
		dev_data.free_buffer_resources();
		dev_data.main_runtime = nullptr;
	}
}

static void on_begin_render_effects(effect_runtime* runtime, command_list* cmd_list, resource_view, resource_view)
{
	if (g_MainCommandList == 0)
		return;

	device* const device = g_MainCommandList->get_device();
	auto& dev_data = device->get_private_data<device_data>();

	if (dev_data.block_effects) dev_data.disable_techniques();
}

static void on_finish_render_effects(effect_runtime* runtime, command_list* cmd_list, resource_view, resource_view)
{
	if (g_MainCommandList == 0)
		return;

	device* const device = g_MainCommandList->get_device();
	auto& dev_data = device->get_private_data<device_data>();

	if (dev_data.block_effects) dev_data.reenable_techniques();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
			return FALSE;
		reshade::register_event<reshade::addon_event::init_device>(on_init_device);
		reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
		reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
		reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
		reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_render_effects);
		reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish_render_effects);

		// Need to set texture binding again after reloading
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(update_effect_runtime);
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(hModule);
		break;
	}

	return TRUE;
}
