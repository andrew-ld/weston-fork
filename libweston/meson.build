deps_libweston = [
	dep_wayland_server,
	dep_pixman,
	dep_libm,
	dep_libdl,
	dep_libdrm,
	dep_xkbcommon,
	dep_matrix_c,
	dep_egl,
	dep_vulkan,
]
srcs_libweston = [
	git_version_h,
	'animation.c',
	'auth.c',
	'bindings.c',
	'clipboard.c',
	'color.c',
	'color-properties.c',
	'color-management.c',
	'color-noop.c',
	'color-operations.c',
	'color-profile-param-builder.c',
	'compositor.c',
	'content-protection.c',
	'data-device.c',
	'drm-formats.c',
	'id-number-allocator.c',
	'input.c',
	'linux-dmabuf.c',
	'linux-explicit-synchronization.c',
	'linux-sync-file.c',
	'log.c',
	'noop-renderer.c',
	'output-capture.c',
	'pixel-formats.c',
	'pixman-renderer.c',
	'plugin-registry.c',
	'screenshooter.c',
	'timeline.c',
	'touch-calibration.c',
	'weston-log-wayland.c',
	'weston-log-file.c',
	'weston-log-flight-rec.c',
	'weston-log.c',
	'weston-direct-display.c',
	color_management_v1_protocol_c,
	color_management_v1_server_protocol_h,
	linux_dmabuf_unstable_v1_protocol_c,
	linux_dmabuf_unstable_v1_server_protocol_h,
	linux_explicit_synchronization_unstable_v1_protocol_c,
	linux_explicit_synchronization_unstable_v1_server_protocol_h,
	input_method_unstable_v1_protocol_c,
	input_method_unstable_v1_server_protocol_h,
	input_timestamps_unstable_v1_protocol_c,
	input_timestamps_unstable_v1_server_protocol_h,
	presentation_time_protocol_c,
	presentation_time_server_protocol_h,
	pointer_constraints_unstable_v1_protocol_c,
	pointer_constraints_unstable_v1_server_protocol_h,
	relative_pointer_unstable_v1_protocol_c,
	relative_pointer_unstable_v1_server_protocol_h,
	single_pixel_buffer_v1_protocol_c,
	single_pixel_buffer_v1_server_protocol_h,
	tearing_control_v1_protocol_c,
	tearing_control_v1_server_protocol_h,
	text_cursor_position_protocol_c,
	text_cursor_position_server_protocol_h,
	text_input_unstable_v1_protocol_c,
	text_input_unstable_v1_server_protocol_h,
	weston_touch_calibration_protocol_c,
	weston_touch_calibration_server_protocol_h,
	weston_content_protection_protocol_c,
	weston_content_protection_server_protocol_h,
	viewporter_protocol_c,
	viewporter_server_protocol_h,
	xdg_output_unstable_v1_protocol_c,
	xdg_output_unstable_v1_server_protocol_h,
	weston_debug_protocol_c,
	weston_debug_server_protocol_h,
	weston_direct_display_protocol_c,
	weston_direct_display_server_protocol_h,
	weston_output_capture_protocol_c,
	weston_output_capture_server_protocol_h,
	tablet_unstable_v2_protocol_c,
	tablet_unstable_v2_server_protocol_h,
]

subdir('desktop')
subdir('shell-utils')

dep_gbm = dependency('gbm', required: false, version: '>= 21.1.1')
if dep_gbm.found()
	config_h.set('HAVE_GBM', '1')
endif

if get_option('backend-vnc')
	dep_pam = dependency('pam', required: false)
	if not dep_pam.found()
		dep_pam = cc.find_library('pam')
	endif
	if not dep_pam.found()
		error('VNC backend requires libpam which was not found. Or, you can use \'-Dbackend-vnc=false\'.')
	endif
	config_h.set('HAVE_PAM', '1')
	deps_libweston += dep_pam
endif

if get_option('perfetto')
	srcs_libweston += [
		'perfetto/u_perfetto.cc',
		'perfetto/u_perfetto.h',
		'timeline-perfetto.c'
	]
	dep_perfetto = dependency('perfetto', fallback: ['perfetto', 'dep_perfetto'])
	deps_libweston += dep_perfetto
	config_h.set('HAVE_PERFETTO', '1')
