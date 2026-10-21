# Building with this repo (phoenix port)

All port changes live as **local modifications** in the pmbootstrap
`pmaports` checkout (`~/.local/var/pmbootstrap/cache_git/pmaports`).
Nothing here is upstream yet. This file maps every repo file to the
package change it requires, so a fresh checkout can be reconstructed.
Rule of thumb: **repo holds the source files, pmaports holds the wiring**
(APKBUILD edits). Proprietary blobs never enter either git repo —
except the pmaports *working tree*, which is local-only by nature.

After any pmaports edit:

```
pmbootstrap checksum <pkg>   # never hand-write sha512sums
pmbootstrap apkbuild_parse <pkg>   # must print no errors
pmbootstrap build <pkg>
pmbootstrap install --password postmarketos
./split-image.sh <qcom-msm8916.img> ./images
fastboot flash boot lk2nd.img        # one time (or after lk2nd rebuild)
fastboot flash -S 200M system ./images/boot-part.img
fastboot flash -S 200M userdata ./images/root-part.img
```

**Image discipline (learned 2026-10-18, do not "optimize"):**
every `pmbootstrap install` re-randomizes the rootfs partition UUID and
the boot image's cmdline (`pmos_root_uuid=`) references that UUID. The
split `boot-part.img`/`root-part.img` are a **pair from one install**:
flash both in the same round, or the new boot code hunts for a rootfs
UUID that does not exist and the device dead-ends before USB. To update
**only the kernel** (rootfs untouched), skip fastboot entirely:

