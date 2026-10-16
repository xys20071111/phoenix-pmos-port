# firmware-lenovo-phoenix

Local-only pmaports package (lives in
`device/testing/firmware-lenovo-phoenix` of the pmbootstrap pmaports
checkout, NOT upstreamable): stock venus video + modem (MPSS) firmware
for the port.

## Blobs (proprietary, never commit)

### venus (from the stock system image)

`/system/vendor/firmware/venus.{mdt,mbn,b00,b01,b02,b03,b04}`
(VIDEO.VE.1.8-00138, 2015). Place the 7 files next to `APKBUILD`.

### modem / mba (from the modem partition FAT image, NON-HLOS)

`backups/modem.bin` is the dd of mmcblk0p1 (modem/NON-HLOS).
Extract with the repo's `fatread.py`:

```
python3 fatread.py backups/modem.bin ls IMAGE
python3 fatread.py backups/modem.bin extract IMAGE/MODEM.MDT
python3 fatread.py backups/modem.bin extract IMAGE/MBA.MBN
python3 fatread.py backups/modem.bin extract IMAGE/MODEM.B05
...
# FAT names are uppercase; rename to lowercase afterwards
```

Needed set (matches the FAT listing exactly, PHDR-indexed:
b00-b06, b08, b09, b12-b18, b21, b22, b23, b25, b26, mdt, mba):
`modem.mdt`, `modem.b00..b26` (the 21 files present), `mba.mbn`.
Modem ELF span is 85 MiB -> `&mpss_mem` sized 0x5600000 (86 MiB,
same as stock `modem_adsp_region`).

Then:

```
pmbootstrap build firmware-lenovo-phoenix
```

`device-qcom-msm8916` depends on `firmware-lenovo-phoenix-venus`
(local addition), so every install gets
`/usr/lib/firmware/qcom/venus-1.8/` populated. No more manual `cp`.

`firmware-lenovo-phoenix-modem` installs `mba.mbn`, `modem.mdt` and
`modem.bNN` straight into `/lib/firmware/` — exactly the paths
`qcom_q6v5_mss` requests. Install it together with
`soc-qcom-msm8916-rproc` when enabling the modem (BOOT-STATUS.md).
