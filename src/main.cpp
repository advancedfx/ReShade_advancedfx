#define ImTextureID unsigned long long

#include <imgui.h>
#include <reshade.hpp>
#include <vector>
#include <queue>

using namespace reshade::api;

struct __declspec(uuid("9A609C4B-75C6-47C5-AFB5-4C65E0807F69")) runtime_data
{
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

	void free_depth_resources(effect_runtime * runtime) {
		const auto& device = runtime->get_device();
		if (depth_texture_view != 0) {
			device->destroy_resource_view(depth_texture_view);
			depth_texture_view = { 0 };
		}
		if (depth_texture != 0) {
			device->destroy_resource(depth_texture);
			depth_texture = { 0 };
		}

		update_effect_runtime(runtime);
	}

	void update_effect_runtime(effect_runtime* runtime) const {

		runtime->update_texture_bindings("DEPTH", depth_texture_view, depth_texture_view);

		runtime->enumerate_uniform_variables(nullptr, [this](effect_runtime* runtime, auto variable) {
			char source[32] = "";
			if (runtime->get_annotation_string_from_uniform_variable(variable, "source", source) && std::strcmp(source, "bufready_depth") == 0)
				runtime->set_uniform_value_bool(variable, depth_texture_view != 0);
			});
	}

	void disable_techniques(effect_runtime* runtime) {
		runtime->enumerate_techniques(nullptr, [this](effect_runtime* runtime, auto technique) {
			if (runtime->get_technique_state(technique)) {
				_disabled_techniques.push(technique);
				runtime->set_technique_state(technique, false);
			}
			});
	}

	void reenable_techniques(effect_runtime* runtime) {
		while (!_disabled_techniques.empty()) {
			runtime->set_technique_state(_disabled_techniques.front(), true);
			_disabled_techniques.pop();
		}
	}

	void free_buffer_resources(effect_runtime* runtime) {
		const auto& device = runtime->get_device();

		device->destroy_resource(_back_buffer_resolved);
		_back_buffer_resolved = {};
		device->destroy_resource_view(_back_buffer_resolved_srv);
		_back_buffer_resolved_srv = {};

		for (const auto view : _back_buffer_targets)
			device->destroy_resource_view(view);
		_back_buffer_targets.clear();

		_hasBackBuffer = false;
	}

	bool ensure_buffers(effect_runtime* runtime, resource back_buffer_resource) {

		const auto& device = runtime->get_device();
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
				free_buffer_resources(runtime);
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
					free_buffer_resources(runtime);
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
				free_buffer_resources(runtime);
				return false;
			}

			_hasBackBuffer = true;
		}

		return _hasBackBuffer;
	}
};

effect_runtime* g_MainRuntime = 0;

static void update_effect_runtime(effect_runtime* runtime)
{
	auto &data = runtime->get_private_data<runtime_data>();
	data.update_effect_runtime(runtime);
}

bool supply_depth(effect_runtime* runtime, void* pDepthTextureResource) {
	auto& data = runtime->get_private_data<runtime_data>();

	if (pDepthTextureResource == 0) {
		data.free_depth_resources(runtime);
		return false;
	}

	device* const device = runtime->get_device();

	resource rs_depth{ (uint64_t)pDepthTextureResource };
	const resource_desc rs_depth_desc(device->get_resource_desc(rs_depth));
	const resource_desc rs_depth_target_desc(data.depth_texture != 0 ? device->get_resource_desc(data.depth_texture) : resource_desc());

	if (data.depth_texture != 0 && (rs_depth_desc.texture.width != rs_depth_target_desc.texture.width || rs_depth_desc.texture.height != rs_depth_desc.texture.height)) {
		data.free_depth_resources(runtime);
	}

	if (data.depth_texture == 0) {
		resource_desc desc(rs_depth_desc);
		desc.type = resource_type::texture_2d;
		desc.heap = memory_heap::gpu_only;
		desc.usage = resource_usage::shader_resource | resource_usage::copy_dest;
		//desc.texture.format = format::r32_float;

		if (device->create_resource(desc, nullptr, resource_usage::copy_dest, &data.depth_texture))
			device->set_resource_name(data.depth_texture, "ReShade advancedfx depth texture");
		else
			return false;

		resource_view_desc view_desc(format_to_default_typed(desc.texture.format));
		if (!device->create_resource_view(data.depth_texture, resource_usage::shader_resource, view_desc, &data.depth_texture_view))
			return false;

		update_effect_runtime(runtime);
	}
	if (data.depth_texture == 0
		|| data.depth_texture_view == 0)
		return false;

	if (auto command_queue = runtime->get_command_queue()) {
		if (auto command_list = command_queue->get_immediate_command_list()) {
			command_list->barrier(data.depth_texture, resource_usage::shader_resource, resource_usage::copy_dest);
			command_list->barrier(rs_depth, resource_usage::render_target, resource_usage::copy_source);

			command_list->copy_resource(rs_depth, data.depth_texture);

			command_list->barrier(rs_depth, resource_usage::copy_source, resource_usage::render_target);
			command_list->barrier(data.depth_texture, resource_usage::copy_dest, resource_usage::shader_resource);
			return true;
		}
	}

	return false;
}

