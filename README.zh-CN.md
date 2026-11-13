# Qualcomm Iris VA-API 视频驱动（SM8550）

[English](README.md) | **中文**

面向小米平板 6S Pro（sheng，骁龙 SM8550）硬件优化的 Qualcomm Iris V4L2 VA-API 视频驱动。为 ARM64 Linux 上的 Google Chrome、Firefox、FFmpeg、Kodi 和 Sunshine（Moonlight 串流）提供硬件视频编解码支持，在已验收的流水线上把解码画面直接送进应用缓冲区，不做拷贝。

仓库内包含的内核补丁、诊断脚本与打包规则均作为硬件验证与部署配套提供。

## 支持特性

### 解码

| 编码格式 | 规格 | 缓冲策略 | 验证基准 |
| --- | --- | --- | --- |
| H.264 | Constrained Baseline, Main, High | Import（零拷贝） | 逐帧 MD5 一致，Fluster 72/135 |
| HEVC | Main, Main 10 | Import（零拷贝） | 逐帧 MD5 一致，Fluster 141/147 |
| VP9 | Profile 0, Profile 2 | Import（零拷贝） | 逐帧 MD5 一致，2026-11-12 Chrome 含 alt-ref 实机验证 |
| AV1 | Profile 0（8/10 位） | Import（零拷贝，解码顺序内核） | 逐帧 MD5 一致，2026-11-11 安装包与浏览器实机验证；1080p 与 4K 10 位以 P010 Import 与 libdav1d 逐帧一致；film grain 合成在 8 位与 10 位逐帧一致 |

硬件解码的判定标准：媒体表面报告选中了硬件解码器（Chrome 为 `VaapiVideoDecoder` 且 `kIsPlatformVideoDecoder=true`，Firefox 为 VA-API 初始化成功），并给出测试窗口内的解码帧数与丢帧数。

AV1 film grain 合成已支持。客户端给出独立的 `current_display_picture` 时，驱动以 grain 关闭的方式解码该访问单元，并在画面 `endPicture` 内同步把 grain 混合到显示表面，赶在客户端复用无 grain 的 `current_frame` 之前完成。以 side data 导出 grain 参数的客户端仍自行合成。`test/iris-av1-grain-gate.sh` 要求每个下载帧与 libdav1d 的加 grain 输出逐字节一致。

### 编码

| 编码格式 | 接口入口 | 输入格式 | 码流封装 | 验证基准 |
| --- | --- | --- | --- | --- |
| H.264 | `VAEntrypointEncSlice`（CBR、VBR、CQP） | NV12 | Annex-B | 1080p FFmpeg，GOP 60，IRQ 增量确认 |
| HEVC | `VAEntrypointEncSlice`（CBR、VBR、CQP） | NV12 | Annex-B | 1080p FFmpeg，GOP 60，IRQ 增量确认 |

硬件编码的判定标准：测试执行后 Iris 硬件中断计数产生有效增量，输出码流可正常解码，且 SPS 参数与请求的 profile 及分辨率完全匹配。

### 视频处理

| 接口入口 | 操作 | 输入格式 | 输出格式 | 验证基准 |
| --- | --- | --- | --- | --- |
| `VAEntrypointVideoProc`（`VAProfileNone`，需显式开启） | 任意比例缩放，插值可选最近邻、双线性或驱动自选，可带裁剪、旋转与镜像；NV12 与 P010 之间双向转换位深；HDR10 转 SDR 色调映射；带全局 alpha 的多层合成，最多十六层，叠加面可为 Y'CbCr 或打包 BGRA/RGBA | NV12、P010、BGRA、RGBA | NV12、P010 | `scale_vaapi` 输出与 swscale `flags=neighbor` 逐字节一致；双线性与四点模型相差不超过 1 个码值，常量不变，1:1 时与点映射相同；裁剪与 `crop,scale` 逐字节一致；跨位深画面与 `scale` 再 `format` 两个方向都逐字节一致；`transpose_vaapi` 全部七个方向与软件滤镜逐字节一致；色调映射与文档所述流程相差不超过 1 个码值，单独执行以及接七个转置方向、裁剪缩放或合成之后都一样；合成的 alpha 0 与 alpha 1 两端各自与输入帧逐字节相同，多层按提交顺序叠加；`overlay_vaapi` 走 BGRA 上传时输出等于纯色叠加面精确的 BT.709 码值 |

