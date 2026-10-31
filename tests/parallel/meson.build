# Run in parallel, then join (wbx_parallel_for): the same machine whatever the
# number of workers. Requires the guest toolchain (guarded in ../meson.build).

guest_parallel_wbx = custom_target('guest_parallel.wbx',
  input: 'guest_parallel.c',
  depend_files: files('../../source/guest/include/waterbox_parallel.h'),
  output: 'guest_parallel.wbx',
  command: [
    waterbox_musl_gcc, waterbox_guest_cflags, waterbox_guest_ldflags,
    '-std=gnu11', '-o', '@OUTPUT@', '@INPUT@', waterbox_emulibc_obj, '-lgcc',
  ],
)

run_parallel = executable('run_parallel', 'run_parallel.c',
  dependencies: [minibox_host_dep, dependency('threads')],
  install: false,
)

test('parallel', run_parallel,
  args: [guest_parallel_wbx],
  suite: 'system',
  timeout: 300,
)
