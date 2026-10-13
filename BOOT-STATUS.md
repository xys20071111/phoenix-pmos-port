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

## 传感器状态 (2026-10-09)
- bma255 (bma2x2) @0x18: probe 成功, 晃动有读数. mount-matrix 待实测校准 (下游 place=6).
- ltr559 @0x23: probe 成功 (iio:device1), 但 ALS/PS/IR 通道恒为 0.
  可能原因: 增益/使能细节与下游驱动不一致. PARK, 以后对比寄存器.
- mmc3524x @0x30: 无主线驱动 (mmc35240 是另一颗芯片), 跳过.
- avago/mpu6050/mc3xxx: 本机未焊接 (i2c 无对应地址).

## 标准流程打通 (2026-10-09)
- lk2nd 23.1-r1 (dtb-files patch) 刷入 boot, fdtdir 自动发现 phoenix DTB.
- fbkeyboard UI 安装启动成功, 无需任何 TWRP 手工修改.
- 完整链路: pmbootstrap install -> split-image.sh -> fastboot flash -> boot.

## 关机修复验证 (2026-10-13)
- 根因: PMIC PS_HOLD_RST_CTL 留着 WARM_RESET (bootloader), 主线只拉 PS_HOLD.
- 修法: qcom-pon.c 加 POWER_OFF handler (power/qcom-pon.c), 先写 SHUTDOWN(0x04).
- 不插线 poweroff 正常关机 ✓. reboot 不受影响 ✓.
- 已知限制: 插着充电线关机/关机状态插线会自动开机 (lk2nd 无 off-mode-charge,
  与多数 pmOS 移植一致, 暂不修).

## WiFi 点亮 (2026-10-13)
- 决定性实验证据: Lineage 下 5GHz 关联成功 -> RF 双频 -> 非 WCN3620.
- 根因: iris compatible 误写 wcn3620, PMU 给 RF 配 19.2MHz XO,
  芯片要 48MHz (固件自报 vIris_TSMC_4.0 with 48MHz XO) -> RF 死寂.
- 改 qcom,wcn3680 后 NV 通过/crash 消失/wlan0/扫描全通. 详见 WIFI.md.

## GPU 花屏窗口修复 (2026-10-13, 待验证)
- 回退 deferred 方案: 花屏窗口被拉长到 root 挂载后 (~5s+), 噪声太强还留残影.
- 正解 (照抄 db820c): device 包发 `30-gpu-firmware.files`
  (a420_pfp/pm4 共 14KB) 到 `/usr/share/mkinitfs/files`, mkinitfs 收录进 initramfs,
  `msm` 回到 modules-initfs. 显示早起 + GPU 早期一次成功, 两全.
- lk2nd 面板初始化 (用户提议) 不采用: lk2nd 树里根本没有 NT35532 面板驱动
  (只有 nt35590/596/521), 移植整套 init 序列到 LK 工作量大, 且只能"遮"
  (splash 盖住) 不能"治" (显示照样晚起). 先治本, 遮以后再说.

## GPU (Adreno 405) 3D 成功 (2026-10-13)
- 显示 (MDP/fbcon) 本就不依赖 GPU, 一直正常.
- 根因: a420_pm4/pfp 请求发生在 initramfs 阶段 (root 约 8s 才挂载),
  而 adreno 驱动是运行时 request_firmware (无 MODULE_FIRMWARE), mkinitfs 不收录固件
  -> initramfs 里必失败且无重试 (后来补 MODULE_FIRMWARE 声明, mkinitfs 仍不认).
- 另缺 vdd/vddcx regulator (dummy 顶着), bind/unbind 热重载不支持 (-EBUSY).
- 旧正解 (已回退, 见上节"花屏窗口修复"): 曾把 msm 踢出 initramfs 改根文件系统阶段加载,
  GPU 能成但显示晚起、花屏窗口太长. 现改固件进 initramfs, msm 回早期.
- 验证: kmscube -> freedreno FD405 / GLES 3.0 / 60fps vsync.
- 遗留: 花屏窗口变长 (待解), INTF1 空接口下溢噪音 (0x04000000, lk2nd 残留, 无害).

## 电池电量修复 (2026-10-13, 待验证)
- 根因: pm8916-bms-vm 用 OCV 查表算容量, 我们的 battery 节点没有 OCV 表 ->
  ocv2cap 返回 -EINVAL -> UPower 0% -> xfce 电源管理关机 (配着充电自动开机,
  看着像重启; 那串 GPU IOMMU fault 只是伴随症状, handler 会 resume).
- 表取自下游 pb1770m batterydata (scud-4v35 profile, 25C 列);
  xwd profile 低电量段差最多 ~100mV, 若低电量不准再换. charge-full 按下游 fcc
  纠正为 3500mAh. 验证: 刚下充电器应 ~100%, 用一会看电压-百分比曲线合理否.
- 遗留: STATUS 恒 discharging (lbc charger 未接, 同 wt82918 现状), 仅显示问题.