视频处理默认关闭，需设置 `V4L2_VA_VPP`。处理在 surface 的 DMA-BUF 上运行固定的 Vulkan 计算内核，缩放取点位置与 swscale 相同，起点是客户端给出的 surface region，先按客户端要求做顺时针旋转再缩放。插值方式由画面的 `filter_flags` 决定：默认值和 `VA_FILTER_INTERPOLATION_NEAREST_NEIGHBOR` 都是点映射，`VA_FILTER_INTERPOLATION_BILINEAR` 在目标样本中心周围取四点做双线性混合，`VA_FILTER_INTERPOLATION_ADVANCED` 由驱动按每个轴自己选——某个轴在缩小时取区域平均，两级抽头在那里会混叠，放大时取四点三次插值。pipeline caps 公布的是 advanced：这个字段存的是单个方法而不是位集合，三者中它最强。NV12 与 P010 用同一套抽头，按目标样本宽度各有一个内核，源与目标的位深可以不同：内核在取点前先做 `code >> 2` 降到八位或 `code << 2` 升到十位，这正是 swscale 的换算，所以只改位深的画面与 `format` 逐字节相同，同一画面里的缩放也与 `scale` 再 `format` 一致。色彩转换和下面这一个之外的滤镜都会被拒绝。水平、垂直或两者同时的镜像都在旋转之后、缩放之前生效，所以客户端能写的 `transpose_vaapi` 七个方向都能跑。几何下限是内核自身的 2x2，不再受解码节点最小尺寸约束，因此可以缩到缩略图大小。滤镜只实现了一个：带 HDR10 元数据的 `VAProcFilterHighDynamicRangeToneMapping`，按 BT.2390 曲线、BT.2020 到 BT.709 矩阵和 BT.709 OETF 把 P010 的 BT.2020/PQ 画面转成 SDR NV12。它按原尺寸转换整幅 surface 并先执行，所以同一画面里的裁剪、缩放、旋转、镜像或第二个 buffer 都是转换之后的独立一档，不再被拒绝；NV12 输入、HDR 转 HDR 和第二个滤镜仍然拒绝。处理器也做合成：一个画面可以带一个主 pipeline buffer 和最多十六个叠加档，每档用 `VABlendState` 把一个 surface 混进输出的一个偶数矩形，也就是 `overlay_vaapi` 提交的形状，只是可以叠多层。各档按提交顺序执行，后一档盖住前一档；驱动先把主矩形拷进输出，再逐层读回输出，因此一个内核能服务任意多层。叠加面可以是输出自身的 Y'CbCr 格式，在两个平面的码值上混合；也可以是打包的 `VA_RT_FORMAT_RGB32` 面（`VA_FOURCC_BGRA` 或 `VA_FOURCC_RGBA`），混合时转换成输出画面的 BT.601、BT.709 或 BT.2020 矩阵。叠加面从它自己的 surface region 采样，所以客户端可以只合成一个 surface 的一部分，插值方式与缩放内核相同。两种形式都实现 `VA_BLEND_GLOBAL_ALPHA` 和 `VA_BLEND_PREMULTIPLIED_ALPHA`（FFmpeg 对带 alpha 通道的叠加输入会置后者）：置上后打包叠加面自己的 alpha 字节会乘上全局 alpha，带真实 alpha 通道的 surface 因此能出它描述的画面。输出范围取自 `output_color_properties`，全范围目标编码在 0..255 而不是 16..235。luma key 会被拒绝，alpha 取 0 或 1 时输出分别与主画面和叠加画面逐字节相同。

## 性能

SM8550 平板上的 HEVC Main 8-bit 吞吐，FFmpeg 8.1.3。每个格五轮交错运行，三个 CPU policy 都锁在 performance governor 的最高频（2016/2803/2956 MHz），`3d00000.gpu` 锁在 680 MHz。片源为 GOP 60、`bframes=0`、`keyint=60` 的 x265 合成 I/P 帧，720p 与 1080p 每次 900 帧，4K 480 帧，进程启动时间因此被摊薄。编码使用 VBR 5/10/20 Mbps，计入 CPU 端的 NV12 上传，`-async_depth 4`；解码不下载、不显示。对照组是 `radxa-pkg/qcom-va-driver`，同一节点、同一批片源、同一种构建布局。

