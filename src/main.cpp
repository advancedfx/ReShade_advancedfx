#define ImTextureID unsigned long long

#include <imgui.h>
#include <reshade.hpp>
#include <reshade_api_device.hpp>
#include <vector>
#include <queue>
#include <map>
#include <set>

HMODULE g_hReShadeModule = nullptr;
#define IDR_COPY_PS                     101
#define IDR_FULLSCREEN_VS               102

struct data_resource
{
	size_t data_size;
	const void* data;
};

data_resource load_data_resource(HMODULE module_handke, unsigned short id)
{
	const HRSRC info = FindResource(module_handke, MAKEINTRESOURCE(id), RT_RCDATA);
	assert(info != nullptr);
	const HGLOBAL handle = LoadResource(module_handke, info);

	data_resource result;
	result.data = LockResource(handle);
	result.data_size = SizeofResource(module_handke, info);

	return result;
}

using namespace reshade::api;

void update_dependent_effect_runtime(effect_runtime* runtime, resource depth_buffer_resource, resource_view rsv);

struct cmp_resource {
	bool operator()(const resource& a, const resource& b) const {
		return a.handle < b.handle;
	}
};


struct __declspec(uuid("9A609C4B-75C6-47C5-AFB5-4C65E0807F69")) runtime_data_s
{
	bool block_effects = true;
	bool effects_were_enabled = true;
	resource _current_depth_buffer_resource = { 0 };
	resource_view _current_depth_texture_view = { 0 };

	void update_effect_runtime(effect_runtime* runtime) const {
		runtime->update_texture_bindings("DEPTH", _current_depth_texture_view, _current_depth_texture_view);

		runtime->enumerate_uniform_variables(nullptr, [this](effect_runtime* runtime, auto variable) {
			char source[32] = "";
			if (runtime->get_annotation_string_from_uniform_variable(variable, "source", source) && std::strcmp(source, "bufready_depth") == 0)
				runtime->set_uniform_value_bool(variable, _current_depth_texture_view != 0);
			});
	}

	void update_effect_runtime(effect_runtime* runtime, resource depth_buffer_resource, resource_view depth_texture_view) {
		_current_depth_buffer_resource = depth_buffer_resource;
		_current_depth_texture_view = depth_texture_view;
		this->update_effect_runtime(runtime);
	}
};

struct __declspec(uuid("4F2FCBC8-D459-4325-A4D8-4EE63F5C4571")) device_data_s
{
	struct back_buffer_data_s {
		bool _hasBackBuffer = false;
		resource_desc _back_buffer_desc;
		uint32_t _width;
		uint32_t _height;
		format _back_buffer_format;
		uint16_t _back_buffer_samples;
		resource _back_buffer_resolved = { 0 };
		resource_view _back_buffer_resolved_srv = {};
		std::vector<resource_view> _back_buffer_targets;
		std::vector<resource_view> _back_buffer_revoled_targets;

		void free_buffer_resources(device* device) {
			if (_hasBackBuffer) {
				for (const auto view : _back_buffer_revoled_targets)
					device->destroy_resource_view(view);
				_back_buffer_revoled_targets.clear();


				device->destroy_resource(_back_buffer_resolved);
				_back_buffer_resolved = {};
				device->destroy_resource_view(_back_buffer_resolved_srv);
				_back_buffer_resolved_srv = {};

				for (const auto view : _back_buffer_targets)
					device->destroy_resource_view(view);
				_back_buffer_targets.clear();

				_hasBackBuffer = false;
			}
		}

		bool ensure_buffers(device* device, resource back_buffer_resource) {
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
					free_buffer_resources(device);
				}

				_back_buffer_desc = back_buffer_desc;

				_width = back_buffer_desc.texture.width;
				_height = back_buffer_desc.texture.height;
				_back_buffer_format = format_to_default_typed(back_buffer_desc.texture.format);
				_back_buffer_samples = back_buffer_desc.texture.samples;

