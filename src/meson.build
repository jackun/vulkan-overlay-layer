# Copyright © 2019 Intel Corporation

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

glslang = find_program('glslangValidator')

overlay_shaders = [
  'overlay.frag',
  'overlay.vert',
]
overlay_spv = []
foreach s : ['overlay.frag', 'overlay.vert']
  overlay_spv += custom_target(
    s + '.spv.h', input : s, output : s + '.spv.h',
    command : [glslang, '-V', '-x', '-o', '@OUTPUT@', '@INPUT@'])
endforeach

# Dispatch structs don't seem to be in vk_layer.h anymore, if they ever where.
# For now, generate them from vk.xml. Probably will use a custom small structs
# when i figure out what is even needed.
vk_layer_table_helpers = []
foreach s : ['vk_dispatch_table_helper.h', 'vk_layer_dispatch_table.h']#, 'vk_loader_extensions.h', 'vk_loader_extensions.c']
  vk_layer_table_helpers += custom_target(
    s, output : s,
    command : [prog_python,
      join_paths(meson.source_root(), 'external/Vulkan-Loader/scripts/loader_genvk.py'),
      '-scripts', '../../Vulkan-Docs/scripts', # relative to loader_genvk.py
      '-registry', join_paths(meson.source_root(), 'external/Vulkan-Docs/xml/vk.xml'),
      '-o','@OUTDIR@', s])
endforeach

vklayer_files = files(
  'layer.cpp',
  'overlay.cpp',
  'stats.cpp',
  'vks/VulkanTools.cpp',
)

vklayer_mesa_overlay = shared_library(
  'VkLayer_NUUDEL_overlay',
  vklayer_files,
  overlay_spv,
  vk_layer_table_helpers,
  c_args : [c_vis_args, no_override_init_args],
  cpp_args : [cpp_vis_args],
  dependencies : [
    dep_dl
  ],
  include_directories : [
    inc_common
  ],
  link_args : cc.get_supported_link_arguments(['-Wl,-Bsymbolic-functions', '-Wl,-z,relro']),
  link_with : [
  ],
  install : true
)

install_data(
  files('VkLayer_NUUDEL_overlay.json'),
  install_dir : join_paths(get_option('datadir'), 'vulkan', 'explicit_layer.d'),
)