| 操作 | 720p | 1080p | 4K |
| --- | ---: | ---: | ---: |
| 解码，本驱动 | 759 fps | 581 fps | 359 fps |
| 解码，对照组 | 759 fps | 579 fps | 358 fps |
| 编码，本驱动 | 644 fps | 591 fps | 167 fps |
| 编码，对照组 | 631 fps | 575 fps | 163 fps |

解码三档都在 0.4% 以内，算持平；编码分别快 2.1%、2.8%、2.5%。解码每 900 帧的客户端用户态 CPU 少 17%、16%、19%（0.27/0.42/0.40 s 对 0.33/0.50/0.50 s），来自把拷贝引擎推迟到真正需要拷贝时才启动：不需要拷贝的流水线不会拉起 GPU 拷贝后端。编码的用户态 CPU 两者相同（720p 1.01 对 1.08 s，1080p 1.94 对 1.91 s，4K 4.82 对 4.80 s），fps 的差距来自调度开销，不是多干活。对照组公布的表是 SC8280XP 的成绩，不能和这里的 SM8550 数字直接比较。

六个 1080p 编码在同一个节点上的对比：H.264、HEVC 8-bit 和 VP9 解码持平（+1.1% 到 +1.3%）；AV1 8-bit 快 9.8%，10-bit 快 11.5%，客户端 CPU 分别少 35% 和 77%；HEVC 10-bit 慢 2.4%，但客户端 CPU 少 16%。1080p HEVC 解码后转成 H.264，吞吐持平，客户端 CPU 少 23%。Kodi 播 1080p 和 4K 的墙钟时间持平，4K 下 CPU 少 6%。

VPP 把 1080p 缩到 720p：每 900 帧 540 fps，客户端用户态 CPU 0.54 s；同样的硬件解码接 `hwdownload,format=nv12,scale=1280:720` 只有 287 fps、8.37 s。画面留在设备上由 GPU 缩放，不用搬到 CPU。用同一份源码打开 VPP 选项重建对照组，它的 Vulkan 后端 536 fps、客户端 CPU 高 14%；镜像和 YUV 叠加按它的设计被拒绝，本驱动能渲染。VPP 默认关闭，设置 `V4L2_VA_VPP` 才启用。

## 运行环境

- 目标平台：高通 SM8550（HFI gen2 Iris VPU），aarch64 架构。
- 核心依赖：`libva` >= 1.1，`libdrm` >= 2.4.52。GPU 拷贝引擎需要 EGL、GLES 和 GBM 支持。
- 发行版基准：以 Fedora 44 作为基准验证环境；`packaging/` 与 `debian/` 下提供 Fedora、Arch 与 Debian/Ubuntu arm64 的打包规则。
- 配套内核补丁：位于 `packaging/kernel/`，附带模块独立编译指南。在未应用补丁的原版内核上，驱动会自动降级至显示顺序解码（GPU 拷贝模式），并将 IDR 请求回退至修改 intra period 方案。

## 安装与配置

### 软件包安装

Fedora：
```sh
rpmbuild -ba packaging/fedora/libva-v4l2-iris.spec
sudo dnf install ~/rpmbuild/RPMS/aarch64/libva-v4l2-iris-*.rpm
```

Arch Linux：
```sh
cd packaging/arch && makepkg -si
```

Debian / Ubuntu（arm64）：
```sh
dpkg-buildpackage -b -us -uc
sudo apt install ../libva-v4l2-iris_0.1.0_arm64.deb
```

Debian 包请在 chroot（`sbuild`、`pbuilder`）里构建：`libgbm-dev` 绑定发行版的 Mesa，直接在设备上安装构建依赖可能降级厂商魔改的图形栈。

安装包会自动放置驱动本体与 `msm_drv_video.so`、`qcom_drv_video.so` 两个符号链接：mpv、VLC 与 GStreamer 等客户端可在未指定 `LIBVA_DRIVER_NAME` 的情况下直接加载该驱动，按兄弟发行版写法显式设置 `LIBVA_DRIVER_NAME=qcom` 的客户端也能落到同一个驱动。

