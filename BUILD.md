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

Bump `pkgrel` on every change, otherwise `install` reuses the cached
package silently.

## 1. Kernel: `device/testing/linux-postmarketos-qcom-msm8916`

| repo file | pmaports change |
|---|---|
| `msm8939-lenovo-phoenix.dts` | `source=` entry + sha; `prepare()` copies it to `arch/arm64/boot/dts/qcom/` and registers `msm8939-lenovo-phoenix.dtb` in the qcom `Makefile` |
| `panel/panel-novatek-nt35532.c` | `source=` + sha; `prepare()` copies to `drivers/gpu/drm/panel/`, appends `Makefile` obj line |
| `panel/panel-novatek-nt35532.kconfig` | appended to `drivers/gpu/drm/panel/Kconfig` in `prepare()`; enable `CONFIG_DRM_PANEL_NOVATEK_NT35532=m` in `config-*.aarch64` |
| `power/qcom-pon.c` | `source=` + sha; `prepare()` overwrites `drivers/power/reset/qcom-pon.c` (poweroff fix) |
| `power/smb358-status.c` + `.kconfig` | same pattern as panel (read-only SMB358 monitor); `CONFIG_SMB358_STATUS=m` |
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

## 4. Bootloader: `main/lk2nd`

`0002-dts-msm8916-phoenix-dtb-files.patch` (repo root) added to `source=`
+ sha; makes `fdtdir` auto-discover the phoenix DTB (no extlinux fdt edits).

## 5. What upstreaming drops

In order: DTS + panel (to msm8916-mainline/linux), venus msm8939 series
(already upstream — then drop res/v1/gcc patches + DT compatible back),
smb358 (write a real driver), qcom-pon poweroff, audio UCM upstream,
lk2nd patch. Until then, everything above stays.
