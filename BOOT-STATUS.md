# 首次启动成功 (2026-10-08, surrogate kiwi DTB)

- ssh user@172.16.42.1 通, hostname phoenix, kernel 6.12.1-msm8916
- USB RNDIS 通, eMMC/分区 UUID 挂载正常 (pmOS_boot on mmcblk0p22, pmOS_root on userdata)
- WCNSS remoteproc 能起来, wcn36xx 报 NV 超时 (surrogate 意料之中)
- 传感器/功放按 a7/kiwi 节点探测失败 (意料之中, phoenix 是 bma2x2/ltr559/mmc3524x)
- 屏幕花屏 (panel 不匹配, 意料之中)
- modem watchdog crash/recover 循环 (固件/配置待调)

结论: SoC 主线链路全通, 只差板级 DTS (panel/touch/sensor/modem)。

## phoenix DTB 启动验证 (2026-10-08)
- /proc/device-tree/model = Lenovo PHAB Plus (自研 DTB 启动成功)
- input 全活: pm8941_pwrkey, pm8941_resin(vol-), GPIO Hall(117), GPIO Buttons vol-up(107),
  pm8xxx_vib, Goodix TouchScreen@78ba000(blsp_i2c6,0x5d,event5)
- AVDD28<=l17/VDDIO<=l6 假设正确, 触摸 probe 成功
- 无 fb0 (panel 驱动待做), battery 无读数, load 偏高 (modem crash 循环, 后续关 modem 或修固件)

## 触摸事件确认
- /dev/input/event5 12秒 tapped 文件变大, Goodix 上报正常
- fb0 存在但 blank 无效: 无 panel 驱动, 背光/电源不可控, 系意料之中
- 面板残影: 关机静置消退, panel 驱动为下一优先级

## 显示点亮 (2026-10-09)
- r9 内核 + phoenix DTB + 自研 NT35532 驱动, 屏幕正常点亮无花屏
- 强制亮度 0xff 生效, fb0 + fbcon 接管, 屏幕可见闪烁登录光标
- 仍待办: 背光驱动(亮度不可调), GPU 固件(a420), WiFi NV, modem, sensors

## 教训: mkinitfs 会重置 /boot
- mkinitfs 重写 extlinux.conf (fdt 行丢失) + 从内核包恢复 DTB, 导致回 lk2nd 菜单.
- 之后每次动 /boot (mkinitfs/升级内核) 都要重做: nowcnss DTB + fdt 行.
- 根治: lk2nd dtb-files 解注释 (fdtdir 自发现) + WiFi 修好后 wcnss 回归源码.

## 背光调光成功 (2026-10-09)
- 根因: mipi_dsi_dcs_set_display_brightness 发 2 字节, NT35532 只要 1 字节.
- 改用 mipi_dsi_dcs_write 单字节后 /sys/class/backlight 调光正常.
- 注意: 跑的是 initramfs 里的驱动, 换 ko 必须重打 initramfs (且会重置 /boot 定制).
