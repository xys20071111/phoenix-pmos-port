# USB OTG - 解除冻结 (2026-10-18)

前提变化: WLAN (SSH over WiFi) + fbkeyboard UI 已常驻。USB 整个挂掉不再等于失联,
lk2nd fastboot 独立于内核 USB, ramoops 已在 DTB。重试可以开始。

## 结论
10-09 第一次尝试把 usb gadget 一起带崩的不是玄学, 机制已闭环:

- `&usb` (qcom,ci-hdrc) 用 `drivers/usb/chipidea/core.c`
  `devm_regulator_get_optional(dev, "vbus")` 拿 `vbus-supply`。
  DT 写了该属性但 provider 不存在 -> **永久 EPROBE_DEFER**,
  ci_hdrc host+gadget 全部趴下 (电脑认不到设备 = 10-09 症状)。
- provider = 主线 smb347 充电驱动 (CHARGER_SMB347)。它无条件注册
  of_match "usb-vbus" 的 regulator (固定 5V, 使能=写 CMD_A bit4,
  即 SMB358 内置 boost 输出 VBUS)。probe 开局
  `!use_mains && !use_usb -> -EINVAL`: **不加 `summit,enable-usb-charging`
  必然 probe 失败**, 于是 vbus regulator 缺席 -> 上面死锁。
- 冲突实锤: 仓库自研 `power/smb358-status.c` 与主线 smb347 驱动
  都匹配 `compatible = "summit,smb358"`, i2c 设备只有一个能绑。
  当前 status 驱动活着 -> smb347 绑不上 -> regulator 永远不存在 ->
  现在的 DTS 状态加 vbus-supply 必死。**必须二选一, 选主线驱动。**

## 已知 (硬件事实, 全部已验证或双源一致)
- SMB358 @0x6a, blsp_i2c4; irq TLMM 62 (下游 62/LEVEL_LOW, STAT 低有效);
  usbid TLMM 110 (= 现有 usb_id)。充电指示靠 status 寄存器读 -> 总线/地址/IRQ 全对。
- 下游 vbus_otg-supply = smb358_otg_vreg (charger 自身节点的 regulator 名),
  即 OTG 5V 是 SMB358 boost, 由充电驱动使能, 不是独立 regulator。
- 主线 binding: `summit,smb347-charger.yaml`。required 只有 compatible+reg,
  但 anyOf 要求至少一个充电使能属性; `usb-vbus` 子节点是标准 regulator 节点。
- 蓝图: z00t (msm8939-asus-z00t.dts, z00t-otg-pr426.diff 11775-12041 行)。
  z00t = 同 SoC 同芯片同引脚, 直接照抄。

## 方案: 主线 smb347 接管, 弃用 smb358-status
充电指示不丢: smb347 提供 `smb347-usb` psy (ONLINE/STATUS/CONSTANT_*),
pm8916_bms 的 `power-supplies = <&smb358>` 是节点 phandle, 不受影响。

副作用评估 (可控):
- 只会被写: float 电压 (battery 节点 4.35V -> 硬件档 4.34V, 比厂标保守 10mV)、
  CMD_A 充电开关 (有电=开, 无电=关, 与 OTP 行为等价)、指示线重配为低有效。
- 不会写: FCC/预充/截止电流 (battery 节点没给对应属性 -> 保持 OTP)、
  温度限 (同样未给 -> VBAT THM 保持出厂配置)、不设 charge-control (板子无 EN pin)。
- 充电状态灯: 下游 LED 走 GPIO (119/8), 不是 STAT pin; 上机后验证一下。

## 执行计划 v2 (每步一刀, 坏了可直接回滚)
### ST0 pmaports 准备 — 已完成 (2026-10-18, r60)
`linux-postmarketos-qcom-msm8916/APKBUILD`:
- source 里删 `smb358-status.c` / `smb358-status.kconfig` 及其 sha512 行
- prepare() 里删该驱动整段 (cp/Kconfig/Makefile 注入)
config-postmarketos-qcom-msm8916.{aarch64,armv7}: 删 `CONFIG_SMB358_STATUS`
  行, 保持 `CONFIG_CHARGER_SMB347=m` (现在就有)。
pmbootstrap 重打包内核装上后, 确认 `dmesg | grep -i smb` 里只剩
smb347 且充电指示照常。

注: 本机 pmbootstrap 的 chroot 初始化 (sudo) 跑不通, sha512 行是手动
用 sha512sum 精确计算写入的 (dts 新哈希 9873d583…)。build 前照例重跑
`pmbootstrap checksum linux-postmarketos-qcom-msm8916` (幂等, 若我的
哈希有误它会纠正), 再 apkbuild_parse。

