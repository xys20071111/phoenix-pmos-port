# firmware-lenovo-phoenix

Local-only pmaports package (lives in
`device/testing/firmware-lenovo-phoenix` of the pmbootstrap pmaports
checkout, NOT upstreamable): stock venus video firmware for the port.

## Blobs (proprietary, never commit)

From the stock system image: `/system/vendor/firmware/venus.{mdt,mbn,
b00,b01,b02,b03,b04}` (VIDEO.VE.1.8-00138, 2015). Place the 7 files next
to `APKBUILD`, then:

```
pmbootstrap build firmware-lenovo-phoenix
```

`device-qcom-msm8916` depends on `firmware-lenovo-phoenix-venus`
(local addition), so every install gets
`/usr/lib/firmware/qcom/venus-1.8/` populated. No more manual `cp`.
