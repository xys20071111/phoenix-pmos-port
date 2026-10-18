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
| `camera/ov13850.c` + `camera/ov13850.kconfig` | `source=` + sha; `prepare()` copies into `drivers/media/i2c/` + appends Makefile/Kconfig (r54): phoenix rear camera driver, mainline-style skeleton; global init = stock vendor table (libmmcamera_ov13850.so decode, 240 entries) with stream-critical regs on Rockchip values, per-mode regs = Rockchip 2112x1568@30 + 4224x3136@7.5; probe detect() has the same 2.5 s retry window as ov5693 (rear DT reg 0x36 is still a placeholder — address found by bus scan) |
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