### ST1 DTS 手术 (只动 charger, 碰 USB 之前) — 已完成 (2026-10-18)
1. 顶部加 `#include <dt-bindings/power/summit,smb347-charger.h>`。
2. tlmm 里加 pinctrl (照抄 idol3):
   ```
   smb_int_default: smb-int-default-state {
       pins = "gpio62";
       function = "gpio";
       drive-strength = <2>;
       bias-pull-up;
   };
   ```
3. charger 节点改成:
   ```
   smb358: charger@6a {
       compatible = "summit,smb358";
       reg = <0x6a>;
       interrupt-parent = <&tlmm>;
       interrupts = <62 IRQ_TYPE_LEVEL_LOW>;
       pinctrl-0 = <&smb_int_default>;
       pinctrl-names = "default";

       monitored-battery = <&battery>;
       summit,enable-usb-charging;

       usb_vbus: usb-vbus {
           regulator-name = "usb_vbus";
           regulator-min-microvolt = <5000000>;
           regulator-max-microvolt = <5000000>;
           regulator-min-microamp = <750000>;
           regulator-max-microamp = <750000>;
       };
   };
   ```
   (IRQ 从 EDGE_BOTH 改 LEVEL_LOW: 驱动把 STAT 重配为低有效+电平语义,
   且写寄存器时 disable_irq, 边沿会丢。)
   phandle 名 `smb358` 保持 (pm8916_bms 引用它)。

已验证 (2026-10-18): 仓库 dts 同步进 pmaports, 在 6.12.1 副本树上套
camss dtsi 本地补丁后 cpp+dtc 编译通过 (剩余 warning 均为上游
msm8939.dtsi 既有问题, 与本改动无关), 反解 DTB 逐项核查无误:
irq 62 LEVEL_LOW / pinctrl 引用 / enable-usb-charging / usb-vbus
regulator 子节点 (5V/750mA) 全部在位。

#### r60 上机诊断 (2026-10-18): `online` 卡 1 → 补丁 r61

r60 刷机后表现: `smb347-usb` 出现且模块在, 但插拔时 `online` 固定为 1
(`status` 实时读 STAT_C 所以会跟变, 不是看错位置)。取证定位 (regmap
debugfs `/sys/kernel/debug/regmap/1-006a/registers`, IRQ 65 义务计数):

- 硬件完全正常: IRQSTAT_E(0x39) BIT0 (拔线=1/插线=0 跟线走),
  IRQSTAT_F(0x3a) BIT0 POWER_OK 同样跟线, STAT_C(0x3d) 充电状态变化
  正常。CFG_STAT(0x05)=0x3e (DISABLED 位已落地)。
- 中断灌进来: 65: gpio62 Level smb358, 空闲态 +254/10s (~25/s 恒速,
  与 handler usleep 25-35ms 节奏吻合 = 电平被按住持续重触发)。
- 根因 (Oculus Go 驱动位图交叉定案): **SMB358 的 IRQSTAT_E 没有
  BIT1 "input-UV IRQ 锁存位"**。SMB347 布局是 STAT(BIT0)+IRQ(BIT1)
  分离; SMB358 输入欠压事件就一个位 (BIT0), BIT1/BIT5 恒 0 → 主线
  handler 的 `if (irqstat_e & (BIT1|BIT5))` 刷新门在 SMB358 上永远
  不进 → 缓存冻结在 probe 值 (probe 时线在场 → 1)。旧驱动能跟线是
  因为它 get_property 直读 0x3a BIT0 live。
- 25/s 风暴: handler 不读 IRQSTAT_A(0x35)/B(0x36)/F(0x3a), 有锁存
  永远无人应答 → STAT 引脚持续拉低 → WFI 常醒, 纯功耗浪费。
- 潜伏功能坑 (缓存门控 start_stop_charging): boot 不插线 → probe
  缓存 0 → 驱动 CMD_A bit1=0 软关充 → 之后插线无事件翻转缓存 →
  充电不自启动。

**补丁 `power/smb347-smb358-input-detect.patch` (r61)**, 手术四刀:
1. `smb347_irq_set()`: CFG_STATUS_IRQ 使能加 BIT2 (INOK/input-OK
   事件锁存, 仅 SMB358) → 插线产生事件唤醒 handler。
2. `smb347_interrupt()`: SMB358 分支在 E 位读完后直接用已读值推导
   `usb_online = !(0x39 & BIT0)` (对 live 位), 与缓存不等才
   start/stop 充电 + `power_supply_changed`, 返回 IRQ_HANDLED。
3. 该分支顺带 read-ack IRQSTAT_A/B/F (释放引脚, 收敛风暴)。
4. define 块注释写明位图偏差 (未来上游化素材)。

SMB347 真机路径 0 改动 (分支仅命中 `smb->id == SMB358`)。