				// Create resolve texture and copy pipeline (do this before creating effect resources, to ensure correct back buffer format is set up)
				if (back_buffer_desc.texture.samples > 1
					// Some effects rely on there being an alpha channel available, so create resolve texture if that is not the case
					|| (_back_buffer_format == format::r8g8b8x8_unorm || _back_buffer_format == format::b8g8r8x8_unorm
						|| _back_buffer_format == format::r8g8b8x8_unorm_srgb || _back_buffer_format == format::b8g8r8x8_unorm_srgb)
					)
				{
					switch (_back_buffer_format)
					{
					case format::r8g8b8x8_unorm:
					case format::r8g8b8x8_unorm_srgb:
						_back_buffer_format = format::r8g8b8a8_unorm;
						break;
					case format::b8g8r8x8_unorm:
					case format::b8g8r8x8_unorm_srgb:
						_back_buffer_format = format::b8g8r8a8_unorm;
						break;
					}

					const bool need_copy_pipeline =
						device->get_api() == device_api::d3d10 ||
						device->get_api() == device_api::d3d11 ||
						device->get_api() == device_api::d3d12;

					resource_usage usage = resource_usage::render_target | resource_usage::copy_dest | resource_usage::resolve_dest;
					if (need_copy_pipeline)
						usage |= resource_usage::shader_resource;
					else
						usage |= resource_usage::copy_source;

					if (!device->create_resource(
						resource_desc(_width, _height, 1, 1, format_to_typeless(_back_buffer_format), 1, memory_heap::gpu_only, usage),
						nullptr, back_buffer_desc.texture.samples == 1 ? resource_usage::copy_dest : resource_usage::resolve_dest, &_back_buffer_resolved) ||
						!device->create_resource_view(
							_back_buffer_resolved,
							resource_usage::render_target,
							resource_view_desc(format_to_default_typed(_back_buffer_format, 0)),
							&_back_buffer_revoled_targets.emplace_back()) ||
						!device->create_resource_view(
							_back_buffer_resolved,
							resource_usage::render_target,
							resource_view_desc(format_to_default_typed(_back_buffer_format, 1)),
							&_back_buffer_revoled_targets.emplace_back())) {
						free_buffer_resources(device);
						return false;
					}

					if (need_copy_pipeline)
					{
						if (!device->create_resource_view(
							_back_buffer_resolved,
							resource_usage::shader_resource,
							resource_view_desc(_back_buffer_format),
							&_back_buffer_resolved_srv))
						{
							free_buffer_resources(device);
							return false;
						}
					}
				}
				// Create render targets for the back buffer resources
				if (!device->create_resource_view(
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
					free_buffer_resources(device);
					return false;
				}

				_hasBackBuffer = true;
			}

