project(
	'planar',
	'c',
	version: '0.1.0',
	license: 'MIT',
	meson_version: '>=0.60.0',
	default_options: [
		'c_std=c11',
		'warning_level=2',
		'werror=true'
	],
)

add_project_arguments(
	'-DWLR_USE_UNSTABLE',
	'-D_POSIX_C_SOURCE=200809L',
	language: 'c',
)

cc = meson.get_compiler('c')

# Dependencies
wlroots = dependency('wlroots-0.19')
wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')
math = cc.find_library('m')

# Subproject: scenefx (drop-in wlr_scene replacement with effects)
scenefx_proj = subproject('scenefx')
scenefx = scenefx_proj.get_variable('scenefx')

# Subproject: tomlc99
toml_dep = dependency('tomlc99', fallback: ['tomlc99', 'toml_dep'])

# Protocols
wayland_protocols = dependency('wayland-protocols')
wayland_scanner = dependency('wayland-scanner', native: true)

wayland_protocols_dir = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir')
wayland_scanner_bin = wayland_scanner.get_variable(pkgconfig: 'wayland_scanner')

protocols_server = [
	wayland_protocols_dir / 'stable/xdg-shell/xdg-shell.xml',
	'protocols/wlr-layer-shell-unstable-v1.xml',
]

protocols_server_src = []
protocols_server_inc = []

foreach protocol : protocols_server
	protocols_server_src += custom_target(
		protocol.underscorify() + '_server_c',
		input: protocol,
		output: '@BASENAME@-protocol.c',
		command: [wayland_scanner_bin, 'private-code', '@INPUT@', '@OUTPUT@'],
	)

	protocols_server_inc += custom_target(
		protocol.underscorify() + '_server_h',
		input: protocol,
		output: '@BASENAME@-protocol.h',
		command: [wayland_scanner_bin, 'server-header', '@INPUT@', '@OUTPUT@'],
	)
endforeach

protocols_server_lib = static_library(
  'protocols_server',
  [protocols_server_src, protocols_server_inc],
)

protocols_server_dep = declare_dependency(
  link_with: protocols_server_lib,
  sources: protocols_server_inc,
)

# Source files
src_files = files(
	'src/config.c',
	'src/cursor.c',
	'src/decoration.c',
	'src/group.c',
	'src/input.c',
	'src/ipc.c',
	'src/keyboard.c',
	'src/layer-shell.c',
	'src/output.c',
	'src/planar.c',
	'src/popup.c',
	'src/seat.c',
	'src/selection.c',
	'src/server.c',
	'src/snap.c',
	'src/toplevel.c',
	'src/workspaces.c',
)

executable(
	'planar',
	src_files,
	dependencies: [
		scenefx,
		wlroots,
		wayland_server,
		xkbcommon,
		math,
		toml_dep,
		protocols_server_dep
	],
	include_directories: include_directories('include'),
	install: true,
)

# planarctl CLI tool
executable(
	'planarctl',
	'planarctl/planarctl.c',
	install: true,
)
