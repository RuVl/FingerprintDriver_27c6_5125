algo_dir = meson.project_source_root() / '..' / 'tools' / 'algo'

test_e2e = executable('test_e2e', 'test_e2e.c',
  include_directories : include_directories('../src'),
  dependencies : openchicago_dep)

test_state = executable('test_state', 'test_state.c',
  include_directories : include_directories('../src'),
  dependencies : openchicago_dep)

test_api = executable('test_api', 'test_api.c',
  include_directories : include_directories('../src'),
  dependencies : openchicago_dep)

# stage-1 comparison against the DLL oracle (internal preprocessor API)
test_preprocess = executable('test_preprocess', 'test_preprocess.c',
  include_directories : include_directories('../src'),
  link_with : openchicago_lib,
  dependencies : [glib_dep, gio_dep, m_dep])

# unit suite: no DLL needed (skips with 77 without the exported dataset)
test('state', test_state, args : [algo_dir], suite : 'unit', timeout : 300)
test('api', test_api, args : [algo_dir], suite : 'unit', timeout : 300)
test('corrupt-state', test_state, args : [algo_dir, '--negative'], suite : 'unit',
  should_fail : true, timeout : 300)

# dll suite: needs win-driver/AlgoChicago.dll (oracle) and the dataset
test('stage1', find_program('run.sh'), suite : 'dll', timeout : 1800)
test('e2e', find_program('e2e.sh'), args : [test_e2e.full_path()], suite : 'dll',
  depends : test_e2e, timeout : 3600)
