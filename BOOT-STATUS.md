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

## 电池 snap-down 补丁 (2026-10-14, 待验证)
- 现象: 拔充电器后 4043mV 仍显示 97% (按表应 ~74%).
- 根因: 驱动只有 snap-up 没有 snap-down; 拔线瞬间 4.35V浮充->OCV 跌 ~0.3V,
  超过 25mV guard 被丢弃, fake_ocv 全程 stranded 高位, 越用越虚 (会死在显示 25%).
- 修法: power/pm8916-bms-vm-fake-ocv-snap.patch, 放电且 gap>100mV 时 snap 到 vbat.
  负载瞬时下垂误触发也无妨 (snap-up 会自己纠回来). 已 dry-run 验证可打上.

## 音频初版 (2026-10-14, 待验证)
- 本机之前零音频节点. 下游: 内部 codec (msm8x16, micbias1-ext-cap),
  无外部功放 (pb1770m 删了 cdc-ext-spk-pa). 模板: kiwi.
- 开 lpass + lpass_codec + pm8916_codec (supplies 全在 msm8939-pm8916.dtsi),
  sound 用 apq8016-sbc-sndcard + Primary MI2S, 无需重编驱动 (全是 =m).
- 运行时还要配混音器 (multimedia->PRI_MI2S_RX 通路, 解 mute), speaker-test 验.

## 音频 GPIO 冲突 (2026-10-14)
- 声卡认了 (card0 phoenix, Primary MI2S), codec 正常, Headset Jack 输入建好.
- 但 sound 的 pri_mi2s pinctrl 抢了 gpio115, ltr559 (IRQ 在 115) probe 失败.
- 根因: 内部 codec 走 PDM (63-68), 板上无外部 MI2S 设备, 113-116 本就该是 GPIO
  (下游 ltr559 IRQ 115 作证). 修法: sound pinctrl 只留 cdc_pdm.
- MBHC 按键检测缺 vthreshold (仅报错,  parked, 耳机线控以后再说).

## 音频输出打通 (2026-10-14)
- 声卡一次点亮 (card0 phoenix, Primary MI2S, codec 全认, MBHC 按键缺表 parked).
- 三个坑: (1) 老 LPASS 无路由 mixer, DMA 硬连, 无需设; (2) HPHL/HPHR 是枚举
  ZERO/Switch, 不是布尔 on; (3) 喇叭挂 RX3, 立体声下 RX3 无数据 -> RX3 MIX1 INP1
  拨到 RX1 当 mono 用. 另: sound 曾抢 gpio115, pinctrl 去掉 pri_mi2s (内部走 PDM).
- 遗留: PipeWire 显示 Dummy (缺 UCM), 麦克风 (无 capture link), 耳机插拔自动切换.

## 音频增益分级 (2026-10-14)
- RX 数字音量 0-124 = -84dB~+40dB, 0dB 在 84 (68%). 之前 80% (=99=+15dB)
  是数字 boost, 巨响; alsactl 存的 60% (=-10dB) 配桌面 -14dB 共 -24dB, 太轻.
- 定级: 硬件锁 0dB (68%), 音量全交给桌面滑杆 (软件). UCM verb 同步改 68%.

## 音频初始设置进镜像 (2026-10-14)
- 每次刷机手设混音不可接受. 方案: device 包发 udev 规则
  (90-msm8916-audio.rules) + 脚本 (usr/libexec/msm8916-audio-init.sh),
  声卡出现即设 (41 秒晚 probe 也覆盖, init 无关, 刷机不丢).
  要求 alsa-utils (已加进 device depends). 文件本体在 repo audio/ 目录.

## UCM 进包 (2026-10-14)
- UCM 手拷 /usr/share, 重刷 rootfs 必丢 -> Dummy 重现. 修法: 同 device 包发货
  (usr/share/alsa/ucm2/phoenix/), pkgrel=5, 和音频脚本同包.
- device-qcom-msm8916 本地增量至今: panel-nt35532 入 initfs, msm 回归,
  a420 固件进 initramfs, 音频 udev+脚本+UCM, alsa-utils 依赖.

## 充电指示: SMB358 只读驱动 (2026-10-14, 已解决)
- PMIC lbc 判死刑: 0x1310 恒 0x04 不跟线走, USB 块没电/没接.
- SMB358 在 blsp_i2c4 (下游 i2c_4=gpio14/15, 之前总线都没开所以扫不到).
- 新驱动 power/smb358-status.c: 只读 STATUS_C/IRQ_F 报 ONLINE/STATUS,
  绝不写配置 (充电靠 bootloader/OTP 本来就能到 100%). IRQ 用 gpio62.