extern "C" bool __declspec(dllexport) AdvancedfxRenderEffects(void* pRenderTargetView, void * pDepthTextureResource) {

	if (pRenderTargetView == 0) {
		// HLAE wants us to render no effects.
		return true;
	}

	if (g_MainRuntime == 0)
		return false;

	auto& data = g_MainRuntime->get_private_data<runtime_data>();

	resource back_buffer_resource{ (uint64_t)pRenderTargetView };

	if (!data.ensure_buffers(g_MainRuntime, back_buffer_resource))
		return false;

	if (auto command_queue = g_MainRuntime->get_command_queue()) {
		if (auto command_list = command_queue->get_immediate_command_list()) {

			// Resolve MSAA back buffer if MSAA is active or copy when format conversion is required
			if (data._back_buffer_resolved != 0)
			{
				if (data._back_buffer_samples == 1)
				{
					command_list->barrier(back_buffer_resource, resource_usage::present, resource_usage::copy_source);
					command_list->copy_texture_region(back_buffer_resource, 0, nullptr, data._back_buffer_resolved, 0, nullptr);
					command_list->barrier(data._back_buffer_resolved, resource_usage::copy_dest, resource_usage::render_target);
				}
				else
				{
					command_list->barrier(back_buffer_resource, resource_usage::present, resource_usage::resolve_source);
					command_list->resolve_texture_region(back_buffer_resource, 0, nullptr, data._back_buffer_resolved, 0, 0, 0, 0, data._back_buffer_format);
					command_list->barrier(data._back_buffer_resolved, resource_usage::resolve_dest, resource_usage::render_target);
				}
			}

			supply_depth(g_MainRuntime, pDepthTextureResource);

			if (data._back_buffer_resolved != 0)
			{
				data.block_effects = false;
				g_MainRuntime->render_effects(command_list, data._back_buffer_targets[0], data._back_buffer_targets[1]);
				data.block_effects = true;
			}
			else
			{
				command_list->barrier(back_buffer_resource, resource_usage::present, resource_usage::render_target);
				data.block_effects = false;
				g_MainRuntime->render_effects(command_list, data._back_buffer_targets[0], data._back_buffer_targets[1]);
				data.block_effects = true;
				command_list->barrier(back_buffer_resource, resource_usage::render_target, resource_usage::present);
			}

			// Stretch main render target back into MSAA back buffer if MSAA is active or copy when format conversion is required
			if (data._back_buffer_resolved != 0)
			{
				const resource resources[2] = { back_buffer_resource, data._back_buffer_resolved };
				const resource_usage state_old[2] = { resource_usage::copy_source | resource_usage::resolve_source, resource_usage::render_target };
				const resource_usage state_final[2] = { resource_usage::present, resource_usage::resolve_dest };

				const resource_usage state_new[2] = { resource_usage::copy_dest, resource_usage::copy_source };

				command_list->barrier(2, resources, state_old, state_new);
				command_list->copy_texture_region(data._back_buffer_resolved, 0, nullptr, back_buffer_resource, 0, nullptr);
				command_list->barrier(2, resources, state_new, state_final);
			}

			return true;
		}
	}

	return false;
}

static void on_init_effect_runtime(effect_runtime *runtime)
{
	runtime->create_private_data<runtime_data>();
}
static void on_destroy_effect_runtime(effect_runtime *runtime)
{
	auto & data = runtime->get_private_data<runtime_data>();
	data.free_depth_resources(runtime);
	data.free_buffer_resources(runtime);

	if (runtime == g_MainRuntime) {
		g_MainRuntime = nullptr;
	}

	runtime->destroy_private_data<runtime_data>();
}

static void on_begin_render_effects(effect_runtime* runtime, command_list* /*cmd_list*/, resource_view, resource_view)
{
	if (nullptr == g_MainRuntime) g_MainRuntime = runtime;

	auto& data = runtime->get_private_data<runtime_data>();
	if (data.block_effects) data.disable_techniques(runtime);
}

static void on_finish_render_effects(effect_runtime* runtime, command_list* /*cmd_list*/, resource_view, resource_view)
{
	auto& data = runtime->get_private_data<runtime_data>();

	if (data.block_effects) data.reenable_techniques(runtime);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
			return FALSE;
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
