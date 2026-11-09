# sheng 视频加速：面向小米平板 6S Pro（SM8550）的 Qualcomm Iris VA-API 后端

[English](README.md) | **中文**

面向小米平板 6S Pro（`sheng`，骁龙 SM8550）的 ARM64 Linux 视频加速套件：给高通
Iris VPU 的 stateful V4L2 解码器/编码器写的 libva / VA-API 驱动、它需要的四个
`qcom-iris` 内核补丁，以及 Fedora RPM / Arch PKGBUILD 打包。在这唯一一台合格目标机上
（Fedora 44、自编译 `ianchb/sm8550-mainline` 内核），Google Chrome、Electron 和
Firefox 走硬件解码，H.264/HEVC 画面从固件直接进 Chrome 自己的缓冲区、中间不拷贝；
VP9 仍走 Copy。驱动由这台机器上所有 VA-API 客户端共用，只在本设备上有实测证据。

## 为什么要有这个项目

ARM64 Linux 上的官方 Google Chrome 编译时开了 `use_vaapi=true`、关了
`use_v4l2_codec`：二进制里唯一的硬解入口是 libva。预编译的 Electron 应用和
Firefox 的 FFmpeg hwaccel 同样如此。没有 VA-API 驱动，这些客户端只能靠 CPU
解码，平板上放一段 1080p HEVC 就是发热和掉电的问题。

Iris 是 stateful 解码器：固件自己解析码流，接口只有 OUTPUT 队列、CAPTURE
队列和几个控制项。之前没有人写过把 VA-API 解码调用翻译成 Iris 会话的 libva
后端，这个仓库补的就是这一层。Chrome 是验收客户端；FFmpeg、GStreamer、mpv、
Firefox 用的是同一个驱动，也都欢迎，只是它们本来就有各自的原生 V4L2 路径。

## 现在能用到什么程度

下表每一行都对应 [TEST-RESULTS.md](TEST-RESULTS.md) 里一次记录在案的硬件
运行，每次都带 boot ID、内核版本、模块哈希和驱动哈希。合格环境：骁龙 SM8550
平板、Fedora 44、Google Chrome 152。

| 编码 | Chrome | Chrome 里走的路径 | 逐比特验证 |
| --- | --- | --- | --- |
| H.264（CB/Main/High） | 硬件解码 | Import，零拷贝 | 逐帧 MD5 与原生 V4L2 解码器一致；Fluster JVT-AVC_V1 72/135；63 条未通过的已在 TEST-RESULTS.md 逐条分类，其中 49 条是内核直接拒绝的场编码码流 |
| HEVC Main | 硬件解码 | Import，零拷贝 | 逐帧 MD5 一致；Fluster JCT-VC-HEVC_V1 141/147 |
| HEVC Main10（P010） | 硬件解码 | Import，零拷贝 | 10-bit 逐帧 MD5 一致 |
| VP9 Profile 0 / 2 | 硬件解码 | GPU blit（见限制） | 逐帧 MD5 一致，8-bit 和 10-bit |
| AV1 Profile 0 | 硬件解码 | GPU blit（见限制） | 逐帧 MD5 一致，alt-ref、无 alt-ref、8-bit 和 10-bit 都验证过 |

"硬件解码"的标准：Chrome 的 media 面板报告 `VaapiVideoDecoder`、平台解码器
标志为真、30 秒窗口内有解码帧数且零丢帧。节点被打开、GPU 进程在跑，这些都
不算证据。

同一个 display 也提供 `VAEntrypointEncSlice`，走内核在解码节点旁边暴露的编码
节点。Chrome 不用它，用它的是 FFmpeg、GStreamer，以及任何向 libva 要编码
entrypoint 的程序。

| 编码 | Entrypoint | 输入 | 输出 | 验证 |
| --- | --- | --- | --- | --- |
| H.264 Constrained Baseline / Main / High | `VAEntrypointEncSlice`，CBR、VBR 或 CQP | NV12 | Annex-B | 1080p `ffmpeg h264_vaapi`，300 帧 GOP60 验收（TEST-RESULTS.md） |
| HEVC Main | `VAEntrypointEncSlice`，CBR、VBR 或 CQP | NV12 | Annex-B | 1080p `ffmpeg hevc_vaapi`，300 帧 GOP60 验收（TEST-RESULTS.md） |

