# Lenovo PHAB Plus (PB1-770N, phoenix) postmarketOS port

Mainline Linux port of postmarketOS for the Lenovo PHAB Plus
(Snapdragon 615 / MSM8939), booting via
[lk2nd](https://github.com/msm8916-mainline/lk2nd).

## Status (fbkeyboard UI boots, 2026-10)

Working: display (NT35532 driver, ported init sequence), backlight/brightness,
touchscreen (Goodix GT9xx), volume keys, hall sensor, vibrator, eMMC, SD slot,
USB networking + SSH (RNDIS gadget auto-bound at boot, pmOS-standard
172.16.42.0/24 — see `usb/`), battery voltage + percentage (OCV table) +
charging indication (SMB358), **USB OTG host mode** (SMB358 boost via the
usb-vbus regulator; Stadia controller + flash drive enumerate, plug/unplug
regression clean — 2026-10-18, see `otg/OTG.md`), accelerometer,
WiFi (WCN3680, dual-band),
GPU 3D (Adreno 405 / freedreno FD405), video decode (Venus H264), audio output (speaker mono + headphones dual-mono,
desktop via UCM), **modem: SMS send/receive + SIM/IMEI/operator**
(2026-10; see `MODEM.md`), poweroff/reboot.

Blocked (external, not our port): phone calls — this modem firmware has no
VoLTE and CS voice is equally dead on stock/LineageOS; the whole Q6 voice
chain (CS-Voice snd card, q6voiced) is wired up but unverifiable.

Parked: light sensor (probes, reads zero),
microphone (capture link presents, function unverified), headset buttons
(thresholds in, unverified), mobile data (bam-dmux netdevs exist, WDS/NM
path unverified — see `MODEM.md`), GPS, camera,
magnetometer (no mainline driver).

## Contents

- `msm8939-lenovo-phoenix.dts` — mainline device tree
  (modem enabled via `msm8916-modem-qdsp6.dtsi`).
- `modem/qcom-bam-dmux-register-netdevs.patch` — kernel patch: register all
  DMUX netdevs at probe (2016-era MSM8939 fw never announces channels;
  without net ports ModemManager refuses to create the modem).
- `panel/panel-novatek-nt35532.c` — NT35532 DRM driver
  (upstream base + phoenix init sequence + DCS backlight).
- `panel/panel-novatek-nt35532.kconfig` — Kconfig snippet.
- `power/qcom-pon.c` — upstream qcom-pon + poweroff handler
  (programs PON shutdown type; stock mainline poweroff reboots this board).
- `power/smb347-smb358-otg-i2c-mode.patch` — kernel patch (USB OTG):
  mainline smb347 hw_init erases the OTP-configured CFG_OTHER[7:6]=0b10
  (`OTG via I2C command`), so the CMD_A OTG bit was silently ignored since
  the first mainline boot. Patch arms it after probe, re-arms on every
  usb-vbus regulator enable, and marks CFG_OTHER volatile (the chip
  self-flips the mode bits on VBUS drops; regmap cache must not mask it).
- `power/smb347-smb358-input-detect.patch` — kernel patch: on the SMB358
  input undervoltage is a single IRQSTAT_E bit + INOK latch
  (`CFG_STATUS_IRQ` BIT2); without enabling it cable insertion is never
  latched/re-detected. Read-only monitor `smb358-status.c` superseded by
  the upstream smb347 driver at r60 (both matched `summit,smb358`).
- `usb/gadget.start` + `usb/unudhcpd.conf` — rootfs-side RNDIS fix: waits
  for the late ci_hdrc probe (initramfs one-shot misses it), assembles +
  binds the configfs gadget with read-back verification (configfs
  attribute files always stat 4096 — `[ -s ]` guards are dead code;
  `/tmp/gadget.log` holds boot forensics), and pins the 172.16.42.1/24
  subnet override for unudhcpd (stock unudhcpd-openrc defaults are
  cross-subnet). Packaged as local-only `phoenix-usbgadget`
  (see `BUILD.md` §7).
- `power/smb358-status.c` + `power/smb358-status.kconfig` — read-only
  Summit SMB358 charger monitor (ONLINE/STATUS; never writes config).
  Register map from the Oculus Go reference (see below). Superseded
  2026-10-18 by the upstream smb347 driver — see the
  `smb347-smb358-*` patch entries above.
- `power/pm8916-bms-vm-fake-ocv-snap.patch` — snap fake_ocv down on fast
  discharge (else SoC strands high after unplug).
- `audio/msm8916-audio-init.sh` + `audio/90-msm8916-audio.rules` — udev
  applies the proven mixer set when the sound card appears.
- `ucm2/phoenix/` — PipeWire/UCM profile (Speaker + Headphones + Mic).
- `0002-dts-msm8916-phoenix-dtb-files.patch` — lk2nd patch so `fdtdir`
  auto-discovers the phoenix DTB (no extlinux hand-edits needed).
- `panel-nt35532-skuk-initseq.c.inc` — init sequence extracted from downstream.
- `downstream-phoenix.dts` — decompiled Lineage DTB (reference).
- `downstream-pb1770m/` — downstream DTS excerpts (sensors, panel).
- `fatread.py` — minimal FAT12/16 reader (for modem partition forensics).
- `split-image.sh` — split a pmbootstrap combined image into
  boot/root partition images (validated byte-identical roundtrip).
- `BOOT-STATUS.md`, `WIFI.md`, `MODEM.md`, `BACKUP.md`, `BUILD.md`,
  `otg/OTG.md` — notes and findings.
- `firmware-lenovo-phoenix/` — local-only firmware package template + provenance
  (APKBUILD + README; blobs live only in the pmaports working tree).
- `images*/`, `backups/`, `firmware-factory/`, `bootloaders/` — local only
  (git-ignored).

## Build & flash (short version)

Requires pmbootstrap with `device-qcom-msm8916`, UI `fbkeyboard`.
Copy `msm8939-lenovo-phoenix.dts`, `panel/panel-novatek-nt35532.c`,
`panel/panel-novatek-nt35532.kconfig` and `power/qcom-pon.c` into
`device/testing/linux-postmarketos-qcom-msm8916`, wire them up in
`APKBUILD` `prepare()` (see git history), enable
`CONFIG_DRM_PANEL_NOVATEK_NT35532`. The Adreno firmware (`a420_pm4/pfp.fw`)
must be in the initramfs, otherwise the early `msm` probe fails its firmware
request: ship `30-gpu-firmware.files` (see db820c precedent) into
`/usr/share/mkinitfs/files` via the `device-qcom-msm8916` package so mkinitfs
picks it up. Build, install, then:

```
./split-image.sh <qcom-msm8916.img> ./images
fastboot flash boot lk2nd.img   # lk2nd 23.1 + dtb-files patch, one time
fastboot flash -S 200M system ./images/boot-part.img
fastboot flash -S 200M userdata ./images/root-part.img
```

Backups of boot/recovery/modem/persist are your own responsibility —
see `BACKUP.md` (serials redacted, use `$SERIAL`).

## External references

Upstream and third-party code this port builds on or borrows from.
Nothing below is vendored except where noted; pmaports edits live as local
modifications until (if ever) they go upstream.

- Kernel base: [msm8916-mainline/linux](https://github.com/msm8916-mainline/linux)
  (`v6.12.1-msm8916`), packaged via pmaports
  `device/testing/linux-postmarketos-qcom-msm8916`.
- Bootloader: [msm8916-mainline/lk2nd](https://github.com/msm8916-mainline/lk2nd);
  our `0002-dts-msm8916-phoenix-dtb-files.patch` only enables the phoenix DTB.
- Panel driver: generated with
  [linux-mdss-dsi-panel-driver-generator](https://github.com/msm8916-mainline/linux-mdss-dsi-panel-driver-generator)
  (Cristian Cozzolino) from the vendor DT; init sequence + 1-byte DCS
  backlight are our additions (`panel/`, `panel-nt35532-skuk-initseq.c.inc`).
- Downstream reference: LineageOS 16.0 kernel for pb1770m
  (local checkout `android_kernel_lenovo_pb1770m-lineage-16.0`, not in repo;
  originally from
  [sheikhshahnawaz41299/android_kernel_lenovo_pb1770m](https://github.com/sheikhshahnawaz41299/android_kernel_lenovo_pb1770m)):
  panel init sequence, sensor placement, WCN rails, OCV table (scud-4v35
  profile), SMB358 node/IRQ, audio + pinctrl facts. Excerpts under
  `downstream-pb1770m/`; decompiled stock DTB in `downstream-phoenix.dts`.
- SMB358 register map: `smb358-charger.c` from the
  [Oculus Go kernel](https://github.com/facebookincubator/oculus-linux-kernel)
  (reference only — our driver is a fresh read-only implementation).
- Device package base: pmaports `device/testing/device-qcom-msm8916`
  (Nikita Travkin). Local additions: NT35532 in initramfs, `msm` restored,
  a420 firmware file list (pattern copied from archived `device-arrow-db820c`
  `30-gpu-firmware.files`), audio udev/script/UCM, `alsa-utils` dep.
- Audio template: in-tree `msm8939-huawei-kiwi.dts` (dai-links, mic routing,
  MBHC thresholds); machine driver `apq8016-sbc-sndcard` used as-is.
- Firmware blobs: `linux-firmware` (a420 GPU, via distro packages) and the
  stock QPST package (modem/WCNSS, local only, never committed).
- WiFi NV fallback: `firmware-qcom-db410c-wcnss-nv` symlink (stock pmaports
  hack, still in the device package).

## License

GPL-2.0-only (kernel-derived files). See `LICENSE`.
