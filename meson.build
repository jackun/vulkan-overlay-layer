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

project('vulkan-overlay-layer',
  ['c', 'cpp'],
  version : 'v0.0.1',
  license : 'MIT',
  meson_version : '>= 0.46',
  default_options : ['buildtype=debugoptimized', 'b_ndebug=if-release', 'c_std=c99', 'cpp_std=c++17']
)

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')
prog_python = import('python').find_installation('python3')

null_dep = dependency('', required : false)

pre_args = [
  '-D__STDC_CONSTANT_MACROS',
  '-D__STDC_FORMAT_MACROS',
  '-D__STDC_LIMIT_MACROS',
  '-DPACKAGE_VERSION="@0@"'.format(meson.project_version()),
]

inc_common = [include_directories('.'),
  include_directories('external/glm')]

# Define DEBUG for debug builds only (debugoptimized is not included on this one)
if get_option('buildtype') == 'debug'
  pre_args += '-DDEBUG'
endif

# TODO: this is very incomplete
if ['linux', 'cygwin', 'gnu'].contains(host_machine.system())
  pre_args += '-D_GNU_SOURCE'
endif

# Check for generic C arguments
c_args = []
foreach a : ['-Werror=implicit-function-declaration',
             '-Werror=missing-prototypes', '-Werror=return-type',
             '-Werror=incompatible-pointer-types',
             '-fno-math-errno',
             '-fno-trapping-math', '-Qunused-arguments']
  if cc.has_argument(a)
    c_args += a
  endif
endforeach

foreach a : ['missing-field-initializers', 'format-truncation']
  if cc.has_argument('-W' + a)
    c_args += '-Wno-' + a
  endif
endforeach

c_vis_args = []
if cc.has_argument('-fvisibility=hidden')
  c_vis_args += '-fvisibility=hidden'
endif

# Check for generic C++ arguments
cpp_args = []
foreach a : ['-Werror=return-type',
             '-fno-math-errno', '-fno-trapping-math',
             '-Qunused-arguments']
  if cpp.has_argument(a)
    cpp_args += a
  endif
endforeach

# For some reason, the test for -Wno-foo always succeeds with gcc, even if the
# option is not supported. Hence, check for -Wfoo instead.

foreach a : ['non-virtual-dtor', 'missing-field-initializers', 'format-truncation']
  if cpp.has_argument('-W' + a)
    cpp_args += '-Wno-' + a
  endif
endforeach

no_override_init_args = []
foreach a : ['override-init', 'initializer-overrides']
  if cc.has_argument('-W' + a)
    no_override_init_args += '-Wno-' + a
  endif
endforeach

cpp_vis_args = []
if cpp.has_argument('-fvisibility=hidden')
  cpp_vis_args += '-fvisibility=hidden'
endif

foreach a : pre_args
  add_project_arguments(a, language : ['c', 'cpp'])
endforeach
foreach a : c_args
  add_project_arguments(a, language : ['c'])
endforeach
foreach a : cpp_args
  add_project_arguments(a, language : ['cpp'])
endforeach

# check for dl support
if cc.has_function('dlopen')
  dep_dl = null_dep
else
  dep_dl = cc.find_library('dl')
endif

subdir('src')
