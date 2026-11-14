# Qualcomm Iris VA-API 视频驱动（SM8550）

[English](README.md) | **中文**

面向小米平板 6S Pro（sheng，骁龙 SM8550）硬件优化的 Qualcomm Iris V4L2 VA-API 视频驱动。为 ARM64 Linux 上的 Google Chrome、Firefox、FFmpeg、Kodi 和 Sunshine（Moonlight 串流）提供硬件视频编解码支持，在已验收的流水线上把解码画面直接送进应用缓冲区，不做拷贝。

仓库内包含的内核补丁、诊断脚本与打包规则均作为硬件验证与部署配套提供。

## 项目身份

同一个组件有五个名字，分别由不同的消费方固定，改其中一个不会自动改其余的：

| 层 | 名字 | 由谁固定 |
| --- | --- | --- |
| 仓库 | `qualcomm-iris-vaapi` | 下游 `Source0` URL 与整机仓的锁定文件 |
| Meson 工程 | `libva-v4l2` | `meson.build`，沿用上游 fork 的名字 |
| 源码包 | `libva-v4l2-iris-<version>.tar.gz` | `scripts/make-dist.sh`、`%autosetup -n` |
| 软件包（RPM、deb、PKGBUILD） | `libva-v4l2-iris` | `packaging/` 与 `debian/` |
| 运行时 VA 驱动 | `v4l2_drv_video.so`，`LIBVA_DRIVER_NAME=v4l2` | libva 的 `<name>_drv_video.so` 约定 |

`msm_drv_video.so` 和 `qcom_drv_video.so` 是同一个驱动的安装别名，供按 DRM 驱动名解析、或设置 `LIBVA_DRIVER_NAME=qcom` 的客户端使用。源码包是上游构建产物：`.gitattributes` 把 CI、Agent 说明和实验记录排除在外；三种发行版打包格式仍留在包里，因为随包的 spec 会对它们运行 `test/iris-packaging-check.sh`。

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

| 接口入口 | 支持特性 | 输入格式 | 输出格式 | 验证基准 |
| --- | --- | --- | --- | --- |
| `VAEntrypointVideoProc`（`VAProfileNone`，需显式开启） | 任意比例缩放（最近邻 / 双线性 / 驱动自选：缩小区域均值、放大三次插值）、裁剪、旋转、镜像；NV12 与 P010 双向位深转换；HDR10 转 SDR 色调映射；最多十六层叠加合成（Y'CbCr 或打包 BGRA/RGBA） | NV12、P010、BGRA、RGBA | NV12、P010 | `scale_vaapi` 最近邻与裁剪与 swscale 逐字节一致；双线性与四点模型相差不超过 1 个码值；色调映射与 BT.2390/BT.709 管线相差不超过 1 个码值；合成 alpha 0 与 1 两端与输入帧逐字节一致 |

视频处理基于 Vulkan 计算内核实现，默认关闭，环境变量 `V4L2_VA_VPP` 存在且非空即启用（例如 `V4L2_VA_VPP=0` 也算启用）。调度管线会自动等待前序解码完成再读取源表面。关于着色器实现、执行顺序与管线约束，详见 [docs/architecture.md](docs/architecture.md)。

## 性能

SM8550 平板上的 HEVC Main 8-bit 吞吐，FFmpeg 8.1.3。每个格五轮交错运行，三个 CPU policy 都锁在 performance governor 的最高频（2016/2803/2956 MHz），`3d00000.gpu` 锁在 680 MHz。片源为 GOP 60 的 x265 合成 I/P 帧，`-async_depth 4`；编码计入 CPU 端 NV12 上传，解码不下载、不显示。对照组为同一节点与片源下的 `radxa-pkg/qcom-va-driver`：

| 操作 | 720p | 1080p | 4K |
| --- | ---: | ---: | ---: |
| 解码，本驱动 | 759 fps | 581 fps | 359 fps |
| 解码，对照组 | 759 fps | 579 fps | 358 fps |
| 编码，本驱动 | 644 fps | 591 fps | 167 fps |
| 编码，对照组 | 631 fps | 575 fps | 163 fps |

解码吞吐持平（各分辨率差距均在 0.4% 以内），得益于按需延迟启动拷贝引擎，解码端客户端 CPU 开销降低 16%–19%（900 帧耗时 0.27/0.42/0.40 s 对 0.33/0.50/0.50 s）。编码吞吐在各分辨率下领先 2.1%–2.8%，客户端 CPU 耗时持平（1.01/1.94/4.82 s 对 1.08/1.91/4.80 s）。对照组在其文档中公布的数据测自 SC8280XP，不能与此处的 SM8550 实测数据直接对比。更多编解码器对比（含 AV1 8/10 位）、并发、转码、功耗及 VPP 跑分详见 [TEST-RESULTS.md](TEST-RESULTS.md#cross-driver-benchmarks)。

## 运行环境

- 目标平台：高通 SM8550（HFI gen2 Iris VPU），aarch64 架构。
- 核心依赖：`libva` >= 1.1，`libdrm` >= 2.4.52。GPU 拷贝引擎需要 EGL、GLES 和 GBM 支持。
- 发行版基准：以 Fedora 44 作为基准验证环境；`packaging/` 与 `debian/` 下提供 Fedora、Arch 与 Debian/Ubuntu arm64 的打包规则。
- 配套内核补丁：位于 `packaging/kernel/`，附带模块独立编译指南。在未应用补丁的原版内核上，驱动会自动降级至显示顺序解码（GPU 拷贝模式），并将 IDR 请求回退至修改 intra period 方案。

## 安装与配置

### 软件包安装

Fedora：
```sh
sh scripts/make-dist.sh --out ~/rpmbuild/SOURCES
rpmbuild -ba packaging/fedora/libva-v4l2-iris.spec \
    --define "source_commit $(git rev-parse HEAD)" \
    --define "tracked_sha256 $(git ls-files -z | xargs -0 sha256sum | sha256sum | awk '{print $1}')"
sudo dnf install ~/rpmbuild/RPMS/aarch64/libva-v4l2-iris-*.rpm
```

Arch Linux：
```sh
sh scripts/make-dist.sh
cd packaging/arch && makepkg -si \
    IRIS_SOURCE_COMMIT=$(git -C ../.. rev-parse HEAD) \
    IRIS_TRACKED_SOURCE_SHA256=$(git -C ../.. ls-files -z \
        | xargs -0 sha256sum | sha256sum | awk '{print $1}')
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
| `V4L2_VA_VPP` | off | 启用 Vulkan 视频处理后端（`VAEntrypointVideoProc`：缩放、裁剪、旋转、镜像、位深转换、色调映射、多层合成） |
| `V4L2_VA_DUMP` | off | 提交的码流单元转储目录路径 |

> **提示（GStreamer）**：GStreamer 的 VA 插件（`libgstva`）默认按厂商白名单过滤驱动。若需注册本驱动的元素（如 `vaav1dec`），需设置环境变量 `GST_VA_ALL_DRIVERS=1`。驱动本身仍由 libva 自动通过 `msm_drv_video.so` 符号链接加载，无需配置 `LIBVA_*` 变量；`test/iris-gstreamer-gate.sh` 在目标机上验证这两种状态。
