# Scripted sequences against a FakeDevice: the state machine, slot ownership
# and publish rules of src/iris without hardware (docs/architecture.md §9).
session_test = executable(
  'session-test',
  ['unit/session-test.cc', 'unit/fake_device.cc'],
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('session', session_test)

# The copy engine is built on the first copy, never by vaCreateContext.
deferred_copy_test = executable(
  'deferred-copy-test',
  ['unit/deferred-copy-test.cc', 'unit/fake_device.cc'],
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('deferred-copy', deferred_copy_test)

# Annex-B reassembly for a requested IDR (src/codec/annexb.cc).
annexb_test = executable(
  'annexb-test',
  'annexb-test.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('annexb', annexb_test)

# Concurrent trace writers must keep one record per line.
trace_test = executable(
  'trace-test',
  'trace-test.cc',
  dependencies: [iris_core_dep, threads_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('trace', trace_test)

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

# Hardware probe, built but not registered as a meson test: a VA client's
# rolling intra refresh. The unit tests cover the controls the session writes
# and the mapping from VA's window to a period; only the firmware can say
# whether the request changes the stream, and this drives it through the
# public API so the answer comes from the path a client uses.
if va_drm_dep.found()
  executable(
    'iris-encode-ir-probe',
    'iris-encode-ir-probe.cc',
    dependencies: [va_drm_dep],
    cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
endif

# Hardware probe, built but not registered as a meson test: a VA client's ROI
# map. The host tests cover the attribute, the rectangle-to-macroblock mapping
# and the kernel control write; only the firmware can say whether it honours
# the per-macroblock delta QP, and the kernel module carries the protocol only
# with packaging/kernel/0017 applied.
if va_drm_dep.found()
  executable(
    'iris-encode-roi-probe',
    'iris-encode-roi-probe.cc',
    dependencies: [va_drm_dep],
    cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
endif

# Hardware probe, built but not registered as a meson test: a VA client's
# slice count. The unit tests prove the translation and the control writes;
# only the firmware can say whether it splits the picture, and this counts the
# coded slice NAL units a client receives.
if va_drm_dep.found()
  executable(
    'iris-encode-slice-probe',
    'iris-encode-slice-probe.cc',
    dependencies: [va_drm_dep],
    cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
endif

# Hardware probe, built but not registered as a meson test: a VA client's
# temporal layer structure. The unit tests cover the translation and the
# control writes; only the firmware can say whether it codes more than one
# layer, and this reads the temporal ids out of the stream it produces.
if va_drm_dep.found()
  executable(
    'iris-encode-temporal-probe',
    'iris-encode-temporal-probe.cc',
    dependencies: [va_drm_dep],
    cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
endif

# Hardware probe, built but not registered as a meson test: one elementary
# stream straight into the node through the driver's session, with a chosen
# CAPTURE format. The VA layer refuses a profile before a stream is parsed,
# so only the node can say whether the firmware itself accepts the stream.
executable(
  'iris-decode-stream-probe',
  'iris-decode-stream-probe.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])

# Hardware probe, built but not registered as a meson test: the only reader of
# UBWC in this tree is the GPU, through VK_EXT_image_drm_format_modifier, and
# only a build with Vulkan can build the reader. test/iris-ubwc-gate.sh runs it
# over the frames iris-decode-stream-probe dumps.
if have_vulkan_vpp
  executable(
    'vk-ubwc-read',
    'vk-ubwc-read.cc',
    dependencies: [iris_core_dep, vulkan_dep],
    cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
endif

# Hardware probe, built but not registered as a meson test: the VA half of a
# UBWC surface. test/iris-ubwc-gate.sh runs it with and without V4L2_VA_UBWC,
# so the grant, the exported four-plane descriptor and the image-entry-point
# refusals all come from the public API rather than from a unit test.
if va_drm_dep.found()
  executable(
    'iris-va-ubwc-probe',
    'iris-va-ubwc-probe.cc',
    dependencies: [iris_core_dep, va_drm_dep],
    cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
endif

# Hardware probe, built but not registered as a meson test: the upload half of
# the image API. vaPutImage and the derived-image write path are what put
# client pixels into a surface; vaGetImage reads them back. No decode is
# involved, so this runs against a render node alone.
if va_drm_dep.found()
  executable(
    'iris-va-image-probe',
    'iris-va-image-probe.cc',
    dependencies: [va_drm_dep],
    cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
endif

# Hardware probe, built but not registered as a meson test: submits one
# VideoProc picture with a surface region. FFmpeg has no crop_vaapi filter, so
# this is the only client that can ask the driver for a crop; the gate compares
# the result with `crop,scale=...:flags=neighbor`.
if va_drm_dep.found()
  executable(
    'iris-va-vpp-probe',
    'iris-va-vpp-probe.cc',
    dependencies: [va_drm_dep],
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

# Hardware probe, built but not registered as a meson test: long-term
# references have no VA-side field, so the probe drives the V4L2 encoder node
# itself and writes the three kernel controls. Two otherwise identical
# sessions, one marking and using an LTR frame, say whether the firmware acts
# on the request: byte-identical streams mean it ignores it.
executable(
  'iris-encode-ltr-probe',
  'iris-encode-ltr-probe.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])

# Hardware probe, built but not registered as a meson test: an encode misc
# parameter buffer can arrive with an element too small for its header. The
# host unit tests cannot open an encoder context, so this drives the refusal
# through the public VA API on the target.
if va_drm_dep.found()
  executable(
    'iris-encode-misc-bound-probe',
    'iris-encode-misc-bound-probe.cc',
    dependencies: [va_drm_dep],
    cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
endif

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

# The node takes whole frames, so a field picture is refused before
# submission rather than failing at the first DQBUF. No hardware.
h264_translator_test = executable(
  'h264-translator-test',
  'h264-translator-test.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('h264-translator', h264_translator_test)

# The rebuilt AV1 frame header walked back with a spec-ordered reader: the
# fields whose coding is not a plain copy of the VA value. No hardware.
av1_obu_test = executable(
  'av1-obu-test',
  'av1-obu-test.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('av1-obu', av1_obu_test)

# The make_av1() rollback path holds one picture until its successor names
# the refreshed DPB slots. This host test keeps that former contract covered
# while the default translator is make_av1_dpb().
av1_translator_test = executable(
  'av1-translator-test',
  'av1-translator-test.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('av1-translator', av1_translator_test)

# A hidden reference-installation frame and a shown frame in one temporal
# unit: structure, show_frame bits, and byte parity with the single-frame
# builder. libdav1d decoding of a real two-frame fixture is checked by
# av1-install-tu-roundtrip.sh, invoked from run-static-checks.sh.
av1_install_tu_test = executable(
  'av1-install-tu-test',
  'av1-install-tu-test.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('av1-install-tu', av1_install_tu_test)

# The default internal-DPB AV1 translator: no held picture, eight-slot
# accounting, and a two-completion install unit when the DPB is full. Host
# coverage for the hidden install path that Firefox 158 needs.
av1_dpb_test = executable(
  'av1-dpb-test',
  'av1-dpb-test.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('av1-dpb', av1_dpb_test)

# VA encode parameters -> V4L2 controls, both codecs, no hardware.
encode_params_test = executable(
  'encode-params-test',
  'encode-params-test.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('encode-params', encode_params_test)

# The encoder session's two contracts on a scripted Iris encoder node: which
# V4L2 writes a VA deblocking request produces (and what happens on a kernel
# that has no loop-filter controls at all), and how a coded buffer's access
# unit is claimed once when three VA entry points can ask for it.
encode_session_test = executable(
  'encode-session-test',
  ['unit/encode-session-test.cc', '../src/va/buffer.cc', '../src/va/report.cc'],
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('encode-session', encode_session_test)

# The VideoProc query answers are ABI-free functions over DriverData
# (src/va/vpp.cc); the entry points only resolve the context, lock and
# translate a throw. This drives the answers with a constructed table.
va_vpp_query_test = executable(
  'va-vpp-query-test',
  ['unit/va-vpp-query-test.cc', '../src/va/vpp.cc', '../src/va/tables.cc', '../src/va/report.cc'],
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('va-vpp-query', va_vpp_query_test)

# The vtable a VA client receives. fill_context is the whole of the init
# function except the capability probe, so this links the real driver and
# asserts the 50 required slots are filled and the 10 libva guards stay empty.
# The -D is the same one the module gets: VA_DRIVER_INIT_FUNC is defined only
# by that build, not by the installed libva headers.
va_vtable_test = executable(
  'va-vtable-test',
  files('unit/va-vtable-test.cc') + va_driver_sources,
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20',
             '-DVA_DRIVER_INIT_FUNC=' + va_driver_init_func])
test('va-vtable', va_vtable_test)

# The config attributes a client branches on, and createConfig's value check.
# Links the real config.cc against a constructed DriverData, as va-vtable does.
va_config_attributes_test = executable(
  'va-config-attributes-test',
  files('unit/va-config-attributes-test.cc') + va_driver_sources,
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20',
             '-DVA_DRIVER_INIT_FUNC=' + va_driver_init_func])
test('va-config-attributes', va_config_attributes_test)

# The calls GStreamer's VA filter makes that FFmpeg's does not, against the
# real image.cc and picture.cc.
va_gst_filter_test = executable(
  'va-gst-filter-test',
  files('unit/va-gst-filter-test.cc') + va_driver_sources,
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20',
             '-DVA_DRIVER_INIT_FUNC=' + va_driver_init_func])
test('va-gst-filter', va_gst_filter_test)

# Hardware-independent regression guards for the browser-facing queue and
# stable-export invariants. The integration probe above remains opt-in because
# hosts without /dev/video nodes cannot execute it.
#
# run-static-checks.sh is the same entry point CI uses, so `meson test` and CI
# cannot drift apart. The individual checks stay registered as well to keep
# failures attributable without reading the aggregate output.
# The aggregate runs every checker in the tree, which is past meson's 30 s
# default; the measured standalone time is about three minutes, so it keeps
# headroom while the individual checkers run beside it.
test('static-checks', find_program('run-static-checks.sh'),
     args: [meson.project_source_root()], timeout: 300)
test('iris-source-invariants', find_program('iris-source-invariants.sh'),
     args: [meson.project_source_root()])
test('iris-env-doc-check', find_program('iris-env-doc-check.sh'),
     args: [meson.project_source_root()])
test('iris-codec-matrix-check', find_program('iris-codec-matrix-check.sh'),
     args: [meson.project_source_root()])
test('iris-trace-vocab-check', find_program('python3'),
     args: ['-I', files('iris-trace-vocab-check.py'), meson.project_source_root()])
test('iris-va-vtable-surface', find_program('python3'),
     args: ['-I', files('iris-va-vtable-surface-check.py'), meson.project_source_root()])
test('iris-experiment-guard-self-test', find_program('iris-experiment-guard-self-test.sh'),
     args: [meson.project_source_root()])
test('iris-eos-check-self-test', find_program('iris-eos-check-self-test.sh'),
     args: [meson.project_source_root()])
test('iris-clip-trace-check-self-test', find_program('iris-clip-trace-check-self-test.sh'),
     args: [meson.project_source_root()])
test('iris-gate-output-self-test', find_program('iris-gate-output-self-test.sh'),
     args: [meson.project_source_root()])
test('iris-deploy-path-check', find_program('iris-deploy-path-check.sh'),
     args: [meson.project_source_root()], timeout: 120)
test('iris-setup-path-check', find_program('iris-setup-path-check.sh'),
     args: [meson.project_source_root()])
test('iris-packaging-check', find_program('iris-packaging-check.sh'),
     args: [meson.project_source_root()])
test('iris-spec-selfobsolete-check', find_program('iris-spec-selfobsolete-check.sh'),
     args: ['--self-test', meson.project_source_root()])
test('iris-make-dist-check', find_program('iris-make-dist-check.sh'),
     args: [meson.project_source_root()], timeout: 120)
test('iris-kernel-patch-check', find_program('iris-kernel-patch-check.sh'),
     args: [meson.project_source_root()], timeout: 120)
test('iris-install-manifest-check', find_program('iris-install-manifest-check.sh'),
     args: [meson.project_source_root()])
test('provenance-self-test', find_program('provenance-self-test.sh'),
     args: [meson.project_source_root()])
test('iris-node-check', find_program('iris-node-check.sh'),
     args: [meson.project_source_root()])