```
pmbootstrap sideload linux-postmarketos-qcom-msm8916   # pushes the apk over SSH and apk-adds it into the running system
```
(reboot after; used successfully for r66 → build #67).

Bump `pkgrel` on every change, otherwise `install` reuses the cached
package silently.

## 1. Kernel: `device/testing/linux-postmarketos-qcom-msm8916`

| repo file | pmaports change |
|---|---|
| `msm8939-lenovo-phoenix.dts` | `source=` entry + sha; `prepare()` copies it to `arch/arm64/boot/dts/qcom/` and registers `msm8939-lenovo-phoenix.dtb` in the qcom `Makefile` |
| `panel/panel-novatek-nt35532.c` | `source=` + sha; `prepare()` copies to `drivers/gpu/drm/panel/`, appends `Makefile` obj line |
| `panel/panel-novatek-nt35532.kconfig` | appended to `drivers/gpu/drm/panel/Kconfig` in `prepare()`; enable `CONFIG_DRM_PANEL_NOVATEK_NT35532=m` in `config-*.aarch64` |
| `power/qcom-pon.c` | `source=` + sha; `prepare()` overwrites `drivers/power/reset/qcom-pon.c` (poweroff fix) |
| `power/smb358-status.c` + `.kconfig` | **wired out (r60, 2026-10-18)**: superseded by upstream `CONFIG_CHARGER_SMB347=m`, which matches the same `summit,smb358` node (two drivers must not race); charging indication now comes from the `smb347-usb` power supply. Reason + plan: `otg/OTG.md`. r60 alone froze `online` at the probe value — see next row |
| `power/smb347-smb358-input-detect.patch` | `source=` + sha (auto-applied, r61): the upstream driver assumes SMB347's IRQSTAT_E layout (input-UV STAT BIT0 **+ separate IRQ latch BIT1**); SMB358 has no separate latch bit, so the cached `usb_online` never refreshed and `online` froze at the probe value. Patch: derive the input state from live IRQSTAT_E BIT0 on every IRQ pass, enable the INOK (input-OK) STATUS_IRQ latch `CFG_STATUS_IRQ BIT2` so *inserting* the cable raises an event, and acknowledge IRQSTAT_A/B/F latches that would otherwise keep the STAT pin asserted (25/s re-fire loop observed on this board). Divergence details + bit map: `otg/OTG.md` |
| `power/smb347-smb358-otg-i2c-mode.patch` | `source=` + sha (auto-applied, r66): mainline `hw_init` clears `CFG_OTHER[7:6]` (OTG control mode) unless `summit,enable-otg-charging` is set — erasing the OTP-burned "OTG via I2C command" (0x80) mode, after which the `usb-vbus` regulator's CMD_A OTG bit is ACKed but silently produces no boost. The patch arms 0x80 in `smb347_probe()` right after the `usb-vbus` regulator registers (so the arming is tied to exactly "the board drives OTG VBUS through this driver"); auto-OTG semantics unchanged. Proven live by wire-write experiment: 0x80 enumerated a Stadia controller from the phone; 0x4?/0x00 never powered VBUS. `otg/OTG.md` |
| `power/pm8916-bms-vm-fake-ocv-snap.patch` | `source=` + sha; applied automatically by `default_prepare` (ab/BUILD auto-applies `*.patch`) |
| `power/qcom-iommu-resv-regions.patch` | same (adds `get_resv_regions` to qcom_iommu) |
| `power/venus-msm8939-res.patch` | same (msm8939 venus resources: dual-core clocks/domains, no `cp_*`) |
| `power/venus-v1-vcodec-clks.patch` | same (core power + clock sequencing on the 1xx path, backported from upstream) |
| `power/gcc-msm8939-venus-trigger.patch` | same (`HW_CTRL_TRIGGER` on venus core GDSCs + `BRANCH_HALT` on core clocks) |
| `modem/qcom-bam-dmux-register-netdevs.patch` | same (bam-dmux: register `wwan0..wwan7` netdevs at probe; MSM8939 2015 fw never announces DMUX channels at boot, so ModemManager never sees a net port and refuses to create the modem) |
| `camss/camss-msm8939-driver.patch` + `camss/msm8939-camss-dtsi.patch` | `source=` + sha (auto-applied); camss on msm8939: driver glue + `msm8939.dtsi` DTI additions (Step 1/2) |
| `camss/gcc-msm8939-camss-mclk.patch` | `source=` + sha; rewrites `ftbl_gcc_camss_mclk0_1_clk` — exact 19.2 MHz (front) / 24 MHz (rear) MCLK instead of the fork's miscomputed 17.78 MHz |
| `camss/ov5693-bringup-retry.patch` | `source=` + sha; **bring-up only** (r52): ov5693 chip-id read retries ~2.5 s so the power-up window fits a full `i2cdetect` scan. Remove once the front camera passes verification |
| `camera/ov13850.c` + `camera/ov13850.kconfig` | `source=` + sha; `prepare()` copies into `drivers/media/i2c/` + appends Makefile/Kconfig (r61 rework): phoenix rear camera driver, mainline-style skeleton; ALL register tables now verbatim from the stock sensor lib decode (libmmcamera_ov13850.so: 50 ms 0x0103 singleton + 217-entry init + 6 per-mode tables), 4-lane CSI2 with per-mode link freqs 300/320/600 MHz (exposes V4L2 LINK_FREQ/PIXEL_RATE per mode); probe detect() keeps the 2.5 s retry window (rear DT reg 0x36 = stock slave info decode) |
| a5xx `MODULE_FIRMWARE` | inline `printf` append in `prepare()` (no repo file; keeps a420 fw refs in `msm.ko`) |

## 2. Device: `device/testing/device-qcom-msm8916`

Shared generic package; our additions are marked local. `modules-initfs`
adds `panel-novatek-nt35532`, restores `msm`.

| repo file | pmaports change |
|---|---|
| `30-gpu-firmware.files` (pattern from archived `device-arrow-db820c`) | `source=` + sha; `package()` installs to `/usr/share/mkinitfs/files/` so a420 fw lands in initramfs |
| `audio/msm8916-audio-init.sh` | installed `0755` to `/usr/libexec/`; needs `alsa-utils` in `depends=` |
| `audio/90-msm8916-audio.rules` | installed to `/usr/lib/udev/rules.d/` (runs the script when the card appears) |
| `ucm2/phoenix/phoenix.conf`, `HiFi.conf` | installed to `/usr/share/alsa/ucm2/phoenix/` (PipeWire profile) |

`depends=` additions (local): `alsa-utils`,
`firmware-lenovo-phoenix-venus`, `firmware-lenovo-phoenix-modem`,
`soc-qcom-msm8916-rproc`, `q6voiced` (pulls `msm-modem`,
`msm-modem-uim-selection` → `rmtfs libqmi qmi-utils`; ModemManager comes
in via `postmarketos-base-ui`).

## 3. Firmware: `device/testing/firmware-lenovo-phoenix` (new, local-only)

Stock venus blobs (`venus.{mdt,mbn,b00..b04}` from the stock
`/system/vendor/firmware/`) installed to
`/usr/lib/firmware/qcom/venus-1.8/` via the `-venus` subpackage.
Stock modem blobs (`mba.mbn`, `modem.mdt`, `modem.bNN` extracted from
the modem/NON-HLOS partition with `fatread.py`) installed to
`/lib/firmware/` via the `-modem` subpackage (modem bring-up, see
`firmware-lenovo-phoenix/README.md`).
See `firmware-lenovo-phoenix/README.md`. Blobs live only in the pmaports
working tree; the repo keeps the APKBUILD template + README.

## 4. USB gadget factory: `device/testing/phoenix-usbgadget` (new, local-only)

| repo file | pmaports change |
|---|---|
| `usb/gadget.start` | own package (2026-10-18, RNDIS root cause: initramfs one-shot `setup_usb_network_configfs` sees no UDC when ci_hdrc probe is late behind the smb358 `vbus-supply` — full chain in `otg/OTG.md`). Ships the byte-identical live fix: `/etc/local.d/gadget.start` (phase 1
assemble configfs g1 immediately (no UDC needed) → wait/retry ≤60 s to
bind: on the r66 first boot the UDC was listed at boot+20 s but the
single bind echo still failed (gadget core not attach-ready yet),
leaving g1 unbound and USB dead until a manual bind minutes later —
so the bind now retries with read-back confirmation → usb0 up
172.16.41.1/24 → restart unudhcpd.usb0. Depends `unudhcpd`. Also ships `/etc/runlevels/default/local` symlink (pmOS base doesn't enable the `local` service; deterministic, no post-install timing tricks). Wired into every fresh rootfs via `pmbootstrap config extra_packages phoenix-usbgadget` |
Long-term upstream path: wait-for-UDC loop in `postmarketos-initramfs`
(`setup_usb_network_configfs`), then this package retires.

## 5. Bootloader: `main/lk2nd`

`0002-dts-msm8916-phoenix-dtb-files.patch` (repo root) added to `source=`
+ sha; makes `fdtdir` auto-discover the phoenix DTB (no extlinux fdt edits).

## 6. What upstreaming drops

In order: DTS + panel (to msm8916-mainline/linux), venus msm8939 series
(already upstream — then drop res/v1/gcc patches + DT compatible back),
smb358 (write a real driver), qcom-pon poweroff, audio UCM upstream,
lk2nd patch. Until then, everything above stays.

## 7. r67 定局补充 (2026-10-18 晚)

| repo file | pmaports change |
|---|---|
| `power/smb347-smb358-otg-i2c-mode.patch` | kernel pkgrel=66→67 (构建 #68，已 sideload 上机): probe 臂位(r66) + **regulator enable 重臂** + **CFG_OTHER 标 volatile**。0x65 破案： 芯片在 VBUS 下降沿自主把模式位翻回 0b01(PIN 死模式)， r66 一次性臂位无法自愈； r67 每次重新启用时重新臂位， 插拔回归采样 404/404 零漂移 |
| `usb/gadget.start` (v4) | phoenix-usbgadget pkgrel=2→3: v3 = **configfs `-s` 死守卫修复**(属性文件 stat.size 恒 4096 与内容无关， read-back 唯一神谕) + `/tmp/gadget.log` 开机取证(实测 boot-time bind i=0 rc=0)； v4 = usb0 → **172.16.42.1/24**(pmOS 规范网段) |
| `usb/unudhcpd.conf` (new) | 同包 pkgrel=3： 安装为 `/etc/conf.d/unudhcpd.usb0`， 显式 `UNUDHCPD_SERVER=172.16.42.1` / `UNUDHCPD_CLIENT=172.16.42.2` —— 修 unudhcpd-openrc 默认 41.x 服务器 / 42.x 客户端跨子网(USB 网 "No route to host" 的根因)； 修复后设备↔PC ping 0% loss |

镜像提醒: `./images/` 里的 boot-part/root-part pair 仍是 r66 安装时(~20:0x)的旧版，
不含 r67 内核/v4/conf。要"随时可整刷"需重跑 `pmbootstrap install` 生成新 pair
（rootfs UUID 重新随机化， **必须成对刷**）。

## 8. camera 链: r61 首帧 + VFE 写主控几何修复 (2026-10-19)

| repo file | pmaports change |
|---|---|
| `camss/camss-vfe41-frame-based-geometry.patch` (new) | kernel pkgrel 68→69 (构建 #70 待用户构建): gen1 ops 可选 op `wm_set_frame_geometry`（仅 vfe-4-1 实现），RDI frame-based 补写 WR_BUFFER_CFG —— stock msm_isp40 的 frame-based 分支写、主线从不写（实测 0x0），导致流出的帧每 2KB 窗丢 1KB（50% 恒零字节）。目标值 2112x1568 pBAA = **0x014A61F3**（330 8B 组 \| 1567\|burst3；r60 旧值 0x029461F3 系 4B 字误推，作废） |
| `camera/ov13850.c`（已在本机部署，本轮无改动） | 源已有 checksum 接线不变；r61 验证 init 对拍 215/216、m1 态 227/228（独差 AEC 0x350b） |
| `msm8939-lenovo-phoenix.dts`（4-lane，已部署） | 运行时 DT 已核验 `data-lanes=<0 2 3 4>` 生效 |

设备事故与铁律: camss MMIO 在 runtime-suspend 态裸读会总线 fault panic
（r61e 两次 reboot 的根因）；**只允许中流读**（/dev/mem mmap，qmmio 已移除、
busybox 无 devmem）。r61e/f 误用旧版 mediasetup 已重写并持久化。
详见 CAMERA.md r61 设备轮条目。

## 9. camera PIX (VFE ISP) r77: bayer demux 更正 + 伪 YUV 路线 (2026-10-20)

| repo file | pmaports change |
|---|---|
| `camss/camss-vfe41-pix-bayer.patch` (new, repo 首次落地) | kernel pkgrel 73→**74**（构建 #75，待用户构建）: r76 的 PIX patch 全面改写为 stock HAL ground truth（从开源 mm-camera VFE40 ISP 源恢复: `camera-ref/stock-src/mm-camera/`，repo `bcyj/android_tools_leeco_msm8996` @ f242858a）。核心 3 点: (1) bayer demux **period=1**（原硬编码 3 是 YUV 值）+ 按 CFA 起始相位选 even/odd cfg（SBGGR10=BGBGBG → 0xCA/0x9C；GBRG 0xAC/0xC9；GRBG 0x9C/0xCA；RGGB 0xC9/0xAC）；(2) bayer 时 MODULE_CFG 清掉 CHROMA_UPSAMPLE(bit3)（stock 对 bayer 关闭该 YUV-only 块）→ 0x08800004，YUV 路径不变 0x0880000C；(3) demux 7-word 块补 0x430/0x434（3D R-image gains）=0。demux gain 语义改为 Q7 12-bit（0x80=1.0，上限 ~31.9x；设备实测线性放大 1.5x/2.88x 通过）。 |
| 运行时 A/B 参数（免重编） | `demux_period`(0=auto)、`demux_even/odd`(原始 32 位)、`demux_gain0/1`(原始 32 位)、`pix_module_cfg`(0x18 全局覆盖)、`camif_width`(CAMIF pixels-per-line 覆盖)、`scale_in_div`(2=scaler 输入减半 → 2:1 水平放大)。全部在 **下一次 STREAMON** 才生效。 |
| 预期波形（判据） | TP 彩条: U=CH1 变 B 图案 [255,0,255,0,...]、V=CH2 变 R 图案 [255,255,0,0,...]、U≠V；Y=G 全宽。若仍折叠 → `camif_width`/`scale_in_div` 二分。之后用户态 R=V,G=Y,B=U 重建彩色（工具 `camera-ref/tools/nv12_recon.py` + `routing_validator.py`）。 |
| 编译验证 | 补丁在 `kernel-extract/ksrc-6.12.1` 上按 APKBUILD 顺序 driver→frame→pix 应用，`make M=drivers/media/platform/qcom/camss modules`（aarch64 cross-gcc 16.2）零告警通过；应用链与 pristine 副本双向验证干净。 |
| r78 结果（kernel #75 已上机验证） | **P1 = NEW_MODEL**：Y=全宽 G、U=B、V=R、U≠V，8/8 对 RDI 真值，无 seam/tiling/折叠；P2（显式 period=1）与 P1 sha 相同。色度通路固有 0.5x（GCHK_1 验证），WB 由 `demux_gain1` 执行：白墙标定字 **0x0173010E**（B 2.108x/R 2.895x）→ WALL_C U/G=0.9825、V/G=0.9798、0 削顶；微调 0x01750110 → 0.9888/0.9876。用户态 `nv12_recon.py`（R=V,G=Y,B=U + --auto-wb）已渲染出 8/8 色相正确的彩条与中性墙帧；证据 `camera-ref/frames/r78/`。详见 CAMERA.md §8。 |
| `camss/camss-front-instrumentation.patch` (new, r77-F) | kernel pkgrel 74→**75**（构建 #76，待用户构建）: 前摄诊断用 instrumentation，**默认行为不变**。stream-on 打 dev_info：CSID CORE_CTRL_0(lane_cnt/lane_assign)/CORE_CTRL_1(phy_sel/low nibble)/CID LUT/CID_n_CFG、CSIPHY1 lane mask/positions/settle、ISPIF INTF_INPUT_SEL/INTF_CMD_0/RDI0 CID mask/status；ISR 报 bit9 short-packet + 0x74。运行时 A/B 参数（默认=现状，STREAMON 生效）：`csid_lane_assign`/`csid_lane_cnt`/`csid_core_ctrl1_lo`（0x9 auto，试 15=0xF）、`csiphy1_lane_pos`（auto=<0 2>；A/B <0 1>=0x10,<1 2>=0x21,<0 3>=0x30,<2 3>=0x32）/`csiphy1_lane_cnt`、`ov5693` 的 `stock_mipi_regs`。补丁链 driver→ov5693-retry→frame→pix→front 交叉编译零告警（qcom-camss.ko + ov5693.ko，W=1 干净）。 | 
| `camss/camss-vfe41-pix-full-isp.patch` (new, r80) | kernel pkgrel 75→**76**（构建 #77，待用户构建）: 后摄 Stage-B 完整 ISP，增量于 pix-bayer v2。demosaic（0x440/0x518/0x520/0x568，OV13850 chromatix 真值）、WB（0x580 r72 增益）、CC（0x5D0 TL84 矩阵 + 恒等回退）、chromaEnh/RGB→YUV（0x640 TL84 系数 + kcb/kcr）、可选 LINEARIZATION/BLC（17 字 knee@0x37C + 36 字 DMI LUT bank0，12-bit BL {R,Gb,Gr,B}={69,66,65,69} 不除 4；模块字 +bit0 = B2 0x08822815）。全部 runtime 门控、默认关：`pix_full_isp` 0/1(B1)/2(B2)/3(B2+BLC)、`pix_linearization`、`pix_full_isp_cfg`、`pix_wb_gain_r/g/b`、`pix_demosaic_interp`、`pix_cc_mode`、`pix_cc_qfactor`、`pix_ce_mode`。实现时修正了文档 §2 字列表 4 处位域打包错误（保留位未留空），见 full_isp_stageB.md §10。验证：全补丁链 patch -p1 干净、qcom-camss.ko 交叉编译零告警（含 W=1）。 |
| `camss/camss-front-payload-diag.patch` (new, r81-F) | kernel pkgrel 76→**77**（构建 #78，待用户构建）: 前摄 payload 可观测性（增量于 front-instrumentation）。CSID total_pkts/ECC/CRC(0x90/0x94/0x98)、captured long/short hdr/ftr(0x6C/0x70/0x74/0x78/0x7C)、PIF MISR(0x80-0x8C)、IRQ_MASK+raw IRQ status，在 stream-on/延迟 100ms/stream-off 快照；gated stock 对齐 A/B：`csid_irq_mask`/`csid_short_pkt_reset`/`csiphy1_settle`/`csiphy1_rev3_quirk`/`ispif_stock_init`（默认全=现状）。背景：RDI0 SOF 以 30fps 到达 VFE 但无 payload，且 stock↔主线 CSID/ISPIF/CSIPHY 编码逐字节一致（R2/R3 关闭）。验证：7 补丁全链 apply 干净、qcom-camss.ko 零告警。**同构建**：`pix_full_isp` 编译默认 0→**2（B2 完整 ISP，真彩色）**，旧路径设 `pix_full_isp=0`。 |
| `camss/ov5693-4307-stock.patch` (new, r82) | kernel pkgrel 77→**78**（构建 #79，待用户构建）: **前摄 MIPI 数据类型修复**。r81-F 计数器证明前摄长包 DT=0x12（非 RAW10 0x2B），payload 长度正确（WC=0xCA8=一行 2592x10b）→ CSID LUT 不映射、无帧。主线 ov5693.c 模式表 `0x4307=0x31`，stock 表 `0x4307=0x30`（唯一已知输出格式差异）→ 本补丁改 stock 值。验证：全链 apply 干净、qcom-camss.ko + ov5693.ko 零告警。 |
| `camss/ov5693-stream-regs.patch` (new, r83) | kernel pkgrel 78→**79**（构建 #80，待用户构建）: **前摄运行时寄存器覆写**（纯诊断）。ov5693 新增 module param `stream_regs`（"REG=VAL,..." 十六进制，默认空=不变），在每次开流时、全局表/模式配置之后、0x0100=1 之前写入。背景：0x4307=0x30（stock 值）下 CSID 完全收不到长包；=0x31 时收到 3240 字节像素包但 DT 不受支持。用该参数可分组二分 stock 表寄存器/试 0x4300 取值，无需每次重建。验证：qcom-camss.ko + ov5693.ko 零告警。 |
| `camss/ov5693-stream-regs.patch`（已构建） | kernel pkgrel 79 / **kernel #80**：**已构建 r79 apk 并成功 sideload 到设备，等待重启加载**。内容：ov5693 运行时寄存器覆写参数 `stream_regs`（"REG=VAL,..."，开流前写入，默认空=不变），用于前摄 MIPI 输出格式的免重建二分。pmaports 与已构建 apk 一致（512 字节缓冲、≤30 对；sha512 已核对 30 文件全部匹配）。 |
