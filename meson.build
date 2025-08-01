project('weston',
	['c', 'cpp'],
	version: '14.0.90',
	default_options: [
		'warning_level=3',
		'c_std=gnu11',
		'b_lundef=true',
	],
	meson_version: '>= 0.63.0',
	license: 'MIT/Expat',
)

libweston_major = 15

# libweston_revision is manufactured to follow the autotools build's
# library file naming, thanks to libtool
version_weston = meson.project_version()
version_weston_arr = version_weston.split('.')
if libweston_major > version_weston_arr[0].to_int()
	if libweston_major > version_weston_arr[0].to_int() + 1
		error('Bad versions in meson.build: libweston_major is too high')
	endif
	libweston_revision = 0
elif libweston_major == version_weston_arr[0].to_int()
	libweston_revision = version_weston_arr[2].to_int()
else
	error('Bad versions in meson.build: libweston_major is too low')
endif

dir_prefix = get_option('prefix')
dir_bin = dir_prefix / get_option('bindir')
dir_data = dir_prefix / get_option('datadir')
dir_include_libweston = 'libweston-@0@'.format(libweston_major)
dir_include_libweston_install = dir_include_libweston / 'libweston'
dir_lib = dir_prefix /get_option('libdir')
dir_libexec = dir_prefix / get_option('libexecdir')
dir_module_weston = dir_lib / 'weston'
dir_module_libweston = dir_lib / 'libweston-@0@'.format(libweston_major)
dir_data_pc = dir_data / 'pkgconfig'
dir_lib_pc = dir_lib / 'pkgconfig'
dir_man = dir_prefix / get_option('mandir')
dir_protocol_libweston = 'libweston-@0@'.format(libweston_major) / 'protocols'
dir_sysconf = dir_prefix / get_option('sysconfdir')
dir_gitlab_ci = meson.project_source_root() / '.gitlab-ci'

public_inc = include_directories('include')
common_inc = [ include_directories('.'), public_inc ]

pkgconfig = import('pkgconfig')

git_version_h = vcs_tag(
	input: 'libweston/git-version.h.meson',
	output: 'git-version.h',
	fallback: version_weston
)

config_h = configuration_data()

cc = meson.get_compiler('c')

global_args = cc.get_supported_arguments(
	'-Wmissing-prototypes',
	'-Wno-unused-parameter',
	'-Wno-shift-negative-value', # required due to Pixman
	'-Wno-missing-field-initializers',
	'-Wno-pedantic',
	'-Wundef',
	'-fvisibility=hidden',
)
add_project_arguments(global_args, language: 'c')

if cc.has_header_symbol('sys/sysmacros.h', 'major')
	config_h.set('MAJOR_IN_SYSMACROS', 1)
elif cc.has_header_symbol('sys/mkdev.h', 'major')
	config_h.set('MAJOR_IN_MKDEV', 1)
endif

add_project_arguments(global_args, language: 'c')

cpp = meson.get_compiler('cpp')

global_cpp_args = cpp.get_supported_arguments(
	'-Wno-unused-parameter',
)

add_project_arguments(global_cpp_args, language: 'cpp')

optional_libc_funcs = [
	'mkostemp', 'strchrnul', 'initgroups', 'posix_fallocate',
	'memfd_create', 'unreachable',
]
foreach func : optional_libc_funcs
	if cc.has_function(func)
		config_h.set('HAVE_' + func.to_upper(), 1)
	endif
endforeach

optional_system_headers = [
	'linux/sync_file.h'
]
foreach hdr : optional_system_headers
	if cc.has_header(hdr)
		config_h.set('HAVE_' + hdr.underscorify().to_upper(), 1)
	endif
endforeach

optional_builtins = {
	'builtin_clz': 'return __builtin_clz(1);',
	'builtin_bswap32': 'return __builtin_bswap32(0);',
	'builtin_popcount': 'return __builtin_popcount(0);',
}
foreach name, check : optional_builtins
	if cc.links('int main(void) { @0@ }'.format(check), name: name)
		config_h.set('HAVE_' + name.to_upper(), 1)
	endif
endforeach

env_modmap = ''

config_h.set('_GNU_SOURCE', '1')
config_h.set('_ALL_SOURCE', '1')
config_h.set('EGL_NO_X11', '1')
config_h.set('MESA_EGL_NO_X11_HEADERS', '1')
config_h.set('EGL_NO_PLATFORM_SPECIFIC_TYPES', '1')

config_h.set('VULKAN_NO_X11', '1')
config_h.set('MESA_VULKAN_NO_X11_HEADERS', '1')
config_h.set('VULKAN_NO_PLATFORM_SPECIFIC_TYPES', '1')