- 验证: 插拔线 online/STATUS 跟着变, 桌面电池图标正常. lbc 已 disable.
- 寄存器映射参考 Oculus Go 内核 smb358-charger.c (只借地址/位定义,
  驱动是重写的只读实现, 见 README 外部引用).

## 耳机声道问题 (2026-10-14, 结论: 双声道混音, parked)
- 现象: single-channel drive (-s1/-s2) 双耳同响; 静音/swap/mux 全套行为自洽地
  指向 PDM 输出即混音 (两路 PDM 输出内容相同, mute 按路独立生效).
- 排除链: INP2/3 全 ZERO, RX1/RX2 mute 独立有效, swap 无效, 换耳机无效,
  关喇叭无效, LPASS I2SCTL=0x4400 (STEREO/SD0/16bit 正确), PipeWire 全停无效,
  冷启 (拔线 30s) 无效.
- Bisect: 去 tertiary (A) 无效, 退 r26 等效 (B) 无效, r25 原版 (C) 无效.
  fork 与 torvalds 6.12 的 audio 驱动 diff 只有 MBHC-gpio, 数据通路无差别.
- 教训: regmap 禁止暴力全读 (挂总线触发看门狗重启); 单点读 lpass 无害.
- 现状: 输出按 dual-mono 用 (喇叭本来就是 mono). 真立体声待驱动深挖.

## 重力旋转修复 (2026-10-14, 已解决)
- 横屏上下颠倒: 缺 mount-matrix, Y/Z 全反 (X 是对的).
- 实测三姿势 (raw): 平放 (0,0,-1)g, 竖直 (0,-1,0)g, 顺时针横屏 (-1,0,0)g
  => screen = diag(1,-1,-1) x sensor (det=+1, 绕 X 转 180 度, 合法旋转).
- bmc150 驱动吃标准 mount-matrix. 另: raw 读数要先停 iio-sensor-proxy
  (占着 buffer 会 EBUSY).

## 旋转矩阵修正 (2026-10-14)
- 第一版 diag(1,-1,-1) 全反 (portrait 反是新毛病, 横屏依旧反).
- 教训: portrait 正误只看 Y, 横屏只看 X; X 才是要反的那个, Y/Z 碰不得.
- 用 hwdb (ACCEL_MOUNT_MATRIX) 热切换试出 diag(-1,1,-1) (绕 Y 180 度),
  四个方向全对, 已落进 DTS. hwdb 只作测试 (重刷即丢, 以 DT 为准).

## 视频硬解 (Venus, 2026-10-14, 起步)
- 现状: DTS 无 venus 节点 (dtsi 只有预留内存), 驱动 CONFIG_VIDEO_QCOM_VENUS=m 现成.
- 驱动要 qcom/venus-1.8/venus.mbn (PIL 签名, 必须原厂).
- stock NON-HLOS 里 58 个文件, 有 MBA/MODEM/WCNSS, 无 venus.* (MODEM_PR 读不出).
  本机无独立 firmware 分区. venus.mbn 下落不明: 查手机 modem 分区 / linux-firmware.
- 下游确认 venus 基址 0x1d00000 (同 8916, 可直接抄 msm8916 模板节点).

## Venus 视频硬解 (2026-10-14, 结论: firmware 起, 会话楔死, parked)
- 节点/固件/驱动就绪: video0(D)/video1(E), venus.mbn 全套原厂, IRQ 55 次, IOMMU 已挂.
- 固件活着: 报版本号 VIDEO.VE.1.8-00138, HFI 能收 buffer 注册 (BufferChk 日志).
- 死法: 会话一起就楔. REQBUFS 超时 / G_FMT 空转 (无 resolution 事件) /
  `H264d output_manager pool is NULL` / 随后一切 HFI 超时, IRQ 数冻结 55 不涨.
- 排除: IOVA 冲突论 (dma-iommu 自顶向下约 3.5GB, 固件区 0-5MB 碰不上;
  且 buffer 注册本身是通的); 时钟 (clk_ignore_unused 无影响已 revert);
  权限; 尺寸 (1920x1080 同样死).
