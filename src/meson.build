# Copyright (C) 2019 Bootlin
# Copyright (C) 2023 Max Schettler
# SPDX-License-Identifier: MIT
#
# Three layers (docs/architecture.md §5):
#   util/   options, trace, error, the encode parameter contract
#   iris/   the only code that touches a codec node
#   codec/  VA parameters <-> bitstream, pure
#   va/     libva entry points on top of the two above
# iris + codec are a static library so the unit tests link them without the
# VA glue.

core_args = ['-Wall', '-Wextra', '-fvisibility=hidden']
core_cpp_args = core_args + ['-std=c++20']
core_deps = [libva_dep, libdrm_dep, kernel_dep]

core_sources = [
	'util/options.cc',
	'util/prime.cc',
	'util/prime.cc',
	'iris/device.cc',
	'iris/slots.cc',
	'iris/alloc.cc',
	'iris/msm_alloc.c',
	'iris/publish.cc',
	'iris/session.cc',
	'iris/encode.cc',
	'codec/h264.cc',
	'codec/hevc.cc',
	'codec/vp9.cc',
	'codec/av1.cc',
	'codec/av1_obu.cc',
	'codec/annexb.cc',
	'codec/encode_h264.cc',
	'codec/encode_hevc.cc',
]
if have_gpu_copy
	core_sources += 'iris/gpu_copy.cc'
	core_deps += [egl_dep, glesv2_dep, gbm_dep]
	core_cpp_args += '-DIRIS_HAVE_GPU_COPY'
endif

iris_core = static_library('iris-core',
	core_sources,
	cpp_args: core_cpp_args,
	c_args: core_args,
	pic: true,
	dependencies: core_deps)
iris_core_dep = declare_dependency(
	link_with: iris_core,
	include_directories: include_directories('.'),
	dependencies: core_deps)

v4l2_drv_video = shared_module('v4l2_drv_video',
	name_prefix: '',
	install: true,
	install_dir: join_paths(get_option('libdir'), 'dri'),
	cpp_args: core_cpp_args + ['-DVA_DRIVER_INIT_FUNC=' + va_driver_init_func],
	sources: [
		'va/driver.cc',
		'va/config.cc',
		'va/surface.cc',
		'va/picture.cc',
		'va/buffer.cc',
		'va/image.cc',
	],
	dependencies: [iris_core_dep])
