# Qualcomm Iris VA-API 视频驱动（SM8550）

[English](README.md) | **中文**

面向小米平板 6S Pro（sheng，骁龙 SM8550）硬件优化的 Qualcomm Iris V4L2 VA-API 视频驱动。为 ARM64 Linux 上的 Google Chrome、Firefox 和 FFmpeg 提供硬件视频编解码支持，并在特定流水线中支持画面从固件直出应用缓冲区的零拷贝路径。

仓库内包含的内核补丁、诊断脚本与打包规则均作为硬件验证与部署配套提供。

## 支持特性

### 解码

| 编码格式 | 规格 | 缓冲策略 | 验证基准 |
| --- | --- | --- | --- |
| H.264 | Constrained Baseline, Main, High | Import（零拷贝） | 逐帧 MD5 一致，Fluster 72/135 |
| HEVC | Main, Main 10 | Import（零拷贝） | 逐帧 MD5 一致，Fluster 141/147 |
| VP9 | Profile 0, Profile 2 | Import（零拷贝） | 逐帧 MD5 一致，2026-11-12 Chrome 含 alt-ref 实机验证 |
| AV1 | Profile 0 | Import（零拷贝，解码顺序内核） | 逐帧 MD5 一致，2026-11-11 安装包与浏览器实机验证 |

硬件解码的判定标准：媒体表面报告选中了硬件解码器（Chrome 为 `VaapiVideoDecoder` 且 `kIsPlatformVideoDecoder=true`，Firefox 为 VA-API 初始化成功），并给出测试窗口内的解码帧数与丢帧数。

### 编码

| 编码格式 | 接口入口 | 输入格式 | 码流封装 | 验证基准 |
| --- | --- | --- | --- | --- |
| H.264 | `VAEntrypointEncSlice`（CBR、VBR、CQP） | NV12 | Annex-B | 1080p FFmpeg，GOP 60，IRQ 增量确认 |
| HEVC | `VAEntrypointEncSlice`（CBR、VBR、CQP） | NV12 | Annex-B | 1080p FFmpeg，GOP 60，IRQ 增量确认 |

硬件编码的判定标准：测试执行后 Iris 硬件中断计数产生有效增量，输出码流可正常解码，且 SPS 参数与请求的 profile 及分辨率完全匹配。

### 视频处理

| 接口入口 | 操作 | 输入格式 | 输出格式 | 验证基准 |
| --- | --- | --- | --- | --- |
| `VAEntrypointVideoProc`（`VAProfileNone`，需显式开启） | 任意比例最近邻缩放，可带裁剪与旋转 | NV12、P010 | NV12、P010 | `scale_vaapi` 输出与 swscale `flags=neighbor` 逐字节一致；裁剪与 `crop,scale` 逐字节一致；三个方向都与 `transpose` 逐字节一致 |

视频处理默认关闭，需设置 `V4L2_VA_VPP`。处理在 surface 的 DMA-BUF 上运行固定的 Vulkan 计算内核，只提供最近邻缩放一种操作，取点位置与 swscale 相同，起点是客户端给出的 surface region，先按客户端要求做顺时针旋转再缩放。NV12 与 P010 走同一套取点，按样本宽度各有一个内核；两端位深不一致的画面、镜像、混合、滤镜与色彩转换都会被拒绝，不做近似处理。 缩放下限是内核自身的 16x16，不再受解码节点最小尺寸约束，因此可以缩到缩略图大小。

## 运行环境

- 目标平台：高通 SM8550（HFI gen2 Iris VPU），aarch64 架构。
- 核心依赖：`libva` >= 1.1，`libdrm` >= 2.4.52。GPU 拷贝引擎需要 EGL、GLES 和 GBM 支持。
- 发行版基准：以 Fedora 44 作为基准验证环境；`packaging/` 目录下提供 Fedora 与 Arch 的打包规则。
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

安装包会自动放置驱动本体与 `msm_drv_video.so` 符号链接，mpv、VLC 与 GStreamer 等客户端可在未指定 `LIBVA_DRIVER_NAME` 的情况下直接加载该驱动。

### 诊断脚本

- `iris-device-check`：只读检测设备节点、固件版本、内核驱动绑定状态及支持格式。
- `iris-verify`：执行轻量级软硬件闭环测试，通过 Iris 中断计数验证编解码通路。

## 缓冲与发布模型

驱动根据客户端内存生命周期动态选择发布策略：

- **Direct**：客户端在解码后直接导出驱动分配的表面，实现零拷贝。
- **Import**：客户端在解码前传入预分配的 DMA-BUF 缓冲区，在应用了解码顺序补丁的内核上实现零拷贝。
- **Copy**：固件解码至驱动内部缓冲槽位，再由 GPU 复制至客户端表面。用于未打补丁的原版内核、VP9，以及同一 VA 上下文内 AV1 分辨率切换后的后半段。AV1 在应用了解码顺序补丁的内核上走 Import 零拷贝路径（2026-11-11 安装包实机验证，见 TEST-RESULTS.md）。

导入的 DMA-BUF 表面必须严格符合 CAPTURE 布局规范（单线性对象，128 字节对齐跨度）。不符合规范的表面直接返回 `VA_STATUS_ERROR_INVALID_PARAMETER`。

## 已知限制

- **VP9 零拷贝**：由于客户端在多帧合成包（alt-ref）完成阶段提前释放表面，当前维持走 GPU 拷贝路径。
- **AV1 动态分辨率切换与浏览器选择**：驱动内部维护八槽 DPB，在 `vaEndPicture` 直接提交当前帧。同一 VA 上下文分辨率改变后，若客户端继续沿用旧 CAPTURE 几何导出表面，驱动检测到跨距不匹配时改用 GPU 拷贝发布，避免跨距错位。Chromium 152 播放本地文件默认走内置软件解码；Chrome 156 在线页面与 Firefox 158 已验证硬解。
- **AV1 film grain 合成**：在 `endPicture` 直接拒绝。VA-API 画面参数区分 `current_frame`（无 grain 参考帧）与 `current_display_picture`（加 grain 显示表面）；驱动单次 CAPTURE 完成对应单个目标表面，支持合成需在用户态增加双目标表面发布路径。客户端以 side data 导出 grain 参数（`apply_grain=0`）时码流可正常硬解。
- **AV1 10-bit Profile**：已通过实机 1080p 60 帧逐帧 MD5 零拷贝 Import 探针验证，驱动当前未对外暴露 10-bit VA Profile。
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
| `V4L2_VA_VPP` | off | 启用 `VAProfileNone` / `VAEntrypointVideoProc` 入口（NV12/P010 最近邻缩放与旋转） |
| `V4L2_VA_DUMP` | off | 提交的码流单元转储目录路径 |

GStreamer 的 VA 插件（`libgstva`）在注册任何元素之前会按厂商字符串过滤 VA 驱动，本驱动的字符串不在白名单里：`gst-inspect-1.0 va` 报 `0 features`，`vaav1dec` 元素不存在。`GST_VA_ALL_DRIVERS=1` 是插件自带的绕过开关，设置后元素才会注册；libva 仍然通过 `msm_drv_video.so` 符号链接加载本驱动，不涉及任何 `LIBVA_*` 变量。`test/iris-gstreamer-gate.sh` 在平板上验证这两种状态。