- 嫌疑 (未证实): TZ 页表看不见高位 buffer; 或 video core0/1 的 GDSC/时钟
  主线管不到 (下游有 5 时钟+3 GDSC, 主线只有 3+1); 或固件在 LoadResources 空池
  直接崩. db410c 同款驱动/DT 未见有人验证过解码.
- 现状: ffplay 软解 33% 照播 (8 核 A53 够 1080p25), 先走软解; Totem 卡另查渲染.

## Venus CP 范围修复 (2026-10-15, 待验证)
- 别人指的路, 代码验过是实的: msm8916_res 无 cp_* -> TZ 保持默认,
  固件 IsBufferProtected 全拒 -> output pool NULL -> PC 超时.
- firmware.c:245 用 TZ 且 cp_size 非零才调 mem_protect_video_var.
- 值取自本机下游 DTB (venus_ns 池首 0x5dc00000, sec_non_pixel 池
  0x1000000/0x24800000), 不是抄 sdm660 的.
- 修法: power/venus-msm8939-res.patch 加 msm8939_res + compatible,
  DTS 切 qcom,msm8939-venus. (教训: patch 手搓行号必错, 改完 diff -u 生成.)

## Venus IOVA 预留 (2026-10-15, 待验证)
- 方向修正: cp_* 设了反而把 0x0~0x5dc00000 锁成 DRM 区 (非安全缓冲全拒),
  已 revert msm8939_res, 回 msm8916-venus. 只保固件镜像区 0x0~0x500000.
- 做法: DT 预留 IOVA (reserved-memory 无 reg 节点 + iommu-addresses,
  memory-region 挂上) + power/qcom-iommu-resv-regions.patch
  (给 qcom_iommu 补 get_resv_regions, 调通用 of_iommu_get_resv_regions).
- 依据: dma-iommu base_pfn=1 (从 0x1000 往上分, 必撞固件); qcom_iommu 之前
  根本没实现 get_resv_regions, DT 写了也白写.

## Venus 双核供电 (2026-10-15, 待验证)
- 根因实锤: 解码时 venus_core0/core1 电源域 off、core 时钟关 (genpd/clk  sampling 确认),
  LoadResources 上不了 Core0 -> NULL pool -> -110 一条链全对上.
- 修法 (对齐主线 v10 形状): gcc 给两 core GDSC 加 HW_CTRL (固件自管上下电),
  res 加 vcodec0_clks + vcodec_pmdomains (6.12 驱动本就支持, 876 行无条件 attach),
  DTS 补 5 时钟 + 3 电源域 + 名字. IOVA 预留保留 (独立问题, 缓冲仍须躲固件区).
- 验过为实的: GDSC/时钟/res 字段在 6.12 全存在; pas_mem_setup 在 6.12 不存在
  (上一轮分析里虚构的, 作废); CB 隔离说无法证伪但不再关键.

## HW_CTRL 回滚 (2026-10-15)
- 加 HW_CTRL 后 probe 直接挂: 主 vcodec 时钟 status stuck off -> -EBUSY,
  连 /dev/video 都没了 (比原来更糟). 机理未深究 (疑 HW trigger 与主时钟
  时序互锁), 先回退 pristine gcc, 保留 DT 三域 + res pmdomains (SW 控制).
- 若 SW 上电后解码仍楔, 再考虑固件自管时钟的可能性.

## Venus vcodec 时钟 (2026-10-15, 待验证)
- 上游 RFC (Erikas/André, lwn 1068059) 实锤方向: 8939 双核要上电, 且 v3/v4 的
  vdec/venc 功耗方法要搬到 v1 (1xx 原生没有 session 功耗钩子!).
- 但它是 RFC (朋友说的 v10 已合入不实), 全套搬运太大. 本地最小等价:
  power/venus-v1-vcodec-clks.patch, 在 core_power_v1 里顺手开关 vcodec0_clks
  (res 没列则 no-op, 其他 1xx 设备行为不变). HW_CTRL 保持 revert (它单上挂 probe).

## Venus 回归二分 (2026-10-15)
- r38 起 probe 挂 (-EBUSY, 主 vcodec 时钟 stuck off). 候选: 三域 attach 改了
  上电时序. 测试: 退回单域 + msm8916-venus (resv/iommu 保留),
  v1-vcodec patch 在 8916 res 下是 no-op, 不用动.
- 若 probe 恢复: 凶手在电源簇 (域 attach/extra 时钟), iommu 无辜.
- 若依然挂: iommu-resv 或 IOVA 节点背锅, 再砍.