验收 ST1 (USB 完全没碰, 不应该有任何回归):
- `dmesg | grep -i smb`: 无报错; `lsmod | grep smb347` 在。
- `cat /sys/class/power_supply/smb347-usb/online` 插拔跟随 (0/1)。
- r61 专项: 插着线 sleep 10 前后查 IRQ 计数, 空闲增量应 ~0 (风暴被
  A/B/F read-ack 收敛; 拔线态允许短促脉冲型触发)。
- 状态/充到 100% 正常; USB RNDIS 依旧。
- 充电状态灯看一眼 (驱动会把 STAT 重配为低有效; 下游证据说 LED 走
  GPIO 119/8, 预期无影响)。
- 失败则查: module 是否加载 (`lsmod | grep smb347` / modprobe
  smb347-charger), probe 报什么 (`dmesg | grep smb347`)。

#### r61 上机结果 (2026-10-18 绿) + r62 既决

- 指示全绿: `online` 插拔双向跟变 (拔 0/Discharging, 插 1/Charging),
  `regulator.22 = usb_vbus` provider 在位 (ST2 安全门), RNDIS 无回归。
- 遗留: 插线空闲态 IRQ 仍 +271/10s (~27/s 恒速) — 与 r60 时同速率,
  且 handler 每笔免了 A/B/F (read-ack 无效) → 结论: 引脚空闲就是
  被按住低电平, 不是漏应答。LEVEL_LOW 下 TLMM 对静态低持续重发;
  旧驱动同线 EDGE_BOTH 起了几周 (静态低只算一次沿) → **定案
  r62: charger IRQ 触发改回 `IRQ_TYPE_EDGE_BOTH`** (DTS 一行),
  handler 按事件刷新不变 — 真实事件 (UV 跳变/INOK 锁存) 照样喂。
- r62 验收: 插线空闲 10s 增量 ≈ 0; 指示变化不回退。
  (2026-10-18 绿: "数值没再不停增长"。ST1 全绿收官。)

### ST2 vbus-supply 入场 — r63 上机红 (键盘无 VBUS; RNDIS 同灭) → r64 INOK 翻转
4. `&usb` 加 `vbus-supply = <&usb_vbus>;` (extcon 不改)。DTB 已核:
   usb@78d9000 vbus-supply -> usb_vbus (5V/750mA) phandle 链完整。
   r61 证据保证 provider 存在 (regulator.22), 不会再触发
   devm_regulator_get_optional 永久 defer。
