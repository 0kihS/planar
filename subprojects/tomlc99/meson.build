project('tomlc99', 'c')

libtoml = static_library(
	'toml',
	'toml.c',
	install: false,
)

toml_dep = declare_dependency(
	link_with: libtoml,
	include_directories: include_directories('.'),
)
