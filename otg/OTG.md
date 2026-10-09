# USB OTG - PARKED (2026-10-09)

## 结论
OTG 第一次上机就把 USB gadget 一起带崩了 (电脑完全认不到设备), 已回滚.
蓝图 (z00t PR#426, SMB358 + usb-vbus) 本身是对的, 但在 phoenix 上有坑.

## 已知
- 下游: smb358-charger@6a 在 blsp_i2c4 (0x78b8000), irq TLMM 62,
  vbus_otg = smb358_otg_vreg, usbid TLMM 110 (与现有 usb_id 一致).
- 主线 CHARGER_SMB347 认 summit,smb358,  probe 要求 monitored-battery.
- 回滚后 USB 恢复 (nowcnss DTB, 无 charger/vbus 节点).

## 失败现象
- `&usb` 加 `vbus-supply = <&usb_vbus>` + charger 节点后, RNDIS 不枚举.
- 最可能: smb347 probe 失败 (缺 battery 属性? IRQ? i2c 总线不对?) ->
  vbus regulator 缺席 -> usb 探测定延/defer 死锁, gadget 起不来.
- 缺少串口/dmesg 实证 (坏的时候连 adb 都没有, 下次记得先抓 pstore 再回滚).

## 重试清单 (以后)
1. DTB 里加 ramoops, 先保证永远有日志再动 USB.
2. 先最小化: 只加 charger 节点 (不加 vbus-supply), 看 smb347 probe 报什么
   (`dmesg | grep -i smb`), i2c 地址/总线对不对 (`i2cdetect`).
3. 再加 vbus-supply + 键盘实测.
4. 参考: otg/z00t-otg-pr426.diff (本目录).