config_h.set_quoted('PACKAGE_STRING', 'weston @0@'.format(version_weston))
config_h.set_quoted('PACKAGE_VERSION', version_weston)
config_h.set_quoted('VERSION', version_weston)
config_h.set_quoted('PACKAGE_URL', 'https://wayland.freedesktop.org')
config_h.set_quoted('PACKAGE_BUGREPORT', 'https://gitlab.freedesktop.org/wayland/weston/issues/')

config_h.set_quoted('BINDIR', dir_bin)
config_h.set_quoted('DATADIR', dir_data)
config_h.set_quoted('LIBEXECDIR', dir_libexec)
config_h.set_quoted('MODULEDIR', dir_module_weston)
config_h.set_quoted('LIBWESTON_MODULEDIR', dir_module_libweston)

backend_default = get_option('backend-default')
if backend_default == 'auto'
	foreach b : [ 'headless', 'x11', 'wayland', 'drm' ]
		if get_option('backend-' + b)
			backend_default = b
		endif
	endforeach
endif
config_h.set_quoted('WESTON_NATIVE_BACKEND', backend_default)
message('The default backend is ' + backend_default)
if not get_option('backend-' + backend_default)
	error('Backend @0@ was chosen as native but is not being built.'.format(backend_default))
endif

dep_wayland_server = dependency('wayland-server', version: '>= 1.22.0')
dep_wayland_client = dependency('wayland-client', version: '>= 1.22.0')
dep_pixman = dependency('pixman-1', version: '>= 0.25.2')
dep_xkbcommon = dependency('xkbcommon', version: '>= 0.5.0')
dep_libinput = dependency('libinput', version: '>= 1.2.0')
if dep_xkbcommon.version().version_compare('>= 1.8.0')
	if dep_libinput.version().version_compare('>= 1.26.0')
		config_h.set('HAVE_COMPOSE_AND_KANA', '1')
	endif
endif
have_libinput_axis_v120 = cc.has_function('libinput_event_pointer_get_scroll_value_v120',
                                          dependencies: dep_libinput)
config_h.set10('HAVE_LIBINPUT_AXIS_V120', have_libinput_axis_v120)

dep_libevdev = dependency('libevdev')
dep_libm = cc.find_library('m')
dep_libdl = cc.find_library('dl')
dep_libdrm = dependency('libdrm', version: '>= 2.4.108')
dep_libdrm_headers = dep_libdrm.partial_dependency(compile_args: true)
dep_threads = dependency('threads')

dep_lcms2 = dependency('lcms2', version: '>= 2.9', required: false)
if dep_lcms2.found()
	config_h.set('HAVE_LCMS', '1')
endif

dep_libdisplay_info = dependency(
	'libdisplay-info',
	version: ['>= 0.2.0', '< 0.3.0'],
	fallback: ['display-info', 'di_dep'],
	default_options: [
		'werror=false',
	],
	required: true,
)

prog_python = import('python').find_installation('python3')
files_xxd_py = files('tools/xxd.py')
cmd_xxd = [ prog_python, files_xxd_py, '@INPUT@', '@OUTPUT@' ]

deps_for_libweston_users = [
	dep_wayland_server,
	dep_pixman,
	dep_xkbcommon,
]

if get_option('renderer-gl')
	dep_egl = dependency('egl', required: false)
	if not dep_egl.found()
		error('libweston + gl-renderer requires egl which was not found. Or, you can use \'-Drenderer-gl=false\'.')
	endif
else
	dep_egl = dependency('', required: false)
endif

if get_option('renderer-vulkan')
	dep_vulkan = dependency('vulkan', required: false)
	if not dep_vulkan.found()
		error('libweston + vulkan-renderer requires vulkan which was not found. Or, you can use \'-Drenderer-vulkan=false\'.')
	endif

	prog_glslang = find_program('glslangValidator', required : false)
	if not prog_glslang.found()
		error('libweston + vulkan-renderer requires glslangValidator which was not found. Or, you can use \'-Drenderer-vulkan=false\'.')
	endif
else
	dep_vulkan = dependency('', required: false)
	prog_glslang = find_program('', required : false)
endif


subdir('include')
subdir('protocol')
subdir('shared')
subdir('libweston')
subdir('xwayland')
subdir('frontend')
subdir('desktop-shell')
subdir('fullscreen-shell')
subdir('ivi-shell')
subdir('kiosk-shell')
subdir('lua-shell')
subdir('remoting')
subdir('pipewire')
subdir('clients')
subdir('wcap')
if get_option('tests')
	subdir('tests')
endif
subdir('data')
subdir('man')
subdir('pam')

if meson.version().version_compare('>= 0.58.0')
	devenv = environment()
	devenv.set('WESTON_MODULE_MAP', env_modmap)
	devenv.set('WESTON_DATA_DIR', meson.current_source_dir() / 'data')
	meson.add_devenv(devenv)
endif

configure_file(output: 'config.h', configuration: config_h)

if get_option('doc')
	subdir('doc/sphinx')
else
	message('Documentation will not be built. Use -Ddoc to build it.')
endif