5. r63 上机红 (2026-10-18): 充电正常; host 角色活了 ([382s] "USB 2.0
   started, EHCI" + root hub 1 port) — vbus-supply 没炸 probe。但
   键盘不枚举 (只见 root hub, 无 "new high-speed USB device"),
   RNDIS 一起死 ("无法连接电脑"); 整场 IRQ 只走 2 次 (r62 的
   EDGE 触发正常, r60 那种 25/s 刷屏没有回来)。
6. 判读: CMD_A bit4 (boost 使能) 是写进去了 — 与上一致走的是
   legacy 角色 (srp/hnp/adp 全 disable → ci_otg_is_fsm_mode 为
   false, host.c 直接 regulator_enable; enable 失败会中止
   host_start, 而 EHCI 起来了 = enable 成功)。物理输出没出 →
   剩一把锁 = INOK。主线 regulator enable 注释原话 "VBUS won't
   be powered if INOK is active": 该板 INOK 在无外部输入时恒为
   活跃 → boost 被 INOK 锁死, 键盘没电。RNDIS 掉线暂判连带
   (boost 反灌/角色切换死角, 待分裂排除)。
7. r64 治法 = 主线 binding canonical 一对 (summit,smb347-charger.yaml
   末例同款): usb-vbus 子节点 `summit,needs-inok-toggle;` + charger
   节点 `summit,inok-polarity = <SMB3XX_SYSOK_INOK_ACTIVE_LOW>;`。
   enable 时置起 CFG_SYSOK(0x08) bit0 (INOK 语义翻成活跃高, 物理
   恒低的 INOK 从此读作"非活跃") → boost 出电; disable (退 host)
   先清 CMD_A bit4、再还原 bit0、再 smb347_start_stop_charging
   (disable 路径自己重开充电)。正常充电全程不碰这个位。DTB 已核:
   两属性落地, cpp 宏出自 include/dt-bindings/power/summit,
   smb347-charger.h (dts #include 在位)。方向核对: 若选
   ACTIVE_HIGH, enable 会把 bit0 清零而 boost 仍锁 — 选择正确。
   一次一刀: r64 vs r63 只多这两个属性, 驱动源码没动。

验收 ST2 (r64, 按顺序):
1. RNDIS 优先裸测: 正常开机直接插 PC (先不碰适配器) → 枚举 +
   网络回归? 若红: 采当时 dmesg + /sys/class/udc + extcon state
   再判 (RNDIS 单独生病 = 角色/extcon 线程, 另案)。
2. 适配器 + 键盘: `dmesg` 应见 "new high-speed USB device",
   键盘可打字; 插着期间 regmap 读 0x30 (bit4 应 =1) 和 0x08
   (bit0 应 =1), /sys/kernel/debug/regulator/regulator.22 状态
   enabled。
3. 拔适配器 → host 停; 插回充电线 → 充电自行恢复 (disable 链
   在代码里, 非预估)。
4. 拔线空闲再验 IRQ 静默 (数值不增长)。
分支: (2) 仍无枚举且 0x30 bit4=1 且充电正常 → INOK 学说破产,
   下一位嫌疑 = boost 输出路径硬件/限流寄存器。

### r64 上机数据 (2026-10-18, 状态 A: 适配器+键盘在插)
寄存器三判:
- `30: 10` — CMD_A bit4 = 1, boost 使能命令**已落地**。
- `08: 19` — CFG_SYSOK bit0 = 1 (基线 0x18 → 0x19), INOK 翻转
  **已生效**。
- `39: 01` / `3a: 00` — IRQSTAT_E bit0 (无外部输入) 符合 boost
  体感; F=0。 regulator_summary: usb_vbus 使能态在, 消费者 =
  78d9000.usb-vbus (use=1)。
→ 从 regulator 到 SMB358 寄存器, 命令链端到端**全部是活的**;
  但键盘照旧无枚举事件, dmesg host 之后零输出。剩余嫌疑空间被
  压缩到: boost 硬件输出路径 (5V 出不到线) 或 键盘供电缺失 —
  键盘 LED 观测 = 最便宜的判据 (有灯 = VBUS 已出, 转枚举侧查;
  无灯 = 5V 没上线, 查板级 VBUS 通路/负载开关)。

RNDIS 更新: 用户确立 **"从开机就没有 RNDIS 设备"** (PC-first
全新开机也死) → 主机会话 wedge 论被**排除**, 假死点在 boot 层。
r61 明确验过 RNDIS 正常, 死亡触发落在 r62..r64 的 DT 变更带:
(a) r62 充电器 trigger→EDGE_BOTH; (b) r63 &usb +vbus-supply;
(c) r64 INOK 两属性。代码读全后 (reg_vbus 无 device 侧消费者,
extcon id-only 驱动逻辑未触碰), 三者对 gadget 都没有已知的
直接作用点 → 下一步要 boot 层证据: /sys/class/udc 是否注册、
configfs gadget 是否 bind 成功、extcon state 在 PC 线上时的
USB 态、dmesg gadget/udc/configfs 全故事。

### r64 上机前情 — INOK 翻转未解、两状态收集计划 (同日早)
- 同症仍在: host 起 (EHCI@248s), 键盘无枚举; PC 侧 lsusb 空白
  ("PC 认为设备没连上")。充电正常, 其余功能不受影响。
- 寄存器询证这轮失败于命令笔误 (助手上轮给了字面 `grep 3A/38`,
  本意是寄存器 0x30/0x08; 且大小写陷阱), 无有效读数;
  0x38 (IRQ_D) = 00 顺带确认 IRQ_D 无事件。

代码侧定案 (6.12.1 读全):
1. reg_vbus 消费者只有两处: core.c:695 probe 读取 + host.c
   (start enable:158-168, stop disable:199-201)。**DEVICE 模式
   零引用** (udc.c 全文无 reg_vbus; otg_fsm 467/487 分支在
   fsm 关闭时是死代码)。
2. extcon-usb-gpio 仅 id-gpio 配置下 **VBUS 态 = ID 态**
   (驱动注释原文); 插 PC 线 → ID 浮高 → USB=1 → gadget
   vbus_active。此链 r63/r64 未触碰, r61/r62 已验证可用。
3. "l13: voltage operation not allowed" = `&usb_hs_phy` 的
   `v3p3-supply = <&pm8916_l13>` (上游基线 msm8916-pm8916.dtsi
   自带), 每次 USB PHY 上电都报 — 非本次变化 (r63 日志只是截). 
   r63/r64 对 DEVICE 模式代码路径没有直接引用点 → RNDIS 死因
   若非 host 会话后的角色切换粘滞, 就不在已读代码里; 需要
   PC-first 裸测分裂。

收集计划 (两状态判别式):
- A = 适配器+键盘插着 (host 活): `sudo cat /sys/kernel/debug/
  regmap/1-006a/registers` 整打 (判 0x30 bit4 / 0x08 bit0 /
  0x39 / 0x3a / 0x3d) + `grep -A6 usb_vbus /sys/kernel/debug/
  regulator/regulator_summary` + dmesg tail + 键盘 LED 观测
  (有灯 = VBUS 真出了 → 问题在枚举侧)。
- B = 重启后全新开机, 什么都不插, 先插 PC 线 30s: PC 侧 lsusb?
  死 → 抓 extcon state + /sys/class/udc/ci_hdrc.0/state +
  `ls /sys/class/udc` + dmesg tail (判定 probe 级 gadget 死)。
  活 → RNDIS 没坏, 死法 = "host 会话后角色切换卡死" 案 —
  再复现一次 适配器→拔→插PC, 立刻抓同四条 + dmesg tail。

### RNDIS 死因定案 (2026-10-18 晚, SSH 直连取证)
- 设备侧证据链: /sys/class/udc/ci_hdrc.0 存在; extcon USB=1 正常;
  **/sys/kernel/config/usb_gadget 空** = gadget 从未被组装; rootfs
  无任何 factory 脚本 (mkinitfs hooks 空, udev 无规则, init.d 只有
  unudhcpd.usb0 = 纯 DHCP 侧)。
- 工厂真身 = **initramfs**: pmOS `setup_usb_network_configfs`
  (init_functions.sh) 开机一次性: `if [ -z "$(get_usb_udc)" ] →
  "No UDC found, skipping"` — **无等待无重试**, _setup_usb_network
  标记保证全场只试一次。
- 时序: r63 起 &usb 挂 vbus-supply → ci_hdrc probe 被 regulator
  依赖拖到 charger 之后 (fw_devlink) → initramfs 跑 factory 时
  UDC 尚未出生 → 跳过 → RNDIS 全程死。r61 时代无 vbus-supply,
  ci 早于 factory → RNDIS 活。一条属性, 两个世界。
- **实锤实验 (手动复刻 initramfs 配方)**: mkdir g1 + rndis.usb0 +
  绑 ci_hdrc.0 → **BIND-OK**, usb0 UP, unudhcpd.usb0 重启 OK,
  dmesg 出 "configfs-gadget.g1 gadget.0: HOST MAC ..." — ci 的
  gadget 侧 100% 健康。PC 侧确认待用户插线。
- 证据污染注记: 用户曾 `dmesg -C` 清缓冲, 开机早期 kmsg 不可回溯;
  udc 符号链接 mtime 19:05:57 (=2943s host 停止时刻) 与 "udc 在
  probe 注册" 的假设不符 (疑似 udc 设备随 gadget 角色启停重建,
  或 re-probe) — 下次开机用 stat(u mtime) vs uptime 实测定谳。
- 修复案 (r65, rootfs 刀, 不动 kernel): 开机 factory 带
  wait-for-udc 循环 (≤30s) + 组装 + up + unudhcpd 重启;
  **已落地**: /etc/local.d/gadget.start (755, local 服务在 default
  runlevel), 下次重启生效 — 重启后 stat udc mtime vs uptime 顺带
  定谳 "udc 何时注册" 之悬案。**已打包 (同日)**: 活体脚本逐字节镜像成
  `device/testing/phoenix-usbgadget` 小包 (depends unudhcpd; 附带
  /etc/runlevels/default/local 符号链接使能 local 服务, 不赌
  post-install 时序; source+sha 自带; 经
  `pmbootstrap config extra_packages phoenix-usbgadget` 进每一张
  fresh rootfs), 此后重刷 rootfs 不再丢; 长期正解仍是给
  postmarketos-initramfs 提 wait-loop 补丁, 届时本包可退役。
- usb0 静态 IP 已补 (172.16.41.1/24, initramfs 会做 ifconfig 这步,
  手动组装时漏了); PC 侧 DHCP → 172.16.42.2, ssh user@172.16.41.1。
- **键盘灯不亮** (用户回报): 命令链全绿但硬件不吐电, IRQSTAT_F
  POWER_OK 从未置位。键盘 LED 指示本身不可靠 (很多键盘仅 OS 驱动
  才亮灯) → 改用鼠标/机械硬盘/手柄等"上电即有迹象"设备重测;
  同时问关键天花板问题: **原生 Android/LineageOS 上这台机用过
  OTG 吗?** 若原生就不支持 → 板上可能根本没有 VBUS 输出通路,
  host 线直接封顶, 重心收回到充电改进 + RNDIS 修复 (已成功)。
  summit,enable-otg-charging 已排除嫌疑 (z00t 蓝本也没有; 全树
  无人用; 驱动注释 "not implemented yet"; 它只管 CFG_OTHER 0xc0
  auto-OTG 位, 与手动 CMD_A bit4 无关)。

### OTG host 死根定案 + 破局 (2026-10-18 深夜, 活体寄存器实验)
**根因: 主线 `hw_init` 把 CFG_OTHER (0x09) bits7:6 (OTG 控制模式) 擦成 0。**

- 寄存器语义 (ZTE smb347-charger.c + Lenovo smb358-charger.c 双源):
  `CFG_OTHER_RID_DISABLED_OTG_PIN 0x40` / `CFG_OTHER_RID_ENABLED_OTG_I2C 0x80` /
  `CFG_OTHER_RID_ENABLED_AUTO_OTG 0xc0`。0x80 = OTG 由 CMD_A bit4 (I2C 命令)
  授权, 0x00 = OTG 整体关闭。
- 主线 `hw_init`: `regmap_update_bits(CFG_OTHER, 0xc0, use_usb_otg ? 0xc0 : 0)`
  — 只认两种模式 (不写 property → 0x00)。**开机第一次 probe 就把芯片 OTP 里
  烧好的 0x80 模式擦掉**, 此后所有 CMD_A bit4 都是死命令: 芯片 ACK 该位但从
  不启动 boost, 无任何 fault 位, IRQSTAT_F bit2 OTG_VALID 从不置位。
- 实验矩阵 (i2cset 活体改写 + boost 复臂 30=0x80→0x90, CMD_A ALLOW_WRITE
  bit7 会自清/衰减, **所有配置写必须同一 shell 一口气完成**):
  - exp2 教训: 跨 SSH 轮询往返的写不粘牢 (先写后 dumps 见基线值),
    0xc0/0x40 实为未测出真值。
  - exp3 (单发粘写): `0x09=0x65` (PIN 模式) 死 ✓。
  - exp4-Q1: stock-parity 残值 + `0x09=0xa5` (bits7:6=0b10, I2C 模式) +
    boost 复臂 → **活**: Stadia 手柄枚举 `usb 1-1`, hidraw + force-feedback。
  - exp5 隔离 (S1/S2/S4): 把 0x04/0x06/0x08/0x0a 全恢复主线原值,
    仅 `0x09=0xa5` → 活 (7 次重枚举/步) → **最小修复集 = CFG_OTHER
    bits7:6 = 0x80 这一个值**; S2 加 0x08 bit0=1 (r64 INOK 翻转) → 同样活,
    翻转非必要 (r64 的 INOK 理论对死根是误会, 属性无害但成冗余)。
  - S3 负对照 (0x09=0x25) 出现 9 次重枚举事件, 判为断开重试噪音, 不完整;
    但 r63/r64 两个实机版本全程跑在 0x25 (hw_init 开机即写) 且 OTG 全死,
    负对照事实上已两度成立。
- **正实锤 (S4 终态)**: on-device `lsusb` → `Bus 001 Device 008: ID
  18d1:9400 Google LLC Stadia Controller rev. A` — 手机正以 host 供电+枚举
  外接设备。stock (downstream) 驱动从不写 CFG_OTHER, 靠 OTP 0x80, 与实测吻合。
- 出场固件已备: `/mnt/local_data/pb1-770n/` (boot.img = 原厂内核等全量分区,
  留作日后寄存器对照/回滚素材)。

**r66 刀 (pkgrel=66; r65 = rootfs gadget factory 刀, 内核编号跳过):**
- 新 kernel patch `power/smb347-smb358-otg-i2c-mode.patch` (已进 APKBUILD
  source+sha, `git apply --check` 绿): `smb347_probe()` 里 usb-vbus regulator
  注册成功之后 — `!use_usb_otg` 时 (板子把 OTG VBUS 交给本驱动的 regulator
  管) 臂 `CFG_OTHER = CFG_OTHER_RID_ENABLED_OTG_I2C (0x80)`;
  auto-OTG (summit,enable-otg-charging) 语义不动 (hw_init 已处理 0xc0)。
  新 define `CFG_OTHER_RID_ENABLED_OTG_I2C 0x80`。语义: 谁挂 usb-vbus 子节点,
  谁就要 I2C 命令模式, 一一对应。
- DTS 同刀清理: 移除 `summit,inok-polarity`、`summit,needs-inok-toggle`
  与 summit,smb347-charger.h include (S1/S2 证明翻转非必要, 向 z00t 蓝本
  收敛, DTS 更小)。
- 上机验证清单: 适配器冷插 (非残留态) → 期待 EHCI + 外设枚举纯驱动路径成活;
  usb host 转场时 IRQSTAT_F bit2 OTG_VALID 应置位; 重启确认 RNDIS 自启
  (gadget.start) 与 host 不互斥; 充电回归。

### r66 sideload 上机 + 首启 USB 全灭定案 (2026-10-18 夜: bind 竞速, r66 无罪)
- 更新通道改为 `pmbootstrap sideload linux-postmarketos-qcom-msm8916`
  (SSH 推 apk 进活体系统 + apk add, 免 fastboot 免配对刷写;
  split 镜像对 boot-part/root-part 必须同轮成对同刷, rootfs UUID 每次
  install 都变, 只刷 boot-part = 按不存在的新 UUID 找 rootfs,
  设备无法引导 — 用户教训, 已入 BUILD.md)。
  reboot → PC 端 USB 全灭。
- **排除程序性死因**: 设备 WiFi SSH 活 (boot 20:26:47, 我检查时已
  up 32 min, 正常引导); `uname -v` #67 (20:08 UTC 用户构建 = r66 内容)。
- **排除 r66 内核自身**: smb347 bound @1-006a (先头我查的是 4-006a,
  那是 bus 编号老误会), `regulator.22` provider=1-006a ✓,
  充电 "Charging" ✓,
  **`i2cget 0x09 = 0xa5` = 臂位由驱动 probe 自动完成** (本轮无任何
  手工 i2cset) — CFG_OTHER 修复在首次引导就生效 ✓。
- **真凶 = gadget.start 的单发 bind**: 证据链 (boot = 20:26:47):
  `/sys/class/udc/ci_hdrc.0` mtime = boot+15~20s (UDC 确实在);
  configfs g1 目录在 (gadget.start 通过了 30s 等待) 但 **g1/UDC 空**
  = 装配完成、echo 绑定一次失败被 2>/dev/null 吞掉, 无重试 → usb0
  不存在 → PC 端全灭。**分钟级之后手工 echo 同一绑定秒成 (bind-rc=0)**
  = 单发正好蹭进 "udc 已列、gadget 核未就绪" 的 EBUSY 窗口。r65 根治了
  晚生 UDC, 又埋了新雷: 任何次引导时序挪动 (r66 的 ci 时序调整)
  就炸。
- 现场恢复: 手工补绑 + usb0 up + unudhcpd restart → PC 端 RNDIS 复活
  (dmesg HOST MAC 2246s)。
- **r67 刀 (gadget.start 结构性修复)**: 装配段与 UDC 解耦 (mkdir
  configfs 无需 UDC, r65 版本来就如此), 等待+绑定合一环: 240×0.25s
  (60s) 内反复 echo 并用回读比对确认绑定 (UDC 文件非空则跳过写,
  幂等可重入), 绑定后才配网/重启 unudhcpd。三副本同哈希 (bdcf036c...):
  设备活体 + pmaports 包 (pkgrel 0→1, 自付 sha — checksum 撞 sudo 墙)
  + workspace usb/gadget.start。此刀 rootfs-only, sideload 不适用
  (非 apk, 文件在活体盘), 走 SSH 直改立效。

### ST3 回归
充电到 100%、WiFi、屏、modem/kbd 都过一遍; OTG host 中途反复插拔。
都绿了再考虑给 msm8916-mainline 提 DTS PR (主线化, 删本地绕路)。

## 失败处置
- 死活进不去: 任何一步失联都先重启再 flash 旧 boot-part.img
  (lk2nd fastboot 与内核 USB 无关, 永远有路)。
- USB 死但 WiFi 活: SSH 抓
  `dmesg | grep -E "ci_hdrc|smb347|regulator"`。
  经典死法 = "smb347-usb-vbus" regulator 不存在 -> ci defer。
  看 `/sys/bus/i2c/devices/4-006a/driver` 绑的是谁。
- 开机都进不去: 下次开机走好镜像后 `dmesg | grep -i pstore` /
  `/sys/fs/pstore/console-*` (ramoops @0x9ff00000, console 0x20000)。
- 保持一次一刀: ST1 与 ST2 不要合并提交, ST1 绿了才 ST2。

## 参考
- otg/z00t-otg-pr426.diff: 相关 hunk = msm8939-asus-z00t.dts 区段
  (ll. 11775-12041)。idol3 区段的 `<&charger>` 当 extcon 是 SMB1360
  另一款芯片的接线, 与本机无关, 不要照抄。
- 主线机制: chipidea core.c (vbus=optional regulator, extcon[0]=VBUS,
  extcon[1]=USB-HOST), host.c (host 起来 -> EHCI portpower ->
  regulator_enable; 无 PORT_VBUS_EARLY flag), smb347-charger.c (注册
  usb-vbus regulator, CMD_A bit4 OTG)。

## r67 定局（2026-10-18 晚）：0x65 破案 + 三层修复全绿

### 0x65 之谜（r66 "重启后全灭"真因之一）
- 采样器（0.2 s 粒度， i2cget 直读）实弹捕获：**PC 线拔出瞬间（VBUS 下降沿 ±1 s 内），
  芯片自主把 CFG_OTHER[7:6] 从 0b10（I2C 命令）翻成 0b01（PIN 模式， exp3 证实为死模式）**。
  驱动 regmap 缓存仍显示 0xa5（缓存遮蔽物理漂移， debugfs cached=0xa5 vs i2cget=0x65 三连）。
  空闲窗 600 样本零漂移 → 翻转是事件驱动， 与 VBUS/角色事件相关。
- IRQSTAT 解码（下游 Motorola/LGE smb358 驱动交叉验证）：
  IRQSTAT_D=0x40 = IRQ_D_APSD_COMPLETE BIT(6) = 输入源检测完成标志（与 OTG 无关， 虚惊）；
  IRQSTAT_F=0x01 = bit0 POWER_OK； bit6/7(OTG_OC)=0， bit4/5(OTG_UV)=0 → 无过流无欠压。
- l13 噪声破案： debugfs 显示 ci_hdrc.0.ulpi-v3p3 的供电就是 pm8916_l13（v1p8=l7），
  "l13: voltage operation not allowed" = 角色切换时 PHY 固定电压供电被拒的噪声， 无害。
- -71 (EPROTO)/"Cannot enable. Maybe the USB cable is bad?" 只出现在刷机后首次开机
  （枚举成功 ~1-4 s 后掉线+描述符失败重试）， 之后未再复现； 头号嫌疑 = downstream 固件在
  host 模式给 ULPI PHY 打 4 组调优（qcom,hsusb-otg-phy-init-seq = <0x74 0x80 0x6d 0x81
  0x3f 0x82 0x33 0x83>）， 主线 ci_hdrc 不打。未证实， 复发再查。

### r67 内核刀最终形态（power/smb347-smb358-otg-i2c-mode.patch, pkgrel=67, 构建号 #68）
单 patch 三处合一：
1. probe 后臂位 CFG_OTHER[7:6]=0b10（r66 原有）；
2. **regulator enable 时重臂**（host 角色每次启动时 rmw； CFG_OTHER 标 volatile 后
   每次直读芯片真实值——覆盖 VBUS 事件导致的芯片自主翻转； 实测 host 会话中 0x09=0xa5 恒成立）；
3. **CFG_OTHER 标 volatile**（regmap 缓存不再遮蔽物理漂移）。
auto-OTG（summit,enable-otg-charging → 0xc0）路径不受影响。

### gadget.start v3→v4（phoenix-usbgadget pkgrel=2→3）
- **v3 破案： configfs 属性文件 stat.size 恒为 4096 与内容无关 → `[ -s ]` 守卫是死代码**
  （v1/v2 全中招： 脚本 240 次重试从未真正执行过 bind echo！）。read-back（读回比对）是唯一神谕。
- v3 加 /tmp/gadget.log 开机取证； 实测 boot-time： creating-gadget → bind-try i=0 rc=0 →
  bound-ok → net-up， 开机自绑成功。
- v4： usb0 地址 172.16.41.1/24 → **172.16.42.1/24**（pmOS 规范网段）。B 修复根因：
  unudhcpd-openrc 包默认 -s 172.16.41.1 -c 172.16.42.2（服务器/客户端跨子网！），
  host→device 全部 "No route to host"。修复 = /etc/conf.d/unudhcpd.usb0 显式
  SERVER=172.16.42.1 CLIENT=172.16.42.2（包内新增 unudhcpd.conf 文件） + usb0 统一 42.1/24。

### 终局回归（全部实测通过）
- OTG： 手柄+U盘 即插即用； 3 次插拔循环枚举零错（-71 未再复现）； 采样 404/404 全 0xa5；
  Stadia = "Stadia Controller rev. A" 完整枚举 + stadia 驱动接管（input/hidraw）。
- RNDIS： 开机自绑 i=0 rc=0； 设备→PC ping 3/3 0% loss（~0.6 ms）， PC=172.16.42.2 同网段。
- 充电： 依旧正常（probe 臂位路径不受影响）。

### 前向兼容： phosh + systemd（usb-moded）时代
- 内核层（r67 补丁 + DTS）= 永久资产， 与 init/DE 完全无关。pmOS 2025-12 USB 栈改造
  （usb-moded）官方公告原文："In host mode, nothing is changed"——OTG 主机模式零影响；
  内核包与 UI/init 选择无关， 换 phosh 镜像时 pmaports fork 构建自动带上 r67。
- 用户态 = 计划内退役： gadget.start（openrc local.d）让位 usb-moded
  （developer-mode service/initd 双形态， 接手 configfs gadget 管理， MTP 走
  FunctionFS/umtprd）； unudhcpd 留任——NM manual 模式 = 手机 172.16.42.1 /
  客户端 172.16.42.2 + unudhcpd 发 DHCP， 与本 port 统一的网段一字不差，
  ssh 172.16.42.1 工作流无缝延续。
- 迁移注意： ① 卸载/不装 phoenix-usbgadget（防两个 configfs g1 管理者打架）；
  ② usb-moded 默认倾向 NCM（RNDIS 仍支持）， PC 端网卡名会变 enx…；
  ③ initramfs UDC 迟到竞态仍在（mkinitfs 是同一个， ci_hdrc 晚于 smb358
  vbus-supply 的时序是 SoC 级）， usb-moded 为守护进程、天然容忍迟到 UDC；
  ④ usb-moded 较新（2026-02 仍有 networking issue）， 迁移前扫 pmaports issue 区。
  本篇三案（0x65 翻转 / configfs 4096 陷阱 / UDC 竞态）即为给未来迁移者的交接文档。
