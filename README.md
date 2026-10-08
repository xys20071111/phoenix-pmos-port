# Lenovo PHAB Plus PB1-770N (phoenix) postmarketOS 移植

## 设备
- Lenovo PHAB Plus PB1-770N, codename `phoenix` (注意：不是小米 phoenix)
- SoC: Snapdragon 615 MSM8939 (上报 msm8916, 8x Cortex-A53)
- arch: aarch64, 2GB RAM, 1080x1920, eMMC
- 现系统: Lineage 17.1 unofficial 2021, kernel 3.10.108
- 分区: boot/mmcblk0p19, recovery/p20, system/p22, userdata/p32, 无 Treble/A-B

## 结论：可移植，走 mainline + lk2nd
- `lk2nd-msm8916` 官方支持 `PB1-770M/N/P`
- `msm8916-mainline/linux` 6.12 覆盖 MSM8939, pmaports 有通用包 `device-qcom-msm8916`
- 主线无 `msm8939-lenovo-phoenix.dts`，需新建。这是主要工作量。
- 易混淆：PR #289 是 PHAB PB1-750M (phoebe/MSM8916)，不是本机，不能直接用。

## 策略
1. 先用 `device-qcom-msm8916 + console` 验证 pmbootstrap 流程 (display 可能不亮，但 USB networking/serial 可调)
2. 新建 `msm8939-lenovo-phoenix.dts`，参考 `msm8939-huawei-kiwi.dts` / `msm8939-samsung-a7.dts`
3. 面板/触摸需从 Lineage device tree 反推，目前 adb 非 root 拿不到 sysfs，需 TWRP 下备份+提取

## 当前进度
- [x] adb 确认设备 + SoC/分区
- [x] pmbootstrap 3.11.1 手动配置完成 (console, PB1-770N)
- [x] pmaports main (edge) 已 clone, 确认通用包存在
- [x] 参考 DTS 已拷到 reference-dts/
- [ ] TWRP 备份 boot/recovery/modemst/persist
- [ ] 提取 panel/touch 型号
- [ ] 写初始 DTS + lk2nd 测试
- [ ] pmbootstrap install console + USB 调试

## 快速命令
```
pmbootstrap status
pmbootstrap -y --as-root zap -p  # 需 sudo 密码，build 前执行
pmbootstrap install --fde --no-firewall --add ssh-server-dropbear
pmbootstrap flasher flash_lk2nd
```
