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

| 编码格式 | 规格 | 缓冲策略 | 一致性 |
| --- | --- | --- | --- |
| H.264 | Constrained Baseline, Main, High | Import（零拷贝） | Fluster 72/135 |
| HEVC | Main, Main 10 | Import（零拷贝） | Fluster 141/147 |
| VP9 | Profile 0, Profile 2 | Import（零拷贝） | 175/305 个测试向量 |
| AV1 | Profile 0（8/10 位） | Import（零拷贝，解码顺序内核） | 8 位与 10 位均与 libdav1d 逐帧一致，含 film grain 合成 |

只有 `data/iris-codec-capabilities.json` 标记为 `production: supported` 的编码格式才对外声明 VA profile。一致性列的数字都来自与软件解码的逐帧比对；具体运行、语料，以及每个未通过向量的归类记录在 `TEST-RESULTS.md`。浏览器会话只有在媒体表面报告了选中的解码器，并给出解码帧数与丢帧数时，才算走硬件。

AV1 film grain 合成已支持。客户端给出独立的 `current_display_picture` 时，驱动以 grain 关闭的方式解码该访问单元，并在画面 `endPicture` 内同步把 grain 混合到显示表面，赶在客户端复用无 grain 的 `current_frame` 之前完成。以 side data 导出 grain 参数的客户端仍自行合成。`test/iris-av1-grain-gate.sh` 要求每个下载帧与 libdav1d 的加 grain 输出逐字节一致。

### 编码

| 编码格式 | 接口入口 | 输入格式 | 码流封装 | 一致性 |
| --- | --- | --- | --- | --- |
| H.264 | `VAEntrypointEncSlice`（CBR、VBR、CQP） | NV12 | Annex-B | 1080p SSIM 0.9986；CBR 与 VBR 下按请求码率输出 |
| HEVC | `VAEntrypointEncSlice`（CBR、VBR、CQP） | NV12 | Annex-B | 1080p SSIM 0.9968；CBR 与 VBR 下按请求码率输出 |

### 视频处理

| 接口入口 | 支持特性 | 输入格式 | 输出格式 | 一致性 |
| --- | --- | --- | --- | --- |
| `VAEntrypointVideoProc`（`VAProfileNone`，需显式开启） | 任意比例缩放（最近邻 / 双线性 / 驱动自选：缩小区域均值、放大三次插值）、裁剪、旋转、镜像；NV12 与 P010 双向位深转换；HDR10 转 SDR 色调映射；最多十六层叠加合成（Y'CbCr 或打包 BGRA/RGBA） | NV12、P010、BGRA、RGBA | NV12、P010 | 最近邻缩放与裁剪与 swscale 逐字节一致；双线性与四点模型相差不超过 1 个码值；色调映射与 BT.2390/BT.709 管线相差不超过 1 个码值；合成 alpha 0 与 1 两端与输入帧逐字节一致 |

视频处理基于 Vulkan 计算内核实现，默认关闭，环境变量 `V4L2_VA_VPP` 存在且非空即启用（例如 `V4L2_VA_VPP=0` 也算启用）。调度管线会自动等待前序解码完成再读取源表面。关于着色器实现、执行顺序与管线约束，详见 [docs/architecture.md](docs/architecture.md)。

## 性能

HEVC Main 8-bit，FFmpeg 8.1.3。CPU 锁最高频，GPU 锁 680 MHz。解码不计下载与显示，编码计入 CPU 端 NV12 上传。

| 操作 | 720p | 1080p | 4K |
| --- | ---: | ---: | ---: |
| 解码 | 759 fps | 581 fps | 359 fps |
| 编码 | 644 fps | 591 fps | 167 fps |

这个 SoC 的吞吐主要跟着 DVFS 走，不锁频测到的是调频策略。多编码格式、转码、并发、功耗与 VPP 数据，以及每个数字的测法，见 [TEST-RESULTS.md](TEST-RESULTS.md)。

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

安装包会自动放置驱动本体与 `msm_drv_video.so`、`qcom_drv_video.so` 两个符号链接：mpv、VLC 与 GStreamer 等客户端可在未指定 `LIBVA_DRIVER_NAME` 的情况下直接加载该驱动，显式设置 `LIBVA_DRIVER_NAME=qcom` 的客户端也能落到同一个驱动。

### 诊断脚本

- `iris-device-check`：只读检测设备节点、固件版本、内核驱动绑定状态及支持格式。
- `iris-verify`：执行轻量级软硬件闭环测试，通过 Iris 中断计数验证编解码通路。

## 客户端配置

mpv、VLC、Sunshine、Firefox、GStreamer 的逐项配置，以及 FFmpeg 的硬解、硬编命令，见
[docs/build-and-deploy.md](docs/build-and-deploy.md#client-setup)。

## 缓冲与发布模型

驱动根据客户端内存生命周期动态选择发布策略：

- **Direct**：客户端在解码后直接导出驱动分配的表面，实现零拷贝。
- **Import**：客户端在解码前传入预分配的 DMA-BUF 缓冲区，在应用了解码顺序补丁的内核上实现零拷贝。
- **Copy**：固件解码至驱动内部缓冲槽位，再由 GPU 复制至客户端表面。用于未打补丁的原版内核，以及同一 VA 上下文内 AV1 分辨率切换后的后半段。AV1 在应用了解码顺序补丁的内核上走 Import 零拷贝路径。

导入的 DMA-BUF 表面必须严格符合 CAPTURE 布局规范（单线性对象，128 字节对齐跨度）。不符合规范的表面直接返回 `VA_STATUS_ERROR_INVALID_PARAMETER`。

## 已知限制

- **AV1 动态分辨率切换**：驱动内部维护八槽 DPB，在 `vaEndPicture` 直接提交当前帧。同一 VA 上下文分辨率改变后，若客户端继续沿用旧 CAPTURE 几何导出表面，驱动检测到跨距不匹配时改用 GPU 拷贝发布，避免跨距错位。Chromium 播放本地文件时会自行选择 AV1 解码器，可能落到内置软件解码；会话是否走硬件以 [docs/build-and-deploy.md](docs/build-and-deploy.md#browser-acceptance-gate) 的浏览器验收门禁为准。
- **原版内核降级**：在缺少显示延迟控制或强制关键帧控制的内核上，解码降级为 GPU 拷贝，关键帧请求通过临时改动 intra period 实现。
- **编码高级特性**：仅支持单图单切片结构。多切片、分块（Tiles）及 ROI 配置直接返回 `VA_STATUS_ERROR_UNIMPLEMENTED`。B 帧需通过 `V4L2_VA_B_FRAMES` 显式开启，且仅限 H.264；客户端管线需至少保持三帧在途（FFmpeg：`-bf 1 -async_depth 3`）。
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
| `V4L2_VA_B_FRAMES` | off | 启用 H.264 编码器的 B 帧；要求客户端管线保持三帧以上在途（FFmpeg：`-async_depth 3`） |
| `V4L2_VA_DUMP` | off | 提交的码流单元转储目录路径 |