"硬件编码"的标准：这轮跑完 Iris 的 IRQ 计数有增量、码流解码无错误、SPS 里
的 profile、level 和分辨率与请求一致。一个 VA context 只返回了字节，不算证据。

## 需要什么

- 带 `qcom-iris` 解码器的 SoC（HFI gen2；SM8550 是合格过的那一个），aarch64。
- libva ≥ 1.1、libdrm ≥ 2.4.52；GPU 拷贝引擎需要 EGL/GLES/GBM；运行时需要
  `v4l-utils`（启动器按驱动名找解码节点，因为节点编号每次开机都会变）。
- 任何能加载 libva 模块的 Linux 发行版。Fedora 44 是合格过的；提供 Fedora
  和 Arch 的打包。Debian / Ubuntu 没有包，也没做过合格验证。
  - 在这两个发行版上编译会要 `libgbm-dev`，那是 Mesa 的包，锁死某个版本的
    `libgbm1`，装上去可能把厂商改过的图形栈降级。编译实际只需要头文件、
    `gbm.pc` 和链接期那个 `libgbm.so` 符号链接，而 Meson 把 GBM 当可选项：
    没有它就编出走 CPU 拷贝的驱动，GPU 拷贝那条路不编进去。要么手工从
    `.deb` 里取这三样，要么不装 GBM，接受慢一点的拷贝路径。
- 可选：五项 Iris 内核改动（按解码顺序出帧、CAPTURE 池上限 64、编码器去块
  滤波控制项、编码器的 `REQUEST_SYNC_FRAME` 控制项、出错完成保留时间戳），已
  rebase 后放在 [`packaging/kernel/`](packaging/kernel/README.md)，附只重编模块
  的步骤。原版内核上驱动会探测到补丁不在，照样能用，只是 Chrome 每帧多一次
  GPU 拷贝，拿不到零拷贝；编码器上非默认的去块请求会被拒绝，按需 IDR 则改成
  挪 intra period；损坏的画面要等一次排空或同步超时才报错，而不是当场报错。

## 安装和使用

Fedora 上从包安装：

```sh
git archive --format=tar.gz --prefix=libva-v4l2-iris-0.1.0/ \
    -o ~/rpmbuild/SOURCES/libva-v4l2-iris-0.1.0.tar.gz HEAD
rpmbuild -ba packaging/fedora/libva-v4l2-iris.spec \
    --define "source_commit $(git rev-parse HEAD)" \
    --define "tracked_sha256 $(git ls-files -z | xargs -0 sha256sum | sha256sum | awk '{print $1}')"
sudo dnf install ~/rpmbuild/RPMS/aarch64/libva-v4l2-iris-*.rpm
```

Arch 用 `packaging/arch/PKGBUILD`。两种包都会把驱动装进 libva 的驱动目录，
旁边再放一个 `msm_drv_video.so` 符号链接，同时装上 `iris-device-check` /
`iris-verify` / `iris-configure-clients` 三个诊断和配置命令、它们共用的节点解析
文件，以及记录源码提交和产物哈希的安装清单。这个链接是给 libva 自己找驱动用的：
这颗 SoC 报的是 `DRIVER=msm_dpu`，没有它，不设 `LIBVA_DRIVER_NAME` 的客户端
（mpv、VLC、GStreamer）根本打不开驱动。

没有启动器，也没有桌面项。Chrome 就按平常的方式启动：

```sh
google-chrome-stable
```

不需要开关、不需要环境变量、也不需要单独的 profile：libva 靠那个符号链接找到
驱动，节点由驱动自己按 `iris_driver` 名字解析。`test/iris-browser-acceptance.sh`
就是这么跑的——把 `LIBVA_DRIVER_NAME` 和两个节点覆盖变量从环境里去掉，要求 GPU
进程确实映射了本驱动并持有节点。手动确认时播放视频并打开
`chrome://media-internals`：解码器一栏必须是 `VaapiVideoDecoder`，且
`kIsPlatformVideoDecoder: true`。

装完先跑这两个，再谈别的：

```sh
iris-device-check          # 只读：节点、固件、驱动、暴露的编解码、该走哪条路
iris-verify                # 解码和编码各跑一遍，看 Iris 中断计数有没有动
```

`iris-verify` 会自己生成测试片源，不需要外部素材；它检查 libva 在没有任何环境
变量时能不能打开驱动、解码和编码是否真的落在硬件上、编出来的流能不能解回来。
mpv 和 Firefox 的配置是可选的，`iris-configure-clients` 只改当前用户自己的
`~/.config/mpv/mpv.conf` 和 Firefox profile 里的 `user.js`，不碰 `/etc`，也不
碰 Chrome。

