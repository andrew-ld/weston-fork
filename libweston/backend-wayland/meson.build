if not get_option('backend-wayland')
	subdir_done()
endif

config_h.set('BUILD_WAYLAND_COMPOSITOR', '1')

srcs_wlwl = [
	'wayland.c',
	presentation_time_protocol_c,
	presentation_time_server_protocol_h,
	presentation_time_client_protocol_h,
	xdg_shell_client_protocol_h,
	xdg_shell_protocol_c,
	viewporter_protocol_c,
	relative_pointer_unstable_v1_client_protocol_h,
	relative_pointer_unstable_v1_protocol_c,
	pointer_constraints_unstable_v1_client_protocol_h,
	pointer_constraints_unstable_v1_protocol_c,
	tearing_control_v1_client_protocol_h,
	tearing_control_v1_protocol_c,
	xdg_decoration_unstable_v1_client_protocol_h,
	xdg_decoration_unstable_v1_protocol_c,
	linux_dmabuf_unstable_v1_protocol_c,
	linux_dmabuf_unstable_v1_client_protocol_h,
]

deps_wlwl = [
	dependency('wayland-client'),
	dependency('wayland-cursor'),
	dep_pixman,
	dep_libweston_private,
	dep_libdrm_headers,
	dep_lib_cairo_shared,
	dep_lib_renderer_borders,
	dep_libdrm
]

if get_option('renderer-gl')
	d = dependency('wayland-egl', required: false)
	if not d.found()
		error('wayland-backend + gl-renderer requires wayland-egl which was not found. Or, you can use \'-Dbackend-wayland=false\' or \'-Drenderer-gl=false\'.')
	endif
	deps_wlwl += d
	deps_wlwl += dep_egl # for gl-renderer.h
endif

if get_option('renderer-vulkan')
	deps_wlwl += dep_vulkan # for vulkan-renderer.h
endif

plugin_wlwl = shared_library(
	'wayland-backend',
	srcs_wlwl,
	include_directories: common_inc,
	dependencies: deps_wlwl,
	name_prefix: '',
	install: true,
	install_dir: dir_module_libweston
)
env_modmap += 'wayland-backend.so=@0@;'.format(plugin_wlwl.full_path())
install_headers(backend_wayland_h, subdir: dir_include_libweston_install)
