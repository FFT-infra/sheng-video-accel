# Qualcomm Iris VA-API 视频驱动（SM8550）

[English](README.md) | **中文**

面向 SM8550（sheng，小米平板 6S Pro）的 Qualcomm Iris V4L2 VA-API 视频驱动，运行于 ARM64 Linux。已验收的流水线上，解码帧直接进入客户端缓冲区。Chrome、Firefox、FFmpeg、Kodi、mpv、GStreamer 与 Sunshine 都通过 VA-API 使用它。

## 支持特性

### 解码

| 编码格式 | 规格 | 缓冲策略 | 一致性 |
| --- | --- | --- | --- |
| H.264 | Constrained Baseline, Main, High | Import（零拷贝） | Fluster 72/135 |
| HEVC | Main, Main 10 | Import（零拷贝） | Fluster 141/147 |
| VP9 | Profile 0, Profile 2 | Import（零拷贝） | Fluster 176/305 |
| AV1 | Profile 0（8/10 位） | Import（零拷贝，解码顺序内核） | 与 libdav1d 逐帧一致，含 film grain 合成 |

只有能力表标记 `production: supported` 的编码格式才声明 VA profile。一致性数字来自与软件解码的逐帧比对。Fluster 是语料结果，不是每次构建的回归；失败向量归类见 `TEST-RESULTS.md`。VP9 参考 MD5 早于本机 libvpx，176/305 不是解码缺陷数。

解码上限 7680x4320，四种编码格式均支持；两个 1080p 客户端可同时解码。

AV1 film grain 合成在用户态完成。客户端给出独立的 `current_display_picture` 时，驱动在复用 `current_frame` 之前把 grain 混到显示表面。以 side data 传 grain 参数的客户端自行合成。`test/iris-av1-grain-gate.sh` 要求每帧与 libdav1d 的加 grain 输出逐字节一致。

### 编码

| 编码格式 | 接口入口 | 输入格式 | 码流封装 | 一致性 |
| --- | --- | --- | --- | --- |
| H.264 | `VAEntrypointEncSlice`（CBR、VBR、CQP） | NV12 | Annex-B | 1080p SSIM 0.9986；CBR 与 VBR 下按请求码率输出 |
| HEVC | `VAEntrypointEncSlice`（CBR、VBR、CQP） | NV12 | Annex-B | 1080p SSIM 0.9968；CBR 与 VBR 下按请求码率输出 |

### 视频处理

| 操作 | 说明 |
| --- | --- |
| 缩放 | 任意比例；最近邻、双线性，或驱动自选（缩小区域均值、放大三次插值） |
| 裁剪、旋转、镜像 | VA 的七个方向，NV12 与 P010 |
| 位深转换 | NV12 <-> P010 |
| 色调映射 | HDR10 转 SDR |
| 合成 | 最多 16 层，Y'CbCr 或打包 BGRA/RGBA |

输入为 NV12、P010、BGRA 或 RGBA，输出为 NV12 或 P010。

视频处理由 Vulkan 计算内核完成，直接读写表面 DMA-BUF。构建中找到 Vulkan 即提供 `VAProfileNone` / `VAEntrypointVideoProc`，设 `V4L2_VA_VPP=0` 可关闭。细节见 [docs/architecture.md](docs/architecture.md)。

## 运行环境

- 平台：高通 SM8550（HFI gen2 Iris VPU），aarch64。
- 核心依赖：`libva` >= 1.1，`libdrm` >= 2.4.52。GPU 拷贝引擎需要 EGL、GLES 与 GBM。
- 发行版：Fedora 44 是基准。`packaging/` 与 `debian/` 提供 Fedora、Arch 与 Debian/Ubuntu arm64 包。
- 内核：`sheng-7.3.4`（`87ac0f2347eb`）配 0008-0009，或 `sheng-7.2.6`（`42f3b40c702a`）配 0001-0004，两者均已验收。原版内核上仍可构建安装，降级方式见「已知限制」。补丁在 `packaging/kernel/`。

## 安装

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

Debian 包请在 chroot（`sbuild`、`pbuilder`）里构建：`libgbm-dev` 绑定发行版 Mesa，直接装依赖可能降级厂商修改过的图形栈。安装包会放置驱动本体和 `msm_drv_video.so`、`qcom_drv_video.so` 两个别名，libva 无需 `LIBVA_DRIVER_NAME` 即可解析。

## 验证

```sh
iris-device-check
iris-verify
```

