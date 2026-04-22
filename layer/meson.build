# Vulkan post-processing layer.
# Forked from vkBasalt_overlay (https://github.com/Boux/vkBasalt_overlay),
# which is itself a fork of vkBasalt by Georg Lehmann. Both are Zlib licensed.
# The original copyright notice is preserved in LICENSE-UPSTREAM.
#
# We inherit the Vulkan hooking, shader pipeline, and ImGui integration.
# We replace: visual design (theme, layout, views), branding (layer/lib/env-var
# names), and loader model (removed enable_environment so we load implicitly).

project(
  'lumen-layer',
  ['c', 'cpp'],
  version: '0.1.0',
  license: 'Zlib',
  meson_version: '>= 1.1.0',
  default_options: [
    'c_std=c11',
    'cpp_std=c++2a',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

cpp = meson.get_compiler('cpp')

layer_lib_name = 'lumen'
layer_include_path = include_directories('src')
layer_lib_dir = get_option('libdir')

if get_option('with_so')
  subdir('src')
endif

if get_option('with_json')
  subdir('config')
endif