### 诊断脚本

- `iris-device-check`：只读检测设备节点、固件版本、内核驱动绑定状态及支持格式。
- `iris-verify`：执行轻量级软硬件闭环测试，通过 Iris 中断计数验证编解码通路。

## 缓冲与发布模型

驱动根据客户端内存生命周期动态选择发布策略：

- **Direct**：客户端在解码后直接导出驱动分配的表面，实现零拷贝。
- **Import**：客户端在解码前传入预分配的 DMA-BUF 缓冲区，在应用了解码顺序补丁的内核上实现零拷贝。
- **Copy**：固件解码至驱动内部缓冲槽位，再由 GPU 复制至客户端表面。用于未打补丁的原版内核，以及同一 VA 上下文内 AV1 分辨率切换后的后半段。AV1 在应用了解码顺序补丁的内核上走 Import 零拷贝路径（2026-11-11 安装包实机验证，见 TEST-RESULTS.md）。

导入的 DMA-BUF 表面必须严格符合 CAPTURE 布局规范（单线性对象，128 字节对齐跨度）。不符合规范的表面直接返回 `VA_STATUS_ERROR_INVALID_PARAMETER`。

## 已知限制

- **AV1 动态分辨率切换与浏览器选择**：驱动内部维护八槽 DPB，在 `vaEndPicture` 直接提交当前帧。同一 VA 上下文分辨率改变后，若客户端继续沿用旧 CAPTURE 几何导出表面，驱动检测到跨距不匹配时改用 GPU 拷贝发布，避免跨距错位。Chromium 152 播放本地文件默认走内置软件解码；Chrome 156 在线页面与 Firefox 158 已验证硬解。
- **原版内核降级**：在缺少显示延迟控制或强制关键帧控制的内核上，解码降级为 GPU 拷贝，关键帧请求通过临时改动 intra period 实现。
- **编码高级特性**：仅支持单图单切片结构。多切片、分块（Tiles）、B 帧及 ROI 配置直接返回 `VA_STATUS_ERROR_UNIMPLEMENTED`。
- **封装头信息**：头信息声明为 `VA_ENC_PACKED_HEADER_NONE`，参数集随码流带内传输。

## 环境变量

| 变量名 | 默认值 | 作用说明 |
| --- | --- | --- |
| `LIBVA_V4L2_VIDEO_PATH` | probe | 强制指定 V4L2 解码节点路径 |
| `LIBVA_V4L2_ENCODER_PATH` | probe | 强制指定 V4L2 编码节点路径 |
| `V4L2_VA_TRACE` | off | 启用标准错误流执行轨迹追踪 |
| `V4L2_VA_SYNC_TIMEOUT_MS` | 2000 | session 等待超时时间（毫秒） |
| `V4L2_VA_COPY` | gpu | 拷贝引擎选择（`gpu` 或 `cpu`） |
| `V4L2_VA_PUBLISH` | auto | 强制指定发布模式（`auto`、`direct`、`import`、`copy`） |
| `V4L2_VA_VPP` | off | 启用 `VAProfileNone` / `VAEntrypointVideoProc` 入口（NV12/P010 任意比例缩放，三种插值方式任选、位深双向转换、旋转、镜像、HDR10 转 SDR 色调映射，以及最多十六层的全局 alpha 合成，含打包 BGRA/RGBA 叠加面） |
| `V4L2_VA_DUMP` | off | 提交的码流单元转储目录路径 |

GStreamer 的 VA 插件（`libgstva`）在注册任何元素之前会按厂商字符串过滤 VA 驱动，本驱动的字符串不在白名单里：`gst-inspect-1.0 va` 报 `0 features`，`vaav1dec` 元素不存在。`GST_VA_ALL_DRIVERS=1` 是插件自带的绕过开关，设置后元素才会注册；libva 仍然通过 `msm_drv_video.so` 符号链接加载本驱动，不涉及任何 `LIBVA_*` 变量。`test/iris-gstreamer-gate.sh` 在平板上验证这两种状态。