			return _hasBackBuffer;
		}
	};

	struct depth_texture_data_s {
		resource _depth_texture = { 0 };
		resource_view _depth_texture_view = { 0 };

		void free_depth_resources(device* device) {
			if (_depth_texture_view != 0) {
				device->destroy_resource_view(_depth_texture_view);
				_depth_texture_view = { 0 };
			}
			if (_depth_texture != 0) {
				device->destroy_resource(_depth_texture);
				_depth_texture = { 0 };
			}
		}

		bool supply_depth(device * device, resource depth_texture_resource) {
			if (depth_texture_resource == 0) {
				free_depth_resources(device);
				return false;
			}

			const resource_desc rs_depth_desc(device->get_resource_desc(depth_texture_resource));
			const resource_desc rs_depth_target_desc(_depth_texture != 0 ? device->get_resource_desc(_depth_texture) : resource_desc());

			if (_depth_texture != 0 && (rs_depth_desc.texture.width != rs_depth_target_desc.texture.width || rs_depth_desc.texture.height != rs_depth_desc.texture.height)) {
				free_depth_resources(device);
			}

			if (_depth_texture == 0) {
				resource_desc desc(rs_depth_desc);
				if (desc.texture.samples == 1) // we always expect a resolved depth buffer from HLAE (otherwise we would need to resolve it ourselves with shaders)
				{
					desc.type = resource_type::texture_2d;
					desc.heap = memory_heap::gpu_only;
					desc.usage = resource_usage::shader_resource | resource_usage::copy_dest;
					//desc.texture.format = format::r32_float;

					if (device->create_resource(desc, nullptr, resource_usage::copy_dest, &_depth_texture))
						device->set_resource_name(_depth_texture, "ReShade advancedfx depth texture");
					else
						return false;

					resource_view_desc view_desc(format_to_default_typed(desc.texture.format));
					if (!device->create_resource_view(_depth_texture, resource_usage::shader_resource, view_desc, &_depth_texture_view))
						return false;
				}
			}
			if (_depth_texture == 0
				|| _depth_texture_view == 0)
				return false;

			return true;
		}
	};

	std::map<resource, back_buffer_data_s, cmp_resource> _back_buffers;
	std::map<resource, depth_texture_data_s, cmp_resource> _depth_buffers;
	std::set<effect_runtime*> _effect_runtimes;

	void free_depth_resources(device* device, resource depth_texture_resource) {
		auto it = _depth_buffers.find(depth_texture_resource);
		if (it != _depth_buffers.end()) {
			for (auto it2 = _effect_runtimes.begin(); it2 != _effect_runtimes.end(); it2++) {
				auto runtime_data = (*it2)->get_private_data<runtime_data_s>();
				if (runtime_data->_current_depth_buffer_resource == depth_texture_resource)
					runtime_data->update_effect_runtime(*it2, { 0 }, { 0 });
			}

			auto& depth_texture_data = it->second;
			device->destroy_resource_view(depth_texture_data._depth_texture_view);
			device->destroy_resource(depth_texture_data._depth_texture);

			_depth_buffers.erase(it);
		}
	}

	void free_buffer_resources(device* device, resource back_buffer_resource) {
		auto it = _back_buffers.find(back_buffer_resource);
		if (it != _back_buffers.end()) {
			it->second.free_buffer_resources(device);
			_back_buffers.erase(it);
		}
	}

	pipeline _copy_pipeline = {};
	pipeline_layout _copy_pipeline_layout = {};
	sampler  _copy_sampler_state = {};

	void on_init_device(device* device) {
		sampler_desc sampler_desc = {};
		sampler_desc.filter = filter_mode::min_mag_mip_point;
		sampler_desc.address_u = texture_address_mode::clamp;
		sampler_desc.address_v = texture_address_mode::clamp;
		sampler_desc.address_w = texture_address_mode::clamp;

		pipeline_layout_param layout_params[2];
		layout_params[0] = descriptor_range{ 0, 0, 0, 1, shader_stage::all, 1, descriptor_type::sampler };
		layout_params[1] = descriptor_range{ 0, 0, 0, 1, shader_stage::all, 1, descriptor_type::shader_resource_view };

		const data_resource vs = load_data_resource(g_hReShadeModule,IDR_FULLSCREEN_VS);
		const data_resource ps = load_data_resource(g_hReShadeModule,IDR_COPY_PS);

		shader_desc vs_desc = { vs.data, vs.data_size };
		shader_desc ps_desc = { ps.data, ps.data_size };

		std::vector<pipeline_subobject> subobjects;
		subobjects.push_back({ pipeline_subobject_type::vertex_shader, 1, &vs_desc });
		subobjects.push_back({ pipeline_subobject_type::pixel_shader, 1, &ps_desc });

		if (!device->create_pipeline_layout(2, layout_params, &_copy_pipeline_layout) ||
			!device->create_pipeline(_copy_pipeline_layout, static_cast<uint32_t>(subobjects.size()), subobjects.data(), &_copy_pipeline) ||
			!device->create_sampler(sampler_desc, &_copy_sampler_state))
		{
			
		}
	}

	void on_destroy_device(device* device) {
		device->destroy_pipeline(_copy_pipeline);
		_copy_pipeline = {};
		device->destroy_pipeline_layout(_copy_pipeline_layout);
		_copy_pipeline_layout = {};
		device->destroy_sampler(_copy_sampler_state);
		_copy_sampler_state = {};
	}

	bool render_effects(device* device, effect_runtime* runtime, resource back_buffer_resource, resource depth_buffer_resource) {
		auto& back_buffer_data = _back_buffers.emplace(std::piecewise_construct, std::forward_as_tuple(back_buffer_resource), std::forward_as_tuple()).first->second;
		auto& depth_buffer_data = _depth_buffers.emplace(std::piecewise_construct, std::forward_as_tuple(depth_buffer_resource), std::forward_as_tuple()).first->second;

		if (!back_buffer_data.ensure_buffers(device, back_buffer_resource))
			return false;

		auto runtime_data = runtime->get_private_data<runtime_data_s>();
		auto old_back_buffer_resource_usage = device->get_resource_desc(back_buffer_resource).usage;

		if (auto command_queue = runtime->get_command_queue()) {
			if (auto command_list = command_queue->get_immediate_command_list()) {

				// Resolve MSAA back buffer if MSAA is active or copy when format conversion is required
				if (back_buffer_data._back_buffer_resolved != 0)
				{
					if (back_buffer_data._back_buffer_samples == 1)
					{
						command_list->barrier(back_buffer_resource, resource_usage::present, resource_usage::copy_source);
						command_list->copy_texture_region(back_buffer_resource, 0, nullptr, back_buffer_data._back_buffer_resolved, 0, nullptr);
						command_list->barrier(back_buffer_data._back_buffer_resolved, resource_usage::copy_dest, resource_usage::render_target);
					}
					else
					{
						command_list->barrier(back_buffer_resource, resource_usage::present, resource_usage::resolve_source);
						command_list->resolve_texture_region(back_buffer_resource, 0, nullptr, back_buffer_data._back_buffer_resolved, 0, 0, 0, 0, back_buffer_data._back_buffer_format);
						command_list->barrier(back_buffer_data._back_buffer_resolved, resource_usage::resolve_dest, resource_usage::render_target);
					}
				}

				if (depth_buffer_data.supply_depth(device, depth_buffer_resource)) {
					if (runtime_data->_current_depth_buffer_resource != depth_buffer_resource || runtime_data->_current_depth_texture_view != depth_buffer_data._depth_texture_view) {
						update_dependent_effect_runtime(runtime, depth_buffer_resource, depth_buffer_data._depth_texture_view);
					}

					command_list->barrier(depth_buffer_resource, resource_usage::depth_stencil, resource_usage::copy_source);
					command_list->copy_texture_region(depth_buffer_resource, 0, nullptr, depth_buffer_data._depth_texture, 0, nullptr);
					command_list->barrier(depth_buffer_data._depth_texture, resource_usage::copy_dest, resource_usage::shader_resource);
				}

				if (back_buffer_data._back_buffer_resolved != 0)
				{
					runtime->render_effects(command_list, back_buffer_data._back_buffer_revoled_targets[0], back_buffer_data._back_buffer_revoled_targets[1]);
				}
				else
				{
					if (old_back_buffer_resource_usage != resource_usage::render_target) command_list->barrier(back_buffer_resource, old_back_buffer_resource_usage, resource_usage::render_target);
					runtime->render_effects(command_list, back_buffer_data._back_buffer_targets[0], back_buffer_data._back_buffer_targets[1]);
					command_list->barrier(back_buffer_resource, resource_usage::render_target, old_back_buffer_resource_usage);
				}

				// Stretch main render target back into MSAA back buffer if MSAA is active or copy when format conversion is required
				if (back_buffer_data._back_buffer_resolved != 0)
				{
					const resource resources[2] = { back_buffer_resource, back_buffer_data._back_buffer_resolved };
					const resource_usage state_old[2] = { resource_usage::copy_source | resource_usage::resolve_source, resource_usage::render_target };
					const resource_usage state_final[2] = { resource_usage::present, resource_usage::resolve_dest };

					if (device->get_api() == device_api::d3d10 ||
						device->get_api() == device_api::d3d11 ||
						device->get_api() == device_api::d3d12)
					{
						const resource_usage state_new[2] = { resource_usage::render_target, resource_usage::shader_resource };

						command_list->barrier(2, resources, state_old, state_new);

						command_list->bind_pipeline(pipeline_stage::all_graphics, _copy_pipeline);

						command_list->push_descriptors(shader_stage::pixel, _copy_pipeline_layout, 0, descriptor_table_update{ {}, 0, 0, 1, descriptor_type::sampler, &_copy_sampler_state });
						command_list->push_descriptors(shader_stage::pixel, _copy_pipeline_layout, 1, descriptor_table_update{ {}, 0, 0, 1, descriptor_type::shader_resource_view, &back_buffer_data._back_buffer_resolved_srv });

						const viewport viewport = { 0.0f, 0.0f, static_cast<float>(back_buffer_data._width), static_cast<float>(back_buffer_data._height), 0.0f, 1.0f };
						command_list->bind_viewports(0, 1, &viewport);
						const rect scissor_rect = { 0, 0, static_cast<int32_t>(back_buffer_data._width), static_cast<int32_t>(back_buffer_data._height) };
						command_list->bind_scissor_rects(0, 1, &scissor_rect);

						const bool srgb_write_enable = (back_buffer_data._back_buffer_format == format::r8g8b8a8_unorm_srgb || back_buffer_data._back_buffer_format == format::b8g8r8a8_unorm_srgb);
						command_list->bind_render_targets_and_depth_stencil(1, &back_buffer_data._back_buffer_targets[srgb_write_enable?1:0]);

						command_list->draw(3, 1, 0, 0);

						command_list->barrier(2, resources, state_new, state_final);
					}
					else
					{
						const resource_usage state_new[2] = { resource_usage::copy_dest, resource_usage::copy_source };

						command_list->barrier(2, resources, state_old, state_new);
						command_list->copy_texture_region(back_buffer_data._back_buffer_resolved, 0, nullptr, back_buffer_resource, 0, nullptr);
						command_list->barrier(2, resources, state_new, state_final);
					}
				}

				return true;
			}
		}

		return false;
	}
};

