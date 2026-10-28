# Building with this repo (phoenix port)

Since 2026-10-24 the port changes live in two **dedicated local packages**
instead of local modifications of the shared/`generic` qcom-msm8916 packages:

- Kernel: `device/testing/linux-lenovo-phoenix` (fork of
  `linux-postmarketos-qcom-msm8916`, pkgrel numbering continues at 82).
- Device: `device/testing/device-lenovo-phoenix` (fork of the local additions
  that used to sit in `device-qcom-msm8916`, numbering carried over 8-r9).
- Both generic packages were restored to the pristine upstream snapshot
  (fdf5ba4 / origin/main of the local pmaports clone) — they build again for
  any other msm8916 device without our patches.
- Complete package mirrors live in this repo under `aports/` (see
  `aports/README.md` for the repo↔pmaports correspondence; the two known
  radioactive-sync spots — the DTS drift fix and the venus patch shape —
  are both resolved as of 2026-10-25: DTS synced during the split; venus
  v9 wired into pmaports (pkgrel 83) and on-device verified (decode pass;
  clock-rate observability is moot on 8939 — single shared RCG, §13).
- Unchanged local pkgs: `firmware-lenovo-phoenix` (-venus/-modem),
  `phoenix-usbgadget`, `main/lk2nd` dtb patch, `main/campix`.

Rule of thumb: **repo holds the source files, pmaports holds the wiring**
(APKBUILD edits). Proprietary blobs never enter either git repo —
except the pmaports *working tree*, which is local-only by nature.

After any pmaports edit:

```
pmbootstrap checksum <pkg>   # never hand-write sha512sums
pmbootstrap apkbuild_parse <pkg>   # must print no errors
pmbootstrap build <pkg>
pmbootstrap install --password postmarketos
./split-image.sh <combined.img> ./images
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
pmbootstrap sideload linux-lenovo-phoenix   # pushes the apk over SSH and apk-adds it into the running system
```
(reboot after; used successfully for r66 → build #67 under the old
`linux-postmarketos-qcom-msm8916` name).

⚠️ **Post-split snapshot**: the currently-flashed rootfs pair still carries
the OLD packages (`device-qcom-msm8916` + `linux-postmarketos-qcom-msm8916`
@ kernel #83). See §12 for the one-time transition.

Bump `pkgrel` on every change, otherwise `install` reuses the cached
package silently.

## 1. Kernel: `device/testing/linux-lenovo-phoenix` (own fork, 2026-10-24)

Fork of `linux-postmarketos-qcom-msm8916`; same source tarball (cached
distfile), config renamed `config-lenovo-phoenix.aarch64` (byte-identical
except five local `CONFIG_` deltas: `CHARGER_SMB347`, `VIDEO_OV5693`,
`VIDEO_OV13850`, `DRM_PANEL_NOVATEK_NT35532`, `QMMIO not set`), aarch64-only.
The upstream generic package was restored to pristine (upstream pkgrel=0);
none of these rows apply to it anymore.

| repo file | pmaports change |
|---|---|
| `msm8939-lenovo-phoenix.dts` | `source=` entry + sha; `prepare()` copies it to `arch/arm64/boot/dts/qcom/` and registers `msm8939-lenovo-phoenix.dtb` in the qcom `Makefile` |
| `panel/panel-novatek-nt35532.c` | `source=` + sha; `prepare()` copies to `drivers/gpu/drm/panel/`, appends `Makefile` obj line |
| `panel/panel-novatek-nt35532.kconfig` | appended to `drivers/gpu/drm/panel/Kconfig` in `prepare()`; enable `CONFIG_DRM_PANEL_NOVATEK_NT35532=m` in `config-*.aarch64` |
| `power/qcom-pon.c` | `source=` + sha; `prepare()` overwrites `drivers/power/reset/qcom-pon.c` (poweroff fix) |
| `power/smb358-status.c` + `.kconfig` | **wired out (r60, 2026-10-18)**: superseded by upstream `CONFIG_CHARGER_SMB347=m`, which matches the same `summit,smb358` node (two drivers must not race); charging indication now comes from the `smb347-usb` power supply. Reason + plan: `otg/OTG.md`. r60 alone froze `online` at the probe value — see next row |
| `power/smb347-smb358-input-detect.patch` | `source=` + sha (auto-applied, r61): the upstream driver assumes SMB347's IRQSTAT_E layout (input-UV STAT BIT0 **+ separate IRQ latch BIT1**); SMB358 has no separate latch bit, so the cached `usb_online` never refreshed and `online` froze at the probe value. Patch: derive the input state from live IRQSTAT_E BIT0 on every IRQ pass, enable the INOK (input-OK) STATUS_IRQ latch `CFG_STATUS_IRQ BIT2` so *inserting* the cable raises an event, and acknowledge IRQSTAT_A/B/F latches that would otherwise keep the STAT pin asserted (25/s re-fire loop observed on this board). Divergence details + bit map: `otg/OTG.md` |
| `power/smb347-smb358-otg-i2c-mode.patch` | `source=` + sha (auto-applied, r66): mainline `hw_init` clears `CFG_OTHER[7:6]` (OTG control mode) unless `summit,enable-otg-charging` is set — erasing the OTP-burned "OTG via I2C command" (0x80) mode, after which the `usb-vbus` regulator's CMD_A OTG bit is ACKed but silently produces no boost. The patch arms 0x80 in `smb347_probe()` right after the `usb-vbus` regulator registers (so the arming is tied to exactly "the board drives OTG VBUS through this driver"); auto-OTG semantics unchanged. Proven live by wire-write experiment: 0x80 enumerated a Stadia controller from the phone; 0x4?/0x00 never powered VBUS. `otg/OTG.md` |
| `power/pm8916-bms-vm-ocv-observer.patch` | `source=` + sha; applied automatically by `default_prepare` (ab/BUILD auto-applies `*.patch`) |
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

## 2. Device: `device/testing/device-lenovo-phoenix` (own package, 2026-10-24)

Own package holding exactly the former local additions of the generic
package (which is again pristine upstream). `modules-initfs`
adds `panel-novatek-nt35532`, restores `msm`; deviceinfo codename
`lenovo-phoenix`, dtb narrowed to `qcom/msm8939-lenovo-phoenix`, screen
1080×1920; `depends=` dropped `firmware-qcom-adreno-a300` (this board only
uses a420) — everything else carried over verbatim (8-r9 numbering).

| repo file | pmaports change |
|---|---|
| `30-gpu-firmware.files` (pattern from archived `device-arrow-db820c`) | `source=` + sha; `package()` installs to `/usr/share/mkinitfs/files/` so a420 fw lands in initramfs |
| `audio/msm8916-audio-init.sh` | installed `0755` to `/usr/libexec/`; needs `alsa-utils` in `depends=`. 2026-10-22: `CARD` fixed `phoenix`→`msm8916` (the QDSP6 sndcard's model — the old name died with the direct-LPASS card) |
| `audio/90-msm8916-audio.rules` | installed to `/usr/lib/udev/rules.d/` (runs the script when the card appears) |
| — (was `ucm2/phoenix/phoenix.conf`/`HiFi.conf`) | **removed 2026-10-22**: hand-rolled UCM matched the dead "phoenix" card name (PipeWire looks up `ucm2/<cardname>` = `ucm2/msm8916`). Replaced by `soc-qcom-msm8916-ucm` in `depends=` (msm8916-mainline/alsa-ucm-conf @3deea2860; its enable sequence = the same proven control set incl. `RDAC2 MUX RX2`) |

`depends=` additions (local): `alsa-utils`,
`firmware-lenovo-phoenix-venus`, `firmware-lenovo-phoenix-modem`,
`soc-qcom-msm8916-rproc`, `soc-qcom-msm8916-ucm` (2026-10-22 音频修复),
`q6voiced` (pulls `msm-modem`,
`msm-modem-uim-selection` → `rmtfs libqmi qmi-utils`), `modemmanager`
(2026-10-24, pkgrel=9 —— base-ui/fbkeyboard 不拉 ModemManager 本体,
"由 postmarketos-base-ui 依赖带入"旧口径撤回).

**OpenRC 开机启动接线 (2026-10-24, device pkgrel 8→9)**: `package()` 直接
`ln -s /etc/init.d/{modemmanager,rmtfs,msm-modem-uim-selection}` 到
`/etc/runlevels/default/` —— phoenix-usbgadget 的 `local` 链接同款模式
(纯包数据, 无 post-install 脚本时序问题; 存量已手动启用的 rootfs 升级时
幂等 no-op)。不再需要 MODEM.md 之前记录的"每刷必手动 rc-update"。全放
default runlevel（上游 soc-qcom-modem-openrc 口径：uim-selection initd 的
`need rmtfs`/`before modemmanager` 是同 runlevel 语义；设备 2026-10-16
dmesg 时间线 mpss t≈60s = `rmtfs -s` 在 default 拉起 mss 的行为）。
q6voiced 不在此接线，仍归上游 `soc-qcom-msm8916-rproc-openrc` 的
post-install。背景与一次性补齐命令：`MODEM.md` "OpenRC 开机启动项"。

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
| `camss/ov5693-stock-24m.patch` (r86) | kernel pkgrel 80→**81**（**kernel #82，已上机验证**）: 前摄切"全 stock 24MHz"。bisect r84-r85 全负 → 换 plan B：DTS MCLK1 19.2→24 MHz、endpoint link-frequencies 400 MHz（=800 Mbps/line，与 ov5693.c menu 对齐）、stock sunny_q5v41b 全表 stream-on 直接应用、FIXED_PPL 2672/PIXEL_RATE 159048960。上机后 RDI 前摄拍照闭环（R87/R88 真实场景 raw ✓，BGGR 软件渲染正常）。 |
| `camss/camss-vfe41-per-line-cal.patch` (new, r89) | kernel pkgrel 81→**82**（构建 **#83**，待用户构建）: **双传感器 per-line Stage-B 定标**。r89 在 #82 上零内核改动打通前摄 PIX ISP（media-ctl 使能 ispif1→vfe0_pix，B2 出真彩 NV12 2592x1944；TP B2_PASS、前摄 WB 定标 260/128/163 → 墙 \|U-V\|=0.39；cc=0 恒等矩阵最中性且目视自然 → 用户定为前摄推荐默认）。但共享模块参数是 OV13850 调校、前后摄共享会产生"切换相机必须改 sysfs"的问题。本补丁在 stream-on 时读 PIX sink 的使能链接（media pad walk → ISPIF line id：0=后摄链、1=前摄链），按源链选择参数组：后摄/共享 = `pix_wb_gain_*`(246/128/172)+`pix_cc_mode=1`（不变），前摄 = **新参数 `pix_front_wb_gain_r/g/b`（默认 260/128/163）+ `pix_front_cc_mode`（默认 0 恒等）**；demosaic 插值归一化同步用 per-line 增益。两组均 0644 可运行时覆写；未使能链接（空闲）回退共享组；RDI/raw 路径完全不受影响。验证：6.12.1 基座按 APKBUILD 顺序 chain+新补丁 patch -p1 应用后**字节级可复现**（cmp 通过）；**已构建 #83 上机全验证**（r90A 前摄零 sysfs 直拍墙 \|U-V\|=1.94/2.00/1.95 三帧、R90C 后摄 full-band 彩条 rdi/pix 各路不混；r91 重启流 boot 闭环：campix 自启武装 RPIX + 默认曝光注册，双摄 boot 裸拍 mean 51.7 可用）。**pmaports pkgrel=82 = 设备 #83，一致。** |

## 10. campix 包: `main/campix`（userland，r91 收官）

英 sources of truth（双份，内容 cmp 收敛）：
- 开发态：`camera-ref/tools/campix/`（`setup.sh`、`mg68h.py`、`camera` CLI、
  `r91_cap.sh`、`r91probe.sh`、`APKBUILD`、init 单元、confd、README）。
- 构建态：`~/.local/var/pmbootstrap/cache_git/pmaports/main/campix/`
  （本体目录，全部 root 规范路径；已提交 pmaports `712a8b5`，
  2026-10-25 血统重排后，原 `cac9434`）。

| repo 文件路径 | 安装路径 |
|---|---|
| `setup.sh`、`mg68h.py` | `/usr/lib/camera/` |
| `camera` | `/usr/bin/camera` |
| `campix.initd` | `/etc/init.d/campix`（OpenRC，`rc-update add campix default`） |
| `campix.service` | `/usr/lib/systemd/system/`（abuild 禁 /etc/systemd） |
| `campix.confd` | `/etc/conf.d/campix`（`CAMPX_BOOT=rear|front`） |
| `README` | `/usr/share/doc/campix/` |

状态与收敛：
- pmaports `pkgrel=1` **从未构建**（sha 已对齐 boot 默认曝光版 setup.sh）。
- 设备 = `campix-0.1.0-r0` 安装 + 一次性 `sudo cp` 同值 setup.sh（语义
  收敛、文件未收敛）；固件侧无残留（/usr/local 无内容）。
- 收敛命令（用户终端，fido 交互）：
  `pmbootstrap build campix && pmbootstrap sideload campix --arch aarch64`
  （`--arch` 只收合法架构值，noarch 不是 sideload 合法值）；
  build 后亦可交助手本地推送 `apk add`。
- boot 行为：武装 RPIX/FPIX 全链 + **boot 默认曝光注册**（rear
  exp3200/gain248、front 1600/248，驱动唤醒重放语义，无常电）；
  细节与硬事实见 `CAMERA.md` §14/§15/§16，生态调查 §15。

## 11. 音频: 耳机双耳有声但不分左右 (2026-10-22)

症状（QDSP6/modem 模式，2026-10-16 起回归）：耳机双耳都有声音，但左右声道
播同一内容；LineageOS 下同机左/右正常。direct-LPASS 时代（2026-10-14 parked
谜团）则是"单线交织 dual-mono"——两者根因不同，见下。

根因（证据链）：
- **Q6 卡名失配**：切 modem/QDSP6 后 snd card model= `msm8916`
  （`msm8916-modem-qdsp6.dtsi` `qcom,model="msm8916"`），仓库手写的
  udev 脚本 `CARD="phoenix"` 全部 `|| true` 静默失败，`ucm2/phoenix`
  也匹配不上（PipeWire 按 `ucm2/<cardname>` 查找）→ 混音器控制一整层
  失效。
- **`RDAC2 MUX` 停在复位默认 'RX1'**：HPHL DAC 硬连 PDM_RX1（左），
  HPHR DAC 本应由 `RDAC2 MUX=RX2` 吃 PDM_RX2（右）；失配下右耳 DAC 吃的
  是左路 PDM_RX1 → 双耳同音。上游通用 UCM（soc-qcom-msm8916-ucm
  @3deea2860）的 Headphones 序列正是含 `RDAC2 MUX RX2` 的同一套控件，
  前提是该包真的被装上且卡名能匹配。
- 10-14 parked 之谜（顺带解密）：direct 时代 I2SCTL=0x4400 =
  spkmode SD0 + spkmono STEREO——上游 `lpass-cpu` 无 dai 子节点时默认
  MODE_8CH→2ch 降级为 SD0 单线交织，CDC 侧 tap 按线分路（RX1←SD0、
  RX2←SD1），单线下无法分 slot → dual-mono，非 codec 硬件问题；
  Q6 模式 DTSI `qcom,sd-lines=<0 1>` 双线分路正确。

| repo 文件 | pmaports 变更（device pkgrel 7→**8**） |
|---|---|
| `audio/msm8916-audio-init.sh` | `CARD="phoenix"`→`"msm8916"`（附注释）；sha512 重算 |
| `ucm2/phoenix/`（已 `git rm`，git 历史留档） | APKBUILD source/package 删除 `phoenix.conf`/`HiFi.conf`；`depends=` 显式加 **`soc-qcom-msm8916-ucm`**（27-r0，edge 已发版） |

验证产物：`~/.local/var/pmbootstrap/packages/edge/aarch64/device-qcom-msm8916-8-r8.apk`
（含新脚本+规则、无 ucm2/phoenix）。构建注意：普通 `pmbootstrap build`
卡在 zap 阶段 `sudo losetup --json`（FIDO 触碰提示污染 stdout），
用 `--lax` 跳过 zap 绕过。

部署与上机验证（待用户执行）：
- `pmbootstrap install --password postmarketos`（或单独
  `pmbootstrap sideload device-qcom-msm8916` 后重启）。
- 快速判据（装包前也可先单点复现）：
  `amixer -c msm8916 cset name='RDAC2 MUX' RX2 && speaker-test -c2 -twav`
  ——翻到 RX2 后双耳立刻分左右即确认根因。
- 若翻转后仍不分左右（兜底 fork 路径）：播放中单点读
  `0x07709000`（Pri MI2S I2SCTL），期待 spkmode=QUAD01(5)+双线而非 SD0。

## 12. 设备拆分: device-lenovo-phoenix + linux-lenovo-phoenix (2026-10-24)

背景：此前所有 phoenix 改动都以"本地未提交改动"方式寄居在两个上游
通用包 `device-qcom-msm8916` / `linux-postmarketos-qcom-msm8916` 里，
上游任何重建/覆盖都会把端口改动抹掉，也不符合"通用包必须保持 pristine"
的常理。本节为拆分定局记录。

### pmaports 工作区变更（~/.local/var/pmbootstrap/cache_git/pmaports）

| 动作 | 内容 |
|---|---|
| **新增** `device/testing/linux-lenovo-phoenix/` | 31 文件（APKBUILD + `config-lenovo-phoenix.aarch64` + 22 补丁 + 源文件）。_flavor=`lenovo-phoenix`，arch=aarch64（去掉 armv7 + 其 config），去掉 `replaces=`，tarball distfile 复用上游名（cache_distfiles 已有 ~230MB 同字节文件，sha 一致，不重下载），其余（prepare/build/package）与通用包本地末态逐行相同；pkgrel=82 连续 |
| **新增** `device/testing/device-lenovo-phoenix/` | 7 文件（APKBUILD、deviceinfo、kernel-cmdline.conf、modules-initfs、30-gpu-firmware.files、msm8916-audio-init.sh、90-msm8916-audio.rules）。deviceinfo codename=`lenovo-phoenix`、dtb=qcom/msm8939-lenovo-phoenix、screen 1080×1920；depends 去掉 `firmware-qcom-adreno-a300`（本板只有 a420）；kernel 子包 extlinux (+ efi 同型保留) depends=`linux-lenovo-phoenix`；pkgver/pkgrel=8/r9 延续 |
| **恢复上游** | `device-qcom-msm8916`、`linux-postmarketos-qcom-msm8916` 两个目录整体 `git restore --source=fdf5ba4`（本地克隆的 origin/main，即本地 7 个 phoenix 提交之前的快照）→ 通用包回到上游 4 文件原状（device pkgrel=0 无本地叠加），任何上游行为不再被端口"wiring"污染 |
| **注释更新** | `device/testing/firmware-lenovo-phoenix/APKBUILD` 头注释 device-qcom-msm8916 → device-lenovo-phoenix |
| **配置切换** | `pmbootstrap config device lenovo-phoenix`（kernel=extlinux、UI fbkeyboard 不变；workdir.chroot 会按新设备重建 rootfs chroot） |
| **验证** | `pmbootstrap build device-lenovo-phoenix` 通过（edge/device-lenovo-phoenix 8-r9 产出，含 runlevel 符号链接/mkinitfs files/deviceinfo 后缀剥离全部正确）；`linux-lenovo-phoenix` 由用户完整构建并上机（见下"上机验证"；r83 venus v9 接线后再构建/sideload 亦通过）；APKBUILD sha512 与文件逐一核对通过，`pmbootstrap apkbuild_parse` 无错 |

### repo 变更（本仓库）

| 动作 | 内容 |
|---|---|
| **新增** `aports/` | 两个包的完整功能镜像（`aports/device-lenovo-phoenix/` 7 文件、`aports/linux-lenovo-phoenix/` 31 文件）+ `aports/README.md`（拆分对照表、事实来源、偏差清单、迁移注意） |
| **同步** `msm8939-lenovo-phoenix.dts` | 仓库旧版（r84，19.2 MHz MCLK）→ r86 部署版（24 MHz、link-freq 400 MHz，构建 #82 上机验证）；此前仓库↔pmaports 漂移 |
| **改** `firmware-lenovo-phoenix/APKBUILD` | 头注释同 pmaports 侧 |
| **改** `README.md`/`BUILD.md` | 包名更新 + 拆分记录（本节） |
| **接线** `power/venus-msm8939-res.patch`、`power/venus-v1-vcodec-clks.patch` | 2026-10-25 把仓库 69b09c7「v9 final shape」换入 pmaports `linux-lenovo-phoenix`（pkgrel 82→83 + 接线注释，checksum 重算，aports 镜像同步）；语义差与上机结果见 §13，**同日上机通过** |

### 上机验证（2026-10-24/25，用户报告）

拆分后的新包（`device-lenovo-phoenix` + `linux-lenovo-phoenix`）刷机后
**系统正常启动，功能看起来正常** —— 拆分本身收工；此后的 venus v9
接线见 §13（接线时刻起部署内容不再与 #82 逐字节等同）。

### 存量 rootfs 一次性迁移（二选一）

- **推荐：整刷**。`pmbootstrap install` 全新产出（device= lenovo-phoenix）
  → `./split-image.sh` → 成对 flash boot/root（image 纪律不变）。
- **增量 sideload**（当前运行系统，内核 #83 不变）：设备上
  `apk del device-qcom-msm8916 linux-postmarketos-qcom-msm8916 &&
   apk add device-lenovo-phoenix`（包装时 kernel-extlinux 会拉入
  linux-lenovo-phoenix；**内核包互斥**——LOCALVERSION 仍为 `-msm8916`，
  /lib/modules 同名目录冲突，不能共存）；然后主机侧重出 boot 镜像对刷。

### pmaports git 状态（2026-10-25 血统固化：e6ff10d + 5 条签名提交）

上游新基 `e6ff10d`（相对拆分前所用旧基 `fdf5ba4` 仅增 `cf40ea2`——
通用 msm8916 包 kconfig 更新）之上重放并已全部 GPG 签名：

```
ac2ac3b  lk2nd: dtb-files (fdtdir auto-discovery of the phoenix DTB)
e760188  firmware-lenovo-phoenix packaging (APKBUILD + provenance README)
6c735e0  phoenix-usbgadget: new local package (RNDIS auto-bind + USB subnet)
712a8b5  campix: new noarch userland package (boot-time camera pipeline arming)
c7ed987  split into dedicated device/kernel packages + venus v9 (pkgrel 83)
```

- 旧血统寄居通用包的三条提交（9ee4fd3 / af110f2 / 244fc92）按方案 A
  跳过：内容已全部由拆分提交自足；两个通用包目录保持上游 `cf40ea2`
  原样（重放时 kernel config 三方合并结果与上游逐字节一致，已复核）。
- 事故记录（2026-10-25 上午）：上游重放与提交动作并发撞车，30 个固件
  blob 曾进入废提交 `b5c672a`——已弃；对象经 reflog 仍可达约 90 天，
  **勿**跑 `git gc --prune`。
- 纪律：`git commit` 前必查 `git status --porcelain`，固件 blob 永不
  入库；blobs 保持未跟踪（`device/testing/firmware-lenovo-phoenix/`
  下 mba/modem/venus 共 30 个 `.mbn`/`.b0x`），仅作本地 distfile。
- `main` 领先 `origin/main` 5 条提交且未 push；是否推送由用户决策。

## 13. Venus v9 接线：final shape 换入 linux-lenovo-phoenix（2026-10-25，上机通过）

背景：r60..r82 部署的 venus 两个补丁是仓库 `630e3a7`（"full upstream-shape
power logic (clean retest)"）的字节级拷贝；次日仓库 `69b09c7`（"venus v9
final shape (TRIGGER, split vcodec clocks)"）重排了 vcodec 时钟数组，但该
重排从未进 pmaports、从未上机。2026-10-24 深度核查（两侧分别对 pristine
6.12.1 打补丁、diff 生成码并用 `patch --dry-run`/实际 patch 验证）确认
差异真实且为行为级，用户决策接线并自行构建上机。

### 语义差（核查结论）

| 项 | 部署旧 shape（= 630e3a7） | 仓库 v9（= 69b09c7） |
|---|---|---|
| `msm8939_res` vcodec 时钟数组 | 两数组同填 `{ "vcodec0_core", "vcodec1_core" }`，`vcodec_clks_num=2` | `vcodec0_clks={ "vcodec0_core" }`、`vcodec1_clks={ "vcodec1_core" }`，`num=1` |
| `core_power_v1` 上/下电 | 单次 `vcodec_clks_enable(vcodec0_clks)`（循环 2 项把两颗钟都开） | 两数组各一次（净 enable/disable 集合不变，仅 fail unwind 标签更细） |
| **`core_clks_set_rate()`（唯一行为差）** | `vcodec1_clks[0]` 绑定同一个 "vcodec0_core" 句柄 → `GCC_VENUS0_CORE1_VCODEC0_CLK` 只被 enable、**从未 set_rate**（停 boot 默认频率） | `vcodec1_clks[0]="vcodec1_core"` → 第二子核时钟也随 freq_tbl 变频（133.33 MHz @1080p30/720p，266.67 MHz @1080p60） |

不变性事实：`VIDC_VCODEC_CLKS_NUM_MAX=2` 两种 shape 都结构合法；
`gcc-msm8939-venus-trigger.patch` 两侧字节一致（69b09c7 的 TRIGGER 部分
已在部署）；69b09c7 对 DTS 只改注释（已随拆分进包）；v9 补丁头部的 `#`
注释 GNU patch 干净跳过（实测 rc=0）；旧 deploy 侧 venus H264 硬解
一直工作 → 测试期望是"不回退 + CORE1 频率开始跟随"。

后记（上机后核实 `gcc-msm8939.c`，即 r83 上机同一份源码）：上表
"唯一行为差"在 8939 硬件上**实际为无差**——venus 路径只有一颗可变频
RCG `vcodec0_clk_src`，三颗分支钟（"core"/CORE0/CORE1）全部共享它
且都带 `CLK_SET_RATE_PARENT`，无论 set_rate 打在哪颗 branch 上，落点
都是同一颗 RCG；详见下文"上机结果"。

### 接线动作（本机完成，不构建）

- pmaports：两个 v9 补丁换入 `device/testing/linux-lenovo-phoenix/`；
  APKBUILD `pkgrel 82→83` + r83 接线注释；`pmbootstrap checksum` 重算
  （tarball 复用 cache，无下载）+ `apkbuild_parse` 无错。
- 仓库：`aports/linux-lenovo-phoenix/` 同步 APKBUILD + 两补丁，
  `diff -rq` 全部 31 文件与 pmaports 一致。

### 上机步骤（构建由用户执行）

```
pmbootstrap build linux-lenovo-phoenix
pmbootstrap sideload linux-lenovo-phoenix
# 设备重启
```

venus 是模块（`CONFIG_VIDEO_QCOM_VENUS=m`），模块级变更 sideload+重启
即可；vmlinuz 内容不变，无需重刷 boot 分区。

### 设备验证点

1. probe 与时序回归：`dmesg | grep -i venus` —— 正常 probe、无
   `-EBUSY`/stuck clock（v9 上电顺序与旧 shape 相同：domains → core
   clks → vcodec0 → vcodec1 → HW 模式交接）。
2. 第二核时钟变频（本次差异之核心）：`grep -iE vcodec
   /sys/kernel/debug/clk/clk_summary`，看 `GCC_VENUS0_CORE0_VCODEC0_CLK`
   / `GCC_VENUS0_CORE1_VCODEC0_CLK` 两行 rate——旧 shape 下 CORE1 播放/
   停播恒为 boot 值；v9 预期播放 1080p@30 或 720p 时两颗都到
   133.33 MHz、1080p@60 时 266.67 MHz、停播回落。
3. 功能回归：1080p/720p H264 硬解照常、无 "output_manager pool is
   NULL"、IRQ 计数随播放持续增长。

### 上机结果（2026-10-25，用户报告）

- **功能：通过**。1080p@30 与 1080p@60 硬解均正常、无回归，用户判定
  通过（"不影响使用，算通过吧"）。
- **验证点 2（时钟变频）：未观察到任何 venus 相关行 rate 变化**
  （播放中亦无，含各 vcodec 分支行）。
- 定论（事后核实 `gcc-msm8939.c`，与 r83 上机同一份源码）：8939 GCC
  的 venus 路径**只有一颗可变频钟** `vcodec0_clk_src`（RCG @0x4C000，
  freq 表 133.33/200/266.67 MHz）；"core"（GCC_VENUS0_VCODEC0_CLK）与
  `gcc_venus0_core{0,1}_vcodec0_clk` 三颗 `clk_branch2` 分支钟全部
  共享它且都带 `CLK_SET_RATE_PARENT`——无论 set_rate 打在
  "vcodec0_core" 还是 "vcodec1_core"，最终都落在同一颗 RCG 上。旧
  shape（同一 branch 打两遍）与 v9（另一 branch 打一遍）在 8939 上
  **频控行为等价**，且物理上不存在"两颗 vcodec 核心钟不同频"的可能。
  ∴ v9 相对旧 shape 在这块硬件上没有频控差，"CORE1 从未 set_rate
  （跑 XO）"的动机推论在 8939 拓扑上不成立（分支钟显示的就是共享
  父钟的频率）；"看不到频率变化"与等价性自洽。
- v9 仍收编：语义等价但更贴上游 msm8939 形态、fail unwind 更细，且
  本次上机实证无回归。若日后想要频变证据：播放 1080p@60 时盯
  `vcodec0_clk_src` 行 rate 是否切到 266.67 MHz（可选，不影响结论）。

### 收敛（已按"通过"落定）

- v9 收编：repo 与 pmaports 双侧归一为 v9 形态（pkgrel 83 即上机版），
  `aports/README.md` 偏差表销账；设备侧结论由用户自行记入
  `BOOT-STATUS.md`。
- 若日后再回退：pmaports 两补丁恢复 `630e3a7` 版 + `pmbootstrap
  checksum` + pkgrel 再 bump，aports 镜像重新同步。


## 14. 电池 OCV/IR 观察器（2026-10-26，待上机）

现象与根因见 `BOOT-STATUS.md`「电池 OCV/IR 观察器重写」一节：
VM-BMS 只有端电压，旧逻辑把 `vbat - 100mV` 当 OCV 直接查表，负载阶跃
（含拔电 4.35V 浮充 -> OCV）会让百分比几秒内掉几十点，静置又弹回。

- 驱动：`power/pm8916-bms-vm-ocv-observer.patch` 取代
  `power/pm8916-bms-vm-fake-ocv-snap.patch`。核心是
  `I_est = (OCV - VBAT) / Rbatt` 后把电流积分成 OCV 位移；负载阶跃只改
  瞬时电流，不再瞬时改 OCV/容量。另含 FIFO 两样本平均 + 2s EMA 上报
  真实端电压（去掉 -100mV）、容量 0.05%/s 限速（大偏差 0.2%/s）、
  resume 用 S3 OCV 重锚、probe 移到 psy/info 就绪后再申请 IRQ。
- DTS：`battery` 节点新增
  `factory-internal-resistance-micro-ohms = <126000>`（下游
  `qcom,default-rbatt-mohm = <126>`），作为观察器的 Rbatt。
- 接线：pmaports `device/testing/linux-lenovo-phoenix/` 已换入新补丁、
  删除旧补丁、同步 DTS/APKBUILD，`pkgrel 83->84`；`pmbootstrap checksum`
  重算、`apkbuild_parse` 通过；repo `aports/` 镜像与 pmaports `diff -rq`
  一致。
- 离线验证：6.12.1-msm8916 树 `drivers/power/supply/pm8916_bms_vm.o`
  单编通过（aarch64 交叉 gcc），`msm8939-lenovo-phoenix.dtb` 编译通过，
  补丁 `patch -p1` dry-run 通过。
- 上机步骤：`pmbootstrap build linux-lenovo-phoenix` +
  `pmbootstrap sideload linux-lenovo-phoenix`，重启。验证点：拔电、
  大负载、静置时百分比连续（允许缓慢滑动，不允许秒降/回跳）；
  低电量在 1~2% 附近关机。
- r85 开机锚点修正（2026-10-27，pkgrel 84->85，待上机）：本机常几天不
  suspend，S3 OCV 寄存器可能是上次（甚至充电浮充时）的旧值，导致每次
  reboot 都从 100% 起步。probe 不再使用 S3；第一条 FIFO 用端电压 +
  IR 余量（放电 +150mV / 充电 -100mV）起步，前 60s 用最高端电压 +30mV
  向上修正。suspend 仅在未充电时强制 S3；resume 只有确实测过 S3 才重锚。
  补丁 `power/pm8916-bms-vm-ocv-observer.patch` 已更新，sha 已手工同步
  （`pmbootstrap checksum` 因当前环境 sudo 不可用未跑，APKBUILD 内 sha
  与文件逐一核对一致）。
- r86 充电电流（2026-10-27，pkgrel 85->86，待上机）：battery 节点补上下游
  快充配置 `constant-charge-current-max-microamp = <1800000>`（下游
  `fastchg-current-max-ma = 1800`）和 `charge-term-current-microamp =
  <100000>`（`iterm-ma = 100`）。此前 smb347 对缺省项不写寄存器，SMB358
  跑 OTP 默认值，充电偏慢；旧电量算法又会在 CV 段快速跳 100，掩盖了真实
  速度。BMS 开机充电的 IR 余量也改为按该 FCC 计算（1.8A*126mOhm≈227mV），
  不再固定 100mV。验证：充电时 `smb347-usb/constant_charge_current` 应约
  1800000µA，10 分钟涨幅约 8~9 点。
- r87 VBAT 偏移 + 满电开机锚点（2026-10-28，pkgrel 86->87，待上机）：满电
  `voltage_now` 读到 4.448V（SMB358 硬件浮充 4.34V），证明 VM-BMS 原始值高
  ~100mV；原 fork 的 `-100mV` 是校准偏移。FIFO 与 S3 OCV 路径都恢复该偏移。
  另外 r86 在 EOC 后仍按 1.8A 减 227mV IR，锚出 87%；现在只有 charger
  `STATUS==CHARGING` 才减，FULL/NOT_CHARGING 不减。满电重启应为 100，拔线
  不再跳到 94。
- r88 对齐 TWRP（2026-10-28，pkgrel 87->88，待上机）：读写 PMIC 持久化的
  `BMS_SOC_REG`/`BMS_OCV_REG`（B0/B1，Android 与 TWRP 同款）：probe 读取上次
  OCV/SoC，运行中每 10s 刷新；TWRP 的 97% = 存储的 OCV 4.28V。寄存器无效或与
  端电压差 >600mV 才回退到 S3 OCV（±400mV），最后才是端电压 + IR。回退充电时
  只有 `vbat+20mV < float`（明显在 CC）才减 `FCC*Rbatt`。