不打包、直接从源码树装：

```sh
meson setup build && meson compile -C build
sudo ./scripts/install-system.sh build       # 驱动进 libva driverdir，启动器进 /usr/local
LIBVA_DRIVERS_PATH=$PWD/build/src LIBVA_DRIVER_NAME=v4l2 vainfo   # 或者先这样试一下
```

`scripts/uninstall-system.sh` 按安装清单把这种安装撤掉。实验室里的完整流程见
[`docs/build-and-deploy.md`](docs/build-and-deploy.md)。

包装好之后其他 VA 客户端不用配，靠的就是那个 `msm_drv_video.so` 链接。唯一的
例外是 GStreamer：它的 VA 插件维护一份厂商白名单，遇到不认识的驱动直接拒绝，
所以 `va` 系列元素只有在 `GST_VA_ALL_DRIVERS=1` 时才会注册。这是 GStreamer 的
门槛，不是驱动的；设上之后 `vah264dec` / `vah265dec` 走的都是 Iris。

## 它怎么解码

一个所有权模型，三种发布策略，按每个 surface 的客户端行为自动选。CAPTURE
池归驱动所有。

- **Direct**：解码之后才导出或映射的 surface（FFmpeg、GStreamer、mpv）直接
  拿走解码完成的那个 CAPTURE 槽位。真正的零拷贝。
- **Import**：客户端在首次解码*之前*就导出的 surface（Chrome 会预先导出整个
  池），驱动把它的缓冲区排进 CAPTURE 队列、只给那一张图，固件直接解进去。
  需要内核补丁提供的解码顺序出帧；按编码从实测证据里选（H.264、HEVC、
  Main10）。
- **Copy**：同样是预先导出的 surface，由 GPU blit（Adreno 上的 EGL）从驱动
  的槽位拷进一块文件描述符永不变化的稳定缓冲区。VP9 和原版内核走这条。

Import 靠两条实测出来的规则成立：同一时刻固件手里只有一张客户端图（按它
的完成次数各挂一个缓冲区）；已提交的图一张都不能跳过。完成结果落到邻居
缓冲区里的话，两个 surface 一起判失败，不发布。契约、状态机和 trace 词表在
[`docs/architecture.md`](docs/architecture.md)，测量数据在
[TEST-RESULTS.md](TEST-RESULTS.md)。

客户端也可以直接把自己的内存交进来：`vaCreateSurfaces2` 带上
`VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2`（或更老的 `_DRM_PRIME`），surface
就建在一块线性的 NV12/P010 DMA-BUF 上，驱动不再分配。缓冲区必须已经是固件
写入的 CAPTURE 布局——128 字节 pitch（P010 是 256），chroma 紧跟在按 32 对齐
的亮度行之后。驱动把 fd dup 一份，客户端随时可以关掉自己的。建出来的 surface
走上面那套策略（解码顺序内核上 Import，否则 Copy），也可以直接当编码输入。

## 已知限制

下面每条都写清楚三件事：什么条件下发生，结果是什么，为什么。依赖内核的条件
直接点名那个内核特性。

- **VP9 走 GPU 拷贝路径。** 一个 VP9 alt-ref 访问单元会解出两张图。
  - 只挂一个缓冲区时，每遇到一次 alt-ref，会话卡约 500 ms：固件拿不到放
    第二张图的地方，就不返回第一张。
  - 挂两个不同图片的缓冲区时，固件自己挑，大约四分之一的完成结果落错位置。

  现在会话改成给一张图按它的完成次数挂缓冲区：同一个客户端缓冲区排两次，
  固件能挑的仍然只有这张图自己的内存。目标机上 FFmpeg 这条路是精确的
  （515/515、没有错位的完成结果、framemd5 与软解一致），但 Chrome 会在一张图
  完成之前就释放它的 surface：768 次排队里有 564 次被放弃，541 个完成结果被
  丢掉，同样形状的 H.264 对照只有 2 和 4。Chrome 仍报 480 帧、0 丢帧，这是
  静默降级，不算通过。浏览器这条路干净之前 VP9 继续走拷贝（TEST-RESULTS.md，
  2026-10-24）。