effect_runtime* g_MainRuntime = 0;

void update_dependent_effect_runtime(effect_runtime* runtime, resource depth_buffer_resource, resource_view rsv) {
	auto data = runtime->get_private_data<runtime_data_s>();
	data->update_effect_runtime(runtime, depth_buffer_resource, rsv);
}

static void update_effect_runtime(effect_runtime* runtime) {
	auto data = runtime->get_private_data<runtime_data_s>();
	data->update_effect_runtime(runtime);
}

extern "C" bool __declspec(dllexport) AdvancedfxRenderEffects(void* pRenderTargetView, void * pDepthTextureResource) {

	if (g_MainRuntime == 0)
		return false;

	if (pRenderTargetView == 0) {
		// HLAE wants us to render no effects.
		g_MainRuntime->set_effects_state(false);
		return true;
	}

	g_MainRuntime->set_effects_state(true);
	auto device = g_MainRuntime->get_device();
	if (device == 0)
		return false;

	auto device_data = device->get_private_data<device_data_s>();
	auto runtime_data = g_MainRuntime->get_private_data<runtime_data_s>();

	resource back_buffer_resource{ (uint64_t)pRenderTargetView };
	resource depth_buffer_resouce{ (uint64_t)pDepthTextureResource };

	runtime_data->block_effects = false;
	auto result = device_data->render_effects(device, g_MainRuntime, back_buffer_resource, depth_buffer_resouce);
	runtime_data->block_effects = true;
	return result;
}

