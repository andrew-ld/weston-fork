# you need to add here any files you add to the toc directory as well
files = [
	'compositor.rst',
	'head.rst',
        'debug-flight-recorder.rst',
	'log.rst',
	'output.rst',
	'output-management.rst',
        'weston-config.rst',
        'shell-utils.rst',
]

foreach file : files
	configure_file(input: file, output: file, copy: true)
endforeach

subdir('images')