- **AV1 会压后一张图。** VA 不提供 `refresh_frame_flags`，所以翻译器只能等
  下一个访问单元的参考表，才知道这张图刷新了哪些槽位。
  - 客户端领先一张图解码时（FFmpeg 带滤镜或下载、Chrome 都是），感觉不到
    任何差别。
  - 客户端先提交一张图、再 `vaSyncSurface` 等它、然后才提交下一张时，会等到
    同步上限。流结束时上限到期，压后的图按最后一帧释放；后面如果还有非帧内
    图，会以一个明确的错误失败，不会解出错像素。

  固件对隐藏帧（`show_frame=0`）不返回像素。会话按解码顺序认出它并照常发布，
  因为 FFmpeg 之后会用 `show_existing_frame` 把它显示出来。因此 Import 仍然
  只走拷贝路径。AV1 Profile 0 同时覆盖 8-bit 和 10-bit 4:2:0，两种都声明：
  P010 还是 NV12 由固件按码流自己决定。film grain 合成会被拒绝，因为内核没给
  用户态任何途径要求固件做这件事。
- **原版内核上 Chrome 走 Copy。** 解码顺序控制项来自内核补丁。没有它时会话
  运行在显示顺序模式：同步等待有上限，超时后做一次 STOP/LAST/START 排空。
  Import 在这个模式下被拒绝。
- **导入的 surface 必须符合 CAPTURE 布局。** 这个布局是：一个 object、线性
  modifier、驱动规定的 pitch、chroma 在规定偏移上。客户端 DMA-BUF 只要带
  tiling modifier、多于一个 object、用自己的 pitch，或者 chroma 不在规定偏移
  上，`vaCreateSurfaces2` 一律返回 `VA_STATUS_ERROR_INVALID_PARAMETER`，不会
  解进错误的像素。PRIME_1 没有 modifier 字段，那里驱动只能信
  `VA_SURFACE_EXTBUF_DESC_ENABLE_TILING` 说的"线性"。