endif

lib_weston = shared_library(
	'weston-@0@'.format(libweston_major),
	srcs_libweston,
	include_directories: common_inc,
	install: true,
	cpp_args: '-std=c++17',
	version: '0.0.@0@'.format(libweston_revision),
	link_whole: lib_libshared,
	dependencies: deps_libweston
)


# For external users, like Weston.
dep_libweston_public = declare_dependency(
	link_with: lib_weston,
	include_directories: public_inc,
	dependencies: deps_for_libweston_users
)

# For internal users, like the backends.
dep_libweston_private = declare_dependency(
	link_with: lib_weston,
	include_directories: [ include_directories('.'), public_inc ],
	dependencies: deps_for_libweston_users
)

# XXX: We should be able to use dep_libweston_private.partial_dependency() instead
# of this, but a Meson bug makes it not work. It will be fixed with
# https://github.com/mesonbuild/meson/pull/5167
# in hopefully Meson 0.51.
dep_libweston_private_h_deps = []
foreach d : deps_for_libweston_users
	dep_libweston_private_h_deps += d.partial_dependency(compile_args: true)
endforeach
dep_libweston_private_h = declare_dependency(
	include_directories: [ include_directories('.'), public_inc ],
	dependencies: dep_libweston_private_h_deps
)

pkgconfig.generate(
	lib_weston,
	filebase: 'libweston-@0@'.format(libweston_major),
	name: 'libweston API',
	version: version_weston,
	description: 'Header files for libweston compositors development',
	requires_private: deps_for_libweston_users,
	subdirs: dir_include_libweston
)

pkgconfig.generate(
	filebase: 'libweston-@0@-protocols'.format(libweston_major),
	name: 'libWeston Protocols',
	version: version_weston,
	description: 'libWeston protocol files',
	variables: [
		'datarootdir=' + '${prefix}' / get_option('datadir'),
		'pkgdatadir=' + '${pc_sysrootdir}${datarootdir}' / dir_protocol_libweston
	],
	install_dir: dir_data_pc
)

if get_option('backend-drm')
	dep_libseat = dependency('libseat', version: '>= 0.4')
	srcs_session_helper = [
		'launcher-libseat.c',
		'launcher-util.c',
	]
	deps_session_helper = [
		dep_libseat,
		dep_libweston_private_h,
	]

	lib_session_helper = static_library(
		'session-helper',
		srcs_session_helper,
		include_directories: common_inc,
		dependencies: deps_session_helper,
		install: false
	)
	dep_session_helper = declare_dependency(link_with: lib_session_helper)
endif

lib_libinput_backend = static_library(
	'libinput-backend',
	[
		'libinput-device.c',
		'libinput-seat.c',
		tablet_unstable_v2_server_protocol_h
	],
	dependencies: [
		dep_libweston_private,
		dep_libinput,
		dependency('libudev', version: '>= 136')
	],
	include_directories: common_inc,
	install: false
)
dep_libinput_backend = declare_dependency(
	link_with: lib_libinput_backend,
	include_directories: include_directories('.')
)

dep_vertex_clipping = declare_dependency(
	sources: 'vertex-clipping.c',
	include_directories: include_directories('.')
)

lib_renderer_borders = static_library(
	'renderer-borders',
	'renderer-borders.c',
	include_directories: common_inc,
	dependencies: [
		dep_lib_cairo_shared,
		dep_egl, # for gl-renderer.h
		deps_for_libweston_users,
	],
	build_by_default: false,
	install: false
)
dep_lib_renderer_borders = declare_dependency(
	link_with: lib_renderer_borders,
	dependencies: dep_lib_cairo_shared
)

subdir('color-lcms')
subdir('renderer-gl')
subdir('renderer-vulkan')
subdir('backend-drm')
subdir('backend-headless')
subdir('backend-pipewire')
subdir('backend-rdp')
subdir('backend-vnc')
subdir('backend-wayland')
subdir('backend-x11')
