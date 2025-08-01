dep_scanner = dependency('wayland-scanner', native: true)
prog_scanner = find_program(dep_scanner.get_variable(pkgconfig: 'wayland_scanner'))

dep_wp = dependency('wayland-protocols', version: '>= 1.41',
	fallback: ['wayland-protocols', 'wayland_protocols'])
dir_wp_base = dep_wp.get_variable(pkgconfig: 'pkgdatadir', internal: 'pkgdatadir')

install_data(
	[
		'weston-content-protection.xml',
		'weston-debug.xml',
		'weston-direct-display.xml',
		'weston-output-capture.xml',
	],
	install_dir: join_paths(dir_data, dir_protocol_libweston)
)

generated_protocols = [
	[ 'color-management', 'staging', 'v1' ],
	[ 'fullscreen-shell', 'unstable', 'v1' ],
	[ 'fractional-scale', 'staging', 'v1' ],
	[ 'input-method', 'unstable', 'v1' ],
	[ 'input-timestamps', 'unstable', 'v1' ],
	[ 'ivi-application', 'internal' ],
	[ 'ivi-hmi-controller', 'internal' ],
	[ 'linux-dmabuf', 'unstable', 'v1' ],
	[ 'linux-explicit-synchronization', 'unstable', 'v1' ],
	[ 'presentation-time', 'stable' ],
	[ 'pointer-constraints', 'unstable', 'v1' ],
	[ 'relative-pointer', 'unstable', 'v1' ],
	[ 'single-pixel-buffer', 'staging', 'v1' ],
	[ 'xdg-decoration', 'unstable', 'v1' ],
	[ 'tablet', 'unstable', 'v2' ],
	[ 'tearing-control', 'staging', 'v1' ],
	[ 'text-cursor-position', 'internal' ],
	[ 'text-input', 'unstable', 'v1' ],
	[ 'viewporter', 'stable' ],
	[ 'weston-debug', 'internal' ],
	[ 'weston-desktop-shell', 'internal' ],
	[ 'weston-output-capture', 'internal' ],
	[ 'weston-content-protection', 'internal' ],
	[ 'weston-test', 'internal' ],
	[ 'weston-touch-calibration', 'internal' ],
	[ 'weston-direct-display', 'internal' ],
	[ 'xdg-output', 'unstable', 'v1' ],
	[ 'xdg-shell', 'unstable', 'v6' ],
	[ 'xdg-shell', 'stable' ],
	[ 'xwayland-shell', 'staging', 'v1' ],
	[ 'linux-drm-syncobj', 'staging', 'v1' ],
]

foreach proto: generated_protocols
	proto_name = proto[0]
	if proto[1] == 'internal'
		base_file = proto_name
		xml_path = proto_name + '.xml'
	elif proto[1] == 'stable'
		base_file = proto_name
		xml_path = dir_wp_base / 'stable' / base_file / (base_file + '.xml')
	elif proto[1] == 'unstable'
		base_file = '@0@-unstable-@1@'.format(proto_name, proto[2])
		xml_path = dir_wp_base / 'unstable' / proto_name / (base_file + '.xml')
	elif proto[1] == 'staging'
		base_file = '@0@-@1@'.format(proto_name, proto[2])
		xml_path = dir_wp_base / 'staging' / proto_name / (base_file + '.xml')
	endif

	foreach output_type: [ 'client-header', 'server-header', 'private-code' ]
		if output_type == 'client-header'
			output_file = '@0@-client-protocol.h'.format(base_file)
		elif output_type == 'server-header'
			output_file = '@0@-server-protocol.h'.format(base_file)
		else
			output_file = '@0@-protocol.c'.format(base_file)
			if dep_scanner.version().version_compare('< 1.14.91')
				output_type = 'code'
			endif
		endif

		var_name = output_file.underscorify()
		target = custom_target(
			'@0@ @1@'.format(base_file, output_type),
			command: [ prog_scanner, output_type, '@INPUT@', '@OUTPUT@' ],
			input: xml_path,
			output: output_file,
		)

		set_variable(var_name, target)
	endforeach
endforeach