- **Chrome 153 有 HEVC seek 回归，这是上游缺陷。** Chromium 153 默认打开了
  `ExtendedVideoBitstreamValidation`。共用的 `H265Decoder` 把每次 seek 后的
  第一个 SPS 当成配置变更，合成器会一直显示 seek 之前的画面。
  [CFM880/iris-vaapi](https://github.com/CFM880/iris-vaapi) 在 SM8150 上定位
  并提交了 [Chromium issue 563075803](https://issues.chromium.org/issues/563075803)。
  本项目没有复现，因为目标机跑的是 Chrome 152。CFM880 现在把这个回归标为
  在 Chrome 156.0.8078.47 里已修好（`stale_repeats = 0/445`，不加任何
  workaround flag），说可以去掉那个开关；他们的报告也提醒，launcher 包装
  脚本偷偷注入同一个 flag 会让 A/B 看起来像是修好了。用 153 的话，加
  `--disable-features=ExtendedVideoBitstreamValidation` 启动，或者留在 152；
  156 及以后两者都不需要。
- **只在一台设备上合格过。** 上面所有数据都来自一台 SM8550 平板。其他 Iris
  gen2 SoC 应该能用，但目前没有证据。Arch 打包提供了，没有验证过。
- **按需 IDR 在目标机上是直接控制项，在 stock 内核上是挪 GOP。** 分两种
  情况：
  - 节点有 `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` 时，驱动直接写这个控制项。
    用 `packaging/kernel/0003-*` 编出来的模块就属于这种情况，同步帧和它的
    参数集都由固件自己写。
  - 节点没有这个控制项时（stock 7.2.6 模块），驱动把这一张图的 intra period
    设成 0，入队前改回去，并补上上一次的 SPS/PPS。固件只在自己写的同步帧上
    带参数集。

  两种方式的切点都在 `-force_key_frames` 要的位置。下一个自动 IDR 在这一次
  之后一个 GOP 处，而不是原来的排期上。走 GOP 那条路时，每张请求 IDR 的图要
  多写一次控制项。因此逐帧都请求反而比直接开 intra-only 贵。客户端要一张
  intra picture 但没打 IDR 标志（FFmpeg 就会这样）时，驱动按 IDR 请求处理，
  因为固件手上只有发 IDR 的那一个杠杆。
- **不支持 B 帧。** 内核最多允许 1 个，而延迟返回的 CAPTURE 会破坏一进一出
  的模型。序列参数里 `ip_period > 1` 的 context 直接返回
  `VA_STATUS_ERROR_INVALID_PARAMETER`。
- **不支持 10-bit、VP9、AV1 编码。** 编码节点的 OUTPUT 队列只列 NV12 和
  QC08C。因此 HEVC Main10 和 VP9 Profile 2 喂不进去。
- **不声明 packed headers，所以打开编码器时没有 extradata。** SPS/PPS 由固件
  自己写，驱动不会再把客户端那一份塞进去。`VAConfigAttribEncPackedHeaders`
  回的是 `VA_ENC_PACKED_HEADER_NONE` 而不是 "不支持"：0 表示这个属性认得、
  只是不收 packed header，`VA_ATTRIB_NOT_SUPPORTED` 才是客户端根本不能问。
  - FFmpeg 能处理。它报个警告，把参数集留在码流里，再从第一个包提取
    `avcC` / `hvcC`。所以直接输出 `.mp4` 是可用的：H.264 的 extradata 43
    字节，HEVC 124 字节。
  - GStreamer 的 VA 编码器在那一项回 `VA_ATTRIB_NOT_SUPPORTED` 时会拒绝，
    因为它分不清这是驱动不认识还是自己问错了。回 0 能过它的检查，所以
    `vah264enc` / `vah265enc` 能启动起来。两者在目标机上各编了 90 帧，
    码流解码干净；第二个卡点是完成结果被认领两次，现在统一走
    `claim_coded()`（TEST-RESULTS.md，2026-10-24）。
- **编码是流水线的，`-async_depth` 生效。** `vaEndPicture` 把图排进队列就
  返回。码流由 `vaSyncBuffer` 在 coded buffer 上取；不调同步的客户端在
  `vaMapBuffer` 时取。FFmpeg 看到 `vaSyncBuffer` 就走异步路径。因此
  `-async_depth` 可以把编码器的四个输入槽全部用上：目标机上 1080p HEVC 从
  depth 1 的 190 fps 升到 depth 4 的 372 fps，各档位输出逐字节相同。对输入
  surface 调 `vaSyncSurface` 仍然有效，含义是同一张图的完成。VA context 的
  分辨率是固定的，换分辨率要新建 context。
- **不支持 multi-slice、tiles、ROI 和 `-max_frame_size`。** FFmpeg 请求这些
  会直接报错。驱动不会静默忽略它们，自己守同样的规矩：picture 参数里 slice
  多于一个、tiles、wavefront 编码、加权预测、scaling list、chroma QP 偏移、
  多个参考帧索引，一律返回 `VA_STATUS_ERROR_UNIMPLEMENTED`。序列头由固件写，
  参考结构和量化由固件定。交出一段没照做的流，比直接失败更糟。
- **picture 的约定是查出来的，不是假设的。** 固件只有一个参考帧和一个改图类型
  的杠杆，所以驱动拿客户端的要求去对编码器能做的事：
  - 非 IDR 的 I picture，或者 HEVC CRA，都会被提升成 IDR 请求。Iris 只暴露
    `FORCE_KEY_FRAME`，它发的是 IDR；另一条路是把这张图当 P 编，码流的顺序
    就错了。FFmpeg 在 `-idr_interval` 大于 0 时会提交这种图：
    `hw_base_encode.c` 给两个 IDR 之间的 GOP 边界发 `FF_HW_PICTURE_TYPE_I`，
    `vaapi_encode_h264.c` 随之用 `slice_type = 7` 配 `idr_pic_flag = 0`。
    默认间隔是 0，所以光给 `-g` 走不到这条路。
  - inter picture 的参考列表里必须写上一张图的 reconstruction surface，别的
    一律返回 `VA_STATUS_ERROR_UNIMPLEMENTED`。
  - 码流必须从 intra picture 开始，标成 non-reference 的图直接拒。
  - 只接受一张覆盖整幅的 slice：`macroblock_address` /
    `slice_segment_address` 为 0，且 slice 覆盖整幅（HEVC 看
    `num_ctu_in_slice`）。B slice，以及 HEVC `coding_type` 和 slice 类型对不上
    的，都拒。
- **slice 的去块滤波参数要么照做，要么报错。** VA 的 slice 参数是客户端唯一
  能提出关掉滤波或调偏移量的地方。内核有这些控制项时，驱动把
  `disable_deblocking_filter_idc`、alpha/beta 转给
  `V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE`/`_ALPHA`/`_BETA`，HEVC 有对应的
  tc/beta。
  - 出厂 SM8550 内核没有这些控制项。这时固件永远按默认的 0/0 滤波，任何非
    默认请求都返回 `VA_STATUS_ERROR_UNIMPLEMENTED`，不会交出一段没照做的流。
  - 加控制项的内核补丁在
    [`packaging/kernel/`](packaging/kernel/README.md)。

  V4L2 里去块是每个 context 一个值，VA 里是每个 slice 一个值。驱动只看第一个
  slice，同一个 context 里改动要新建 context。

## 开发

```sh
meson setup build-local && meson compile -C build-local
sh test/run-static-checks.sh
meson test -C build-local --print-errorlogs
git diff --check
```

硬件门禁在目标机上跑：`test/iris-browser-acceptance.sh --clip …`（Chrome
证据）、`test/iris-matrix.sh`（与原生解码器比逐帧 MD5）、
`test/iris-structure-matrix.sh`、`test/iris-dynamic-resolution.sh`、
`test/iris-concurrency-soak.sh`、`test/iris-zero-copy-suite.sh`。可能导致
固件复位的实验走 `scripts/iris-experiment-guard.sh`。完整清单见
[`test/README.md`](test/README.md)；贡献者和编码代理的规则在
[CONTRIBUTING.md](CONTRIBUTING.md) 和 [AGENTS.md](AGENTS.md)。

## 许可证与致谢

MIT，含 LGPL-2.1 覆盖的部分，与上游相同；见 `COPYING`、`COPYING.MIT`、
`COPYING.LGPL`。

- 本项目 fork 自 [mxsrc/libva-v4l2](https://github.com/mxsrc/libva-v4l2)，
  后者又源于 Bootlin 的 libva-v4l2-request。VA 和 V4L2 的脚手架来自那里；
  `src/` 里的 stateful Iris 实现、浏览器验收门禁和打包是本 fork 的工作。
- `include/linux/` 内置的内核 UAPI 头文件带各自的 SPDX 标记
  （`GPL-2.0+ WITH Linux-syscall-note OR BSD-3-Clause`）。
- `packaging/kernel/` 用四个补丁携带五项 Iris 内核改动：四项来自
  [radxa-pkg/qcom-va-driver](https://github.com/radxa-pkg/qcom-va-driver)
  （Radxa，Xilin Wu；这个仓库 2026-10-26 之前叫 `strongtz/libva-v4l2`），
  一项是上游 `REQUEST_SYNC_FRAME` 提交的编码器部分（Wangao Wang，高通）。
  它们改的是 GPL-2.0 的内核代码，许可证也是 GPL-2.0，与驱动本身不同。
  HEVC 显式 RPS 的 slice 重写沿用了同一项目的做法。

## 环境变量

驱动读取的每个开关都列在这里，且只在 `src/util/options.cc` 一处读取；
`test/iris-env-doc-check.sh` 会在这张表、英文 README 的表和该文件三者不一致
时让构建失败。这些全部是诊断用途，启动器一个都不设。

| 变量 | 默认值 | 用途 |
| --- | --- | --- |
| `LIBVA_V4L2_VIDEO_PATH` | 自动探测 | 指定解码节点，不再扫描 `/dev/video*` 找 `iris_driver` 解码器。 |
| `LIBVA_V4L2_ENCODER_PATH` | 自动探测 | 指定编码节点，不再扫描 `/dev/video*` 找 `iris_driver` 编码器。指到非 Iris 编码节点会让 display 初始化失败。 |
| `V4L2_VA_TRACE` | 关 | 在 stderr 上输出逐帧 trace，任何值都表示开启。记录格式见 `docs/architecture.md`。 |
| `V4L2_VA_SYNC_TIMEOUT_MS` | `2000` | `vaSyncSurface()` 的等待上限，`50..60000`。不覆盖时，冷会话的第一帧最多等 `30000` ms 让固件起来。 |
| `V4L2_VA_COPY` | `gpu` | 往客户端预先导出的 surface 里发布画面时用的引擎：`gpu`（Adreno 上的 EGL blit）或 `cpu`。 |
| `V4L2_VA_PUBLISH` | `auto` | 对所有 surface 强制 `copy`、`direct` 或 `import` 发布。仅诊断用；`auto` 按客户端导出行为逐 surface 选择（解码后导出走 Direct，解码顺序内核上的预导出走 Import，其余走 Copy）。会话无法满足 `import` 时退回 `copy`。 |
| `V4L2_VA_DUMP` | 关 | 把每个提交的访问单元写进这个目录。 |