static void on_init_device(device* device) {
	device->create_private_data<device_data_s>();

	auto device_data = device->get_private_data<device_data_s>();
	device_data->on_init_device(device);
}

static void on_destroy_device(device* device) {
	auto device_data = device->get_private_data<device_data_s>();
	device_data->on_destroy_device(device);

	device->destroy_private_data<device_data_s>();
}


static void on_init_effect_runtime(effect_runtime *runtime)
{
	runtime->create_private_data<runtime_data_s>();

	if (auto device = runtime->get_device()) {
		auto device_data = device->get_private_data<device_data_s>();
		device_data->_effect_runtimes.emplace(runtime);
	}
}

static void on_destroy_effect_runtime(effect_runtime *runtime)
{
	if (auto device = runtime->get_device()) {
		auto device_data = device->get_private_data<device_data_s>();
		device_data->_effect_runtimes.erase(runtime);
	}

	auto runtime_data = runtime->get_private_data<runtime_data_s>();
	runtime_data->_current_depth_buffer_resource = { 0 };
	runtime_data->_current_depth_texture_view = { 0 };

	if (runtime == g_MainRuntime) {
		g_MainRuntime = nullptr;
	}
	runtime->destroy_private_data<runtime_data_s>();
}

static void on_destroy_resource(device* device, resource resource) {
	auto device_data = device->get_private_data<device_data_s>();
	device_data->free_depth_resources(device, resource);
	device_data->free_buffer_resources(device, resource);
}