`iris-device-check` 只读：节点、固件版本、绑定的驱动与队列格式。`iris-verify` 编解码一段样例，要求 Iris 中断计数增长。四种编码格式的完整像素比对用 `test/iris-matrix.sh`。浏览器会话只有在媒体表面给出解码器、解码帧数与丢帧数时才算走硬件，见 [docs/build-and-deploy.md](docs/build-and-deploy.md#browser-acceptance-gate)。

## 性能

HEVC Main 8-bit，FFmpeg 8.1.3。CPU 锁最高频，GPU 锁 680 MHz。解码不计下载与显示，编码计入 CPU 端 NV12 上传。

| 操作 | 720p | 1080p | 4K |
| --- | ---: | ---: | ---: |
| 解码 | 953 fps | 754 fps | 346 fps |
| 编码 | 679 fps | 585 fps | 172 fps |

这组数字来自已验收的 7.3.4 组合。吞吐主要跟 DVFS 走，不锁频测到的是调频策略。完整数据见 [TEST-RESULTS.md](TEST-RESULTS.md)。

1080p、4 Mbit/s 下，H.264 比 libx264 `veryfast` 快 4.4 倍，HEVC 比 libx265 快 15.2 倍，两种编码都能到 7680x4320（`test/iris-encode-baseline.sh`）。

## 缓冲模型

- **Direct**：客户端导出驱动分配的表面，解码后直接使用。
- **Import**：客户端在解码前分配并传入 DMA-BUF。在解码顺序内核上零拷贝。
- **Copy**：固件解码到驱动槽位，GPU 复制到客户端缓冲区。用于原版内核，以及同一 VA 上下文内 AV1 分辨率切换后的后半段。

导入的 DMA-BUF 必须符合原生 CAPTURE 布局：单一线性对象，跨度 128 字节对齐。其他情况返回 `VA_STATUS_ERROR_INVALID_PARAMETER`。

## 已知限制

- **AV1 分辨率切换**：上下文内切换后，继续按旧几何导出表面的客户端在这些帧上得到 GPU 拷贝。Chromium 播本地文件时可能选软件 AV1 解码器，是否走硬件以浏览器门禁为准。
- **原版内核**：解码降级为 GPU 拷贝，IDR 请求改为移动 intra period。
- **编码**：仅单图单切片。多切片、分块与 ROI 返回 `VA_STATUS_ERROR_UNIMPLEMENTED`。B 帧需 `V4L2_VA_B_FRAMES` 开启，仅 H.264，客户端需保持至少三帧在途（FFmpeg：`-bf 1 -async_depth 3`）。
- **intra refresh**：滚动行请求（`VAEncMiscParameterRIR`）在 CBR 下生效，VBR/CQP 下拒绝。`ltr_count`/`frame_ltr_index`/`use_ltr_frames` 不声明。
- **封装头**：extradata 报告 `VA_ENC_PACKED_HEADER_NONE`，参数集随码流带内传输。
- **UBWC**：节点列出 `Q08C`/`Q10C`，固件确实写出 UBWC。Turnip 能逐字节解 Q08C，EGL（Copy 路径与 `vaGetImage`/`vaPutImage`）不能，P010 也没有压缩格式。没有客户端请求压缩表面，表面保持 `NV12`/`P010`，导入必须是 `DRM_FORMAT_MOD_LINEAR` 平面。

## 环境变量

| 变量名 | 默认值 | 作用说明 |
| --- | --- | --- |
| `LIBVA_V4L2_VIDEO_PATH` | probe | 强制指定 V4L2 解码节点路径 |
| `LIBVA_V4L2_ENCODER_PATH` | probe | 强制指定 V4L2 编码节点路径 |
| `V4L2_VA_TRACE` | off | 启用标准错误流执行轨迹追踪 |
| `V4L2_VA_SYNC_TIMEOUT_MS` | 2000 | session 等待超时时间（毫秒） |
| `V4L2_VA_COPY` | gpu | 拷贝引擎选择（`gpu` 或 `cpu`） |
| `V4L2_VA_PUBLISH` | auto | 强制指定发布模式（`auto`、`direct`、`import`、`copy`） |
| `V4L2_VA_VPP` | on | 设为 `0` 关闭 Vulkan 视频处理 |
| `V4L2_VA_B_FRAMES` | off | 启用 H.264 编码器的 B 帧；需保持三帧以上在途（FFmpeg：`-async_depth 3`） |
| `V4L2_VA_DUMP` | off | 提交的码流单元转储目录路径 |

## 命名

同一个组件有五个名字，分别由不同的消费方固定：

| 层 | 名字 | 由谁固定 |
| --- | --- | --- |
| 仓库 | `qualcomm-iris-vaapi` | 下游 `Source0` URL 与整机仓的锁定文件 |
| Meson 工程 | `libva-v4l2` | `meson.build`，沿用上游 fork 的名字 |
| 源码包 | `libva-v4l2-iris-<version>.tar.gz` | `scripts/make-dist.sh`、`%autosetup -n` |
| 软件包（RPM、deb、PKGBUILD） | `libva-v4l2-iris` | `packaging/` 与 `debian/` |
| 运行时 VA 驱动 | `v4l2_drv_video.so`，`LIBVA_DRIVER_NAME=v4l2` | libva 的 `<name>_drv_video.so` 约定 |

## 文档

- 客户端配置与 FFmpeg 命令：[docs/build-and-deploy.md](docs/build-and-deploy.md#client-setup)
- 测试、门禁与诊断：[test/README.md](test/README.md)
- 内部设计与 trace 词表：[docs/architecture.md](docs/architecture.md)
- 当前验收组合：[docs/STATE.md](docs/STATE.md)
- 硬件证据：[TEST-RESULTS.md](TEST-RESULTS.md)
