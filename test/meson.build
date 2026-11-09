# Scripted sequences against a FakeDevice: the state machine, slot ownership
# and publish rules of src/iris without hardware (docs/architecture.md §9).
session_test = executable(
  'session-test',
  ['unit/session-test.cc', 'unit/fake_device.cc'],
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('session', session_test)

# Annex-B reassembly for a requested IDR (src/codec/annexb.cc).
annexb_test = executable(
  'annexb-test',
  'annexb-test.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('annexb', annexb_test)

# Hardware probe, built but not registered as a meson test: it needs a
# decode-order Iris node and arguments. Proves by content that picture k
# lands in the k-th queued client DMA-BUF; the acceptance test
# docs/architecture.md §9 requires for the Import policy.
executable(
  'iris-import-probe',
  'iris-import-probe.cc',
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])

# Hardware probe, built but not registered as a meson test: a VA client that
# hands the driver its own DMA-BUF. Proves the public API path (import in the
# CAPTURE layout, aliasing, a firmware encode reading the client's pixels),
# which no unit test can. Needs libva's DRM display.
va_drm_dep = dependency('libva-drm', required: false)
if va_drm_dep.found()
  executable(
    'iris-va-import-probe',
    'iris-va-import-probe.cc',
    dependencies: [iris_core_dep, va_drm_dep],
    cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
endif

# Hardware probe, built but not registered as a meson test: it needs the
# encoder node. Answers whether the loop-filter controls exist and whether a
# deblocking request the firmware cannot honour is refused, which is the §10
# invariant 13 gate a scripted node cannot give a verdict on.
executable(
  'iris-encode-control-probe',
  'iris-encode-control-probe.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])

# Hardware probe, built but not registered as a meson test: the stable-buffer
# allocation chain (MSM cached-coherent, MSM write-combined, system DMA heap).
# Every rung needs an MSM render node, and the fallthroughs are only reached
# when the earlier ones are refused, so the probe runs under the fault shim
# below. Covers docs/architecture.md §3.4.
executable(
  'iris-alloc-probe',
  'iris-alloc-probe.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])

# The shim the probe loads with LD_PRELOAD: it refuses MSM GEM allocations so
# the driver's retry and fallthrough run on real hardware.
shared_module(
  'alloc-fault',
  'alloc-fault.c',
  dependencies: [libdrm_dep],
  c_args: ['-Wall', '-Wextra'],
  name_prefix: '')

# Hardware probe, built but not registered as a meson test: whether a GPU job
# leaves a pending fence on its destination buffer right after glFlush(). That
# is the window the encode input's fence wait covers; if it is not observable
# on this GPU, no probe can claim the wait mattered.
if have_gpu_copy
  executable(
    'gpu-fence-check',
    'gpu-fence-check.cc',
    dependencies: [iris_core_dep, egl_dep, glesv2_dep, gbm_dep],
    cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
endif

# Hardware probe, built but not registered as a meson test: whether the
# encoder waits for an input surface's pending GPU write before handing it to
# the firmware. Needs the encoder node, an MSM render node and a real GPU
# driver, and compares access units, so only a target run can judge it.
# docs/architecture.md §10 invariant 11.
if have_gpu_copy
  executable(
    'iris-encode-fence-probe',
    'iris-encode-fence-probe.cc',
    dependencies: [iris_core_dep, egl_dep, glesv2_dep, gbm_dep],
    cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
endif

# Hardware probe, built but not registered as a meson test: whether a VA
# client's descriptor count returns to its starting value across the surface
# lifecycle it repeats. A leak of one fd per surface is invisible in a short
# run and fatal in a long one, and the count comes from /proc/self/fd rather
# than from the driver's own ledger.
if va_drm_dep.found()
  executable(
    'iris-fd-leak-probe',
    'iris-fd-leak-probe.cc',
    dependencies: [iris_core_dep, va_drm_dep],
    cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
endif

# Client memory (DRM_PRIME_2 / PRIME_1) accepted only in the CAPTURE layout,
# and the one definition of that layout. No hardware.
prime_import_test = executable(
  'prime-import-test',
  'prime-import-test.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('prime-import', prime_import_test)

# Parameter-set bitstream round trips for the HEVC translator.
hevc_parameter_sets_test = executable(
  'hevc-parameter-sets-test',
  'hevc-parameter-sets-test.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('hevc-parameter-sets', hevc_parameter_sets_test)

# The rebuilt AV1 frame header walked back with a spec-ordered reader: the
# fields whose coding is not a plain copy of the VA value. No hardware.
av1_obu_test = executable(
  'av1-obu-test',
  'av1-obu-test.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('av1-obu', av1_obu_test)

# VA encode parameters -> V4L2 controls, both codecs, no hardware.
encode_params_test = executable(
  'encode-params-test',
  'encode-params-test.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('encode-params', encode_params_test)

# The encoder session's control contract on a scripted Iris encoder node:
# which V4L2 writes a VA deblocking request produces, and what happens on a
# kernel that has no loop-filter controls at all.
encode_session_test = executable(
  'encode-session-test',
  'unit/encode-session-test.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('encode-session', encode_session_test)

# Hardware-independent regression guards for the browser-facing queue and
# stable-export invariants. The integration probe above remains opt-in because
# hosts without /dev/video nodes cannot execute it.
#
# run-static-checks.sh is the same entry point CI uses, so `meson test` and CI
# cannot drift apart. The individual checks stay registered as well to keep
# failures attributable without reading the aggregate output.
test('static-checks', find_program('run-static-checks.sh'),
     args: [meson.project_source_root()])
test('iris-source-invariants', find_program('iris-source-invariants.sh'),
     args: [meson.project_source_root()])
test('iris-env-doc-check', find_program('iris-env-doc-check.sh'),
     args: [meson.project_source_root()])
test('iris-codec-matrix-check', find_program('iris-codec-matrix-check.sh'),
     args: [meson.project_source_root()])
test('iris-experiment-guard-self-test', find_program('iris-experiment-guard-self-test.sh'),
     args: [meson.project_source_root()])
test('iris-eos-check-self-test', find_program('iris-eos-check-self-test.sh'),
     args: [meson.project_source_root()])
test('iris-deploy-path-check', find_program('iris-deploy-path-check.sh'),
     args: [meson.project_source_root()])
test('iris-setup-path-check', find_program('iris-setup-path-check.sh'),
     args: [meson.project_source_root()])
test('iris-packaging-check', find_program('iris-packaging-check.sh'),
     args: [meson.project_source_root()])
test('iris-kernel-patch-check', find_program('iris-kernel-patch-check.sh'),
     args: [meson.project_source_root()])
test('iris-install-manifest-check', find_program('iris-install-manifest-check.sh'),
     args: [meson.project_source_root()])
test('provenance-self-test', find_program('provenance-self-test.sh'),
     args: [meson.project_source_root()])
test('iris-node-check', find_program('iris-node-check.sh'),
     args: [meson.project_source_root()])