static void on_begin_render_effects(effect_runtime* runtime, command_list* /*cmd_list*/, resource_view, resource_view)
{
	auto data = runtime->get_private_data<runtime_data_s>();
	if (data->block_effects) {
		data->effects_were_enabled = runtime->get_effects_state();
		if (data->effects_were_enabled) {
			runtime->set_effects_state(false);
		}
	}
}

static void on_finish_render_effects(effect_runtime* runtime, command_list* /*cmd_list*/, resource_view, resource_view)
{
	auto data = runtime->get_private_data<runtime_data_s>();

	if (data->block_effects && data->effects_were_enabled) {
			runtime->set_effects_state(true);
	}
}

static bool on_reshade_set_effects_state(effect_runtime* runtime, bool enabled) {	
	return false; // Effects should be toggled with mirv_streams and not with effects key.
}

static void on_reshade_present(effect_runtime* runtime) {
	if (nullptr == g_MainRuntime) g_MainRuntime = runtime;
}


HMODULE get_reshade_module_handle(HMODULE initial_handle = nullptr)
{
	static HMODULE handle = initial_handle;
	if (handle == nullptr)
	{
		HMODULE modules[1024]; DWORD num = 0;
		if (K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &num))
		{
			if (num > sizeof(modules))
				num = sizeof(modules);

			for (DWORD i = 0; i < num / sizeof(HMODULE); ++i)
			{
				if (GetProcAddress(modules[i], "ReShadeRegisterAddon") &&
					GetProcAddress(modules[i], "ReShadeUnregisterAddon"))
				{
					handle = modules[i];
					break;
				}
			}
		}
	}
	return handle;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		g_hReShadeModule = get_reshade_module_handle();

		if (!reshade::register_addon(hModule))
			return FALSE;

		reshade::register_event<reshade::addon_event::init_device>(on_init_device);
		reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
		reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_render_effects);
		reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish_render_effects);
		reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
		reshade::register_event<reshade::addon_event::reshade_set_effects_state>(on_reshade_set_effects_state);
		reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);

		// Need to set texture binding again after reloading
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(update_effect_runtime);
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(hModule);
		g_hReShadeModule = nullptr;
		break;
	}

	return TRUE;
}
