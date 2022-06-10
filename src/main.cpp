#define ImTextureID unsigned long long

#include <imgui.h>
#include <reshade.hpp>

using namespace reshade::api;

struct __declspec(uuid("745350f4-bd58-479b-8ad0-81e872a9952b")) device_data
{
	effect_runtime *main_runtime = nullptr;

	resource depth_texture = { 0 };
	resource_view depth_texture_view = { 0 };
	resource vertex_rect = { 0 };

	void free_resources() {
		if (main_runtime == 0)
			return;

		const auto& device = main_runtime->get_device();
		if (vertex_rect != 0) {
			device->destroy_resource(vertex_rect);
			vertex_rect = { 0 };
		}
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
		dev_data.free_resources();
		return false;
	}

	if (dev_data.main_runtime == 0)
		return false;

	resource rs_depth{ (uint64_t)pDepthTextureResource };
	const resource_desc rs_depth_desc(device->get_resource_desc(rs_depth));
	const resource_desc rs_depth_target_desc(dev_data.depth_texture != 0 ? device->get_resource_desc(dev_data.depth_texture) : resource_desc());

	if (dev_data.depth_texture != 0 && (rs_depth_desc.texture.width != rs_depth_target_desc.texture.width || rs_depth_desc.texture.height != rs_depth_desc.texture.height) || true) {
		dev_data.free_resources();
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

		float vertex_data[4][3] = {
			{-1,-1,0},
			{1,-1,0},
			{-1,1,0},
			{ 1,1,0}
		};
		subresource_data data;
		data.data = &vertex_data;
		resource_desc vertex_data_desc(sizeof(float) * 4 * 3, memory_heap::gpu_only, resource_usage::vertex_buffer);
		if (!device->create_resource(vertex_data_desc, &data, resource_usage::vertex_buffer, &dev_data.vertex_rect))
			return false;

		update_effect_runtime(dev_data.main_runtime);
	}
	if (dev_data.depth_texture == 0
		|| dev_data.depth_texture_view == 0
		|| dev_data.vertex_rect == 0)
		return false;

	render_pass_render_target_desc rts;
	rts.clear_color[0] = 1;
	rts.clear_color[1] = 1;
	rts.clear_color[2] = 1;
	rts.clear_color[3] = 1;
	rts.load_op = render_pass_load_op::load;
	rts.store_op = render_pass_store_op::store;
	rts.view = dev_data.depth_texture_view;

	g_MainCommandList->barrier(dev_data.depth_texture, resource_usage::shader_resource, resource_usage::copy_dest);
	g_MainCommandList->barrier(rs_depth, resource_usage::render_target, resource_usage::copy_source);

	g_MainCommandList->copy_resource(rs_depth, dev_data.depth_texture);

	g_MainCommandList->barrier(rs_depth, resource_usage::copy_source, resource_usage::render_target);
	g_MainCommandList->barrier(dev_data.depth_texture, resource_usage::copy_dest, resource_usage::shader_resource);

	return true;
}

extern "C" bool __declspec(dllexport) AdvancedfxRenderEffects(void* pRenderTargetView, void * pDepthTextureResource) {

	if (g_MainCommandList == 0)
		return false;

	device* const device = g_MainCommandList->get_device();
	const auto& dev_data = device->get_private_data<device_data>();

	if (dev_data.main_runtime == 0)
		return false;

	if (pRenderTargetView == 0) {
		// HLAE wants us to render no effects.
		dev_data.main_runtime->render_effects(g_MainCommandList, resource_view{ 0 });
		return true;
	}

	resource_view rtv{ (uint64_t)pRenderTargetView };

	uint32_t width, height;
	dev_data.main_runtime->get_screenshot_width_and_height(&width, &height);

	const resource_desc render_target_desc = device->get_resource_desc(device->get_resource_from_view(rtv));

	if (render_target_desc.texture.width != width || render_target_desc.texture.height != height)
		return false; // Ignore render targets that do not match the effect runtime back buffer dimensions

	supply_depth(pDepthTextureResource);

	dev_data.main_runtime->render_effects(g_MainCommandList, rtv);

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
		dev_data.free_resources();
		dev_data.main_runtime = nullptr;
	}
}

static void on_draw_settings(effect_runtime *runtime)
{
	device *const device = runtime->get_device();
	auto &dev_data = device->get_private_data<device_data>();

	if (runtime != dev_data.main_runtime)
	{
		ImGui::TextUnformatted("This is not the main effect runtime.");
		return;
	}

	ImGui::TextUnformatted("v1.0.0");
	ImGui::TextUnformatted("There are no settings currently.");
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
		reshade::register_overlay(nullptr, on_draw_settings);

		// Need to set texture binding again after reloading
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(update_effect_runtime);
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(hModule);
		break;
	}

	return TRUE;
}
