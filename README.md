# Lenovo PHAB Plus (PB1-770N, phoenix) postmarketOS port

Mainline Linux port of postmarketOS for the Lenovo PHAB Plus
(Snapdragon 615 / MSM8939), booting via
[lk2nd](https://github.com/msm8916-mainline/lk2nd).

## Status (console UI boots, 2026-10)

Working: display (Novatek NT35532 mainline driver, ported init sequence),
backlight/brightness, touchscreen (Goodix GT9xx), volume keys, hall sensor,
vibrator, eMMC, SD slot, USB networking + SSH, battery voltage, accelerometer.

Parked: WiFi (WCNSS zero-RX, see `WIFI.md`), USB OTG (see `otg/OTG.md`),
light sensor (probes, reads zero), GPU 3D accel, modem calls, GPS, camera,
magnetometer (no mainline driver).

## Contents

- `msm8939-lenovo-phoenix.dts` — mainline device tree.
- `panel/panel-novatek-nt35532.c` — NT35532 DRM driver
  (upstream base + phoenix init sequence + DCS backlight).
- `panel/panel-novatek-nt35532.kconfig` — Kconfig snippet.
- `panel-nt35532-skuk-initseq.c.inc` — init sequence extracted from downstream.
- `downstream-phoenix.dts` — decompiled downstream DTB (reference).
- `downstream-pb1770m/` — downstream DTS excerpts (sensors, panel).
- `split-image.sh` — split a pmbootstrap combined image into
  boot/root partition images (validated byte-identical roundtrip).
- `BOOT-STATUS.md`, `WIFI.md`, `BACKUP.md` — notes and findings.
- `images/`, `backups/`, `firmware-factory/` — local only (git-ignored).

## Build & flash (short version)

Requires pmbootstrap with `device-qcom-msm8916`, UI `console`.
Copy `msm8939-lenovo-phoenix.dts`, `panel/panel-novatek-nt35532.c`
and `panel/panel-novatek-nt35532.kconfig` into
`device/testing/linux-postmarketos-qcom-msm8916`, wire them up in
`APKBUILD` `prepare()` (see git history), enable
`CONFIG_DRM_PANEL_NOVATEK_NT35532`, build, install, then:

```
./split-image.sh <qcom-msm8916.img> ./images
fastboot flash -S 200M system ./images/boot-part.img
fastboot flash -S 200M userdata ./images/root-part.img
```

lk2nd does not (yet) know this board's DTB name, so point extlinux at
the DTB explicitly instead of `fdtdir` (e.g. from TWRP):

```
mount -t ext2 /dev/block/mmcblk0p22 /tmp/pmsys
# replace the "fdtdir /" line with: fdt /msm8939-lenovo-phoenix.dtb
```

Backups of boot/recovery/modem/persist are your own responsibility —
see `BACKUP.md` (serials redacted, use `$SERIAL`).

## License

GPL-2.0-only (kernel-derived files). See `LICENSE`.
