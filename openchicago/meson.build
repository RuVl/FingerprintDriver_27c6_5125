# openchicago — open ChicagoHS (AlgoChicago.dll profile 12) implementation.
#   meson setup build && ninja -C build && meson test -C build
# The DLL comparison tests (suite "dll") need win-driver/AlgoChicago.dll and
# the exported dataset in tools/algo; the "unit" suite runs everywhere.
project('openchicago', 'c',
  version : '0.1.0',
  license : 'LGPL-2.1-or-later',
  default_options : ['c_std=gnu11', 'warning_level=1', 'buildtype=debugoptimized'])

cc = meson.get_compiler('c')
glib_dep = dependency('glib-2.0')
gio_dep = dependency('gio-2.0')
m_dep = cc.find_library('m', required : false)

inc = include_directories('include', 'src')

openchicago_sources = files(
  'src/openchicago.c',
  'src/goodix-chicago-calibration.c',
  'src/goodix-chicago-enrollment.c',
  'src/goodix-chicago-feature.c',
  'src/goodix-chicago-late-rejection.c',
  'src/goodix-chicago-match.c',
  'src/goodix-chicago-preprocess.c',
  'src/goodix-chicago-runtime.c',
  'src/goodix-chicago-template.c',
)

openchicago_lib = static_library('openchicago', openchicago_sources,
  include_directories : inc,
  dependencies : [glib_dep, gio_dep, m_dep],
  c_args : ['-Wno-unused-function'],
  pic : true)

openchicago_dep = declare_dependency(
  link_with : openchicago_lib,
  include_directories : include_directories('include'),
  dependencies : [glib_dep, gio_dep, m_dep])

subdir('tests')
