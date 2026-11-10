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
| VP9 | Profile 0, Profile 2 | Copy（GPU 复制） | 逐帧 MD5 一致 |
| AV1 | Profile 0 | Import（零拷贝，解码顺序内核） | 逐帧 MD5 一致，2026-11-10 实机验证 |

硬件解码的判定标准：Chrome 媒体控制台明确报告 `VaapiVideoDecoder`，启用平台解码器标识，并在 30 秒测试窗口内零丢帧。

### 编码

| 编码格式 | 接口入口 | 输入格式 | 码流封装 | 验证基准 |
| --- | --- | --- | --- | --- |
| H.264 | `VAEntrypointEncSlice`（CBR、VBR、CQP） | NV12 | Annex-B | 1080p FFmpeg，GOP 60，IRQ 增量确认 |
| HEVC | `VAEntrypointEncSlice`（CBR、VBR、CQP） | NV12 | Annex-B | 1080p FFmpeg，GOP 60，IRQ 增量确认 |

硬件编码的判定标准：测试执行后 Iris 硬件中断计数产生有效增量，输出码流可正常解码，且 SPS 参数与请求的 profile 及分辨率完全匹配。

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
- **Copy**：固件解码至驱动内部缓冲槽位，再由 GPU 复制至客户端表面。用于未打补丁的原版内核、VP9，以及同一 VA 上下文内 AV1 分辨率切换后的后半段。AV1 在应用了解码顺序补丁的内核上走 Import 零拷贝路径（2026-11-10 实机验证，见 TEST-RESULTS.md）。

导入的 DMA-BUF 表面必须严格符合 CAPTURE 布局规范（单线性对象，128 字节对齐跨度）。不符合规范的表面直接返回 `VA_STATUS_ERROR_INVALID_PARAMETER`。

## 已知限制

- **VP9 零拷贝**：由于客户端在多帧合成包（alt-ref）完成过程中的表面生命周期管理限制，当前强制走 GPU 拷贝路径。
- **AV1 解码缓冲**：translator 需持有一帧，直到下一个 access unit 的参考帧表指出它刷新了哪些 DPB 槽位。`vaDestroyContext` 会在 session drain 前发布最后一帧，所以文件解码不需要等同步超时；VA-API 没有单独的 flush 入口，逐帧同步且不销毁 context 的客户端仍会在码流末尾走带边界的同步释放。Firefox 158 在解码线程里逐帧同步 surface，等待当前 surface 时下一个 access unit 无法提交，硬解 AV1 会按 fail-closed 拒绝后续帧（`AV1 stream continued after a picture was released at download time`），Firefox 再回退到软件解码；Firefox 的 H.264、HEVC、VP9 硬解已验证，包括禁用 AV1 的 YouTube live 页面和 B 站 HEVC live 页面，AV1 硬解客户端是 Chrome 和 FFmpeg。AV1 零拷贝 Import 已在解码顺序内核上验证。同一 VA 上下文内分辨率切换后，如果客户端继续按旧 CAPTURE 几何导出 surface，新几何无法通过 Import 投递：驱动会记录布局不匹配并改用 GPU 拷贝发布这些帧。
- **原版内核降级**：在缺少显示延迟控制或强制关键帧控制的内核上，解码降级为 GPU 拷贝，关键帧请求通过临时改动 intra period 实现。
- **编码高级特性**：仅支持单图单切片结构。多切片、分块（Tiles）、B 帧及 ROI 配置直接返回 `VA_STATUS_ERROR_UNIMPLEMENTED`。
- **封装头信息**：头信息声明为 `VA_ENC_PACKED_HEADER_NONE`，参数集随码流带内传输。

## 环境变量

| 变量名 | 默认值 | 作用说明 |
| --- | --- | --- |
| `LIBVA_V4L2_VIDEO_PATH` | probe | 强制指定 V4L2 解码节点路径 |
| `LIBVA_V4L2_ENCODER_PATH` | probe | 强制指定 V4L2 编码节点路径 |
| `V4L2_VA_TRACE` | off | 启用标准错误流执行轨迹追踪 |
| `V4L2_VA_SYNC_TIMEOUT_MS` | 2000 | 表面同步超时时间（毫秒） |
| `V4L2_VA_COPY` | gpu | 拷贝引擎选择（`gpu` 或 `cpu`） |
| `V4L2_VA_PUBLISH` | auto | 强制指定发布模式（`auto`、`direct`、`import`、`copy`） |
| `V4L2_VA_DUMP` | off | 提交的码流单元转储目录路径 |
