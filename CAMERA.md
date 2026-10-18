# Camera (OV13850 后摄 + OV5693 前摄) 移植方案

调查日期: 2026-10-16. 本机 stock 系统镜像 (`/run/media/xys/_/`,
Lineage 16 时代 vendor) 已挂载验证,传感器型号为**实测结论**,
非推测.

## 1. 硬件事实

| 项 | 后摄 (cam0) | 前摄 (cam1) |
|---|---|---|
| sensor (stock lib 实测) | OV13850 (`libmmcamera_ov13850.so`) | OV5693 (`libmmcamera_ov5693.so`) |
| 模组 (下游 DTS eeprom 名) | `sunny_q13v04b` | `sunny_q5v41b` |
| 分辨率 | 13MP 4224x3136 (stock 只用 2112x1568 分档) | 5MP 2592x1944 |
| 从地址 (7-bit, 待 i2cdetect 复核) | `0x10` (rk 公版 DTS 同值) | `0x36` (OV5693 SCCB 惯例) |
| MCLK | gpio26 (`GCC_CAMSS_MCLK0_CLK`, stock 24MHz) | gpio27 (`GCC_CAMSS_MCLK1_CLK`, stock 24MHz) |
| reset / standby | gpio35 / gpio34 | gpio28 / gpio33 |
| 电源 | vdig=l2 1.2V, vio=l6 1.8V, vana=l17 2.85V, vaf=l10 2.8V | vio=l6, vana=l17, vdig=l2 (无 vaf) |
| CSI 通路 | csiphy0 -> csid0 -> ispif -> VFE0 | csiphy1 -> csid1 -> ispif -> VFE0 |
| mount-angle (下游) | 90 | 270 |
| AF 马达 | DW9761 (stock `libactuator_dw9761_camera.so`, CCI 0x0c, AF-pwdm gpio120) | 无 |
| 闪光灯 | LM3646, 在 **blsp_i2c2** (下游 i2c_0=0x78b6000=QUP2) 地址 `0x67`, EN=gpio36 NOW=gpio69 | 无 |

AF / 闪光灯 / EEPROM 全部 defer: 不装驱动 VCM 停在机械零位
(一般为无限远端), 不影响首帧点亮. EEPROM (`qcom,eeprom`)
主线无驱动, 校准数据对首帧无用, 跳过.

## 2. stock 输出配置 (从 sensor lib `.data` 解码, 小端)

后摄唯一分档: **2112x1568**, line=2400, frame=3328,
vt_pixel_clk=240MHz, op_pixel_clk=256MHz
-> 240M/(2400*3328) = 30.0fps. 另有一组全幅参数
(vt=448M/op=480M, line=4512/frame=3328, crop 4208x3120),
stock 未启用, 首版只做 2112x1568.

前摄: **2592x1944**, line=2672, frame=1984.
上游 ov5693 驱动自带该分档 (LINK 419.2MHz, PIXEL 167.68MHz,
XVCLK 19.2MHz), 与 stock 行场参数同量级, 运行时复核.

lane 数: 后摄 2112x1568 RAW10@30 = 993Mbps, 2 lane @600MHz DDR
(1200Mbps) 刚好装下; 前摄 2592x1944 RAW10@30 = 1.51Gbps,
2 lane @419.2MHz DDR (1.68Gbps) 装下. 结论: **都是 2 lane**,
lane 顺序待运行时确认 (先按标准 `<0 1>` / camss 侧 `<0 2>`).

## 3. 上游驱动盘点 (内核 `v6.12.1-msm8916`)

| 块 | 状态 |
|---|---|
| camss (`qcom,msm8939-camss`) | **缺**: `camss.c` 只认 8916/8996/sdm660/sdm845/sm8250/sc8280xp. 8939 与 8916 同代 (`csiphy-v3.1`/`csid-v3.1`/`ispif-v3.0`/`vfe40`), 只需加 `msm8939_resources` (csiphy x2 + csid **x3** + ispif + vfe x1) + binding. `CONFIG_VIDEO_QCOM_CAMSS=m` 已开 |
| CCI | 现成 (`i2c-qcom-cci` 认 `qcom,msm8916-cci`, `CONFIG_I2C_QCOM_CCI=m` 已开). `V4L2_FWNODE`/`V4L2_ASYNC` 都是 `m`, 已开 |
| OV5693 前摄 | 现成 (`ov5693.c` + binding). 要求 supplies `avdd/dovdd/dvdd`, clock `xvclk`, endpoint `link-frequencies`. `CONFIG_VIDEO_OV5693` 当前 **not set**, 用时打开 |
| OV13850 后摄 | **缺** (主线只有 ov13858/ov13b10). Sebastian Fricke 的 GPL 驱动 (`initBasti/OV13850_camera`, 曾投 linux-media v3, 含 2112x1568 + 4224x3136 全表, xclk 24MHz, link 300MHz) 可作寄存器表来源, 但它是 RK 系写法 (`rk-camera-module.h` DT 属性), 需改写为标准 supplies/clock/GPIO + CCI 风格 |
| DW9761 AF | **缺** (`dw9714.c` 只支持 dw9714, `CONFIG_VIDEO_DW9714` not set). defer |
| LM3646 | 有 `lm3646.c` 但无 OF match (老式 `i2c_device_id` 写法). defer |
| SoC DTS | `reference-dts/msm8939.dtsi` 的 camss 节点已起草 (csid2 + vfe0_vbif, 24 clocks, 7 IRQ, 需 `vdda-supply`, `pm8916.dtsi` 里已补 `vdda=<&pm8916_l2>`), **但还没接入 APKBUILD** |

## 4. 移植步骤

### Step 1 — 内核 SoC 层 (不依赖传感器型号, 可先行)

> 已落地 (2026-10-16, pkgrel 48): `camss/` 下三文件,
> pmaports 已接线. `&camss`/`&cci` 在 device DTS 里先行
> `status="okay"` (无 sensor endpoints, 只验证驱动 bind).
> 教训: tarball 版 `msm8939.dtsi` 的 `apps_iommu` 漏了
> `iommu-ctx@3000` (vfe, asid 3; 8916 的 dtsi 里有),
> camss `iommus = <&apps_iommu 3>` xlate 报
> `WARN_ON(ctxs[asid] == NULL)` — dtsi patch 里已补上.

1. `drivers/media/platform/qcom/camss/camss.c`: 加
   `msm8939_resources` (照抄 `msm8916_resources`, csid 加第 3 路,
   clocks/interrupts/reg-names 照 `reference-dts/msm8939.dtsi`
   的 camss 节点), `of_match` 加 `qcom,msm8939-camss`.
2. 新建 binding `qcom,msm8939-camss.yaml` (照抄 8916 版:
   clocks 24 / interrupts 7 / reg 11).
3. `APKBUILD prepare()`: 除 device dts 外, 另拷贝
   `reference-dts/msm8939.dtsi` + `msm8939-pm8916.dtsi`
   覆盖同名文件 (camss 节点 + `vdda-supply` 在里面).
4. `config-*.aarch64`: `CONFIG_VIDEO_OV5693=m`
   (前摄先行; 后摄驱动落地时再加 `CONFIG_VIDEO_OV13850=m`).

### Step 2 — 设备 DTS (`msm8939-lenovo-phoenix.dts` 追加)

> 已落地 (2026-10-17, pkgrel 49): 前后摄节点全部进 device DTS —
> 前摄 ov5693 即刻可 bind (上游驱动已配 `CONFIG_VIDEO_OV5693=m`),
> 后摄.ov13850 节点为占位 (等 Step 3 驱动)。
> 事实核对结论: 前摄 GPIO 27(mclk1)/28(rst)/33(stby)、后摄
> 26(mclk0)/35(rst)/34(stby) 与 `msm8939.dtsi` 上游
> `camera_front/rear_default` 完全重合; 供电 vdig=L2(1.2)/
> vana=L17(2.85)/vio=L6(1.8) (无 L10=vaf, AF 属二期).
> 驱动端差异记录: ov5693 驱动按 19.2MHz MCLK 出表 (DT 里
> assigned 19.2MHz, 非 stock 24MHz); 后摄按 stock 24MHz.

#### r49 实测 → MCLK 时钟表 bug (pkgrel 50 修复)

> 现象: `Found clk freq 17777777, expected 19200000` + 随后
> 读 chip id (0x300a) -ENXIO, 前摄 probe 失败.
> 根因 (内核侧, 非 DTS): gcc-msm8939.c fork 里
> `ftbl_gcc_camss_mclk0_1_clk` 抄错 — `F(24000000,P_GPLL0,1,1,45)`
> 实算 = 800MHz×1/45 = **17.7777MHz**, 表内标称 24M 与硬件
> 实际频率不自洽 (8916 上游原版是 `F(23880000,P_GPLL0,1,2,67)`
> = 800×2/67 = 23.8806MHz, 即上游 mezzanine dts assigned
> 23880000 的由来; 8939 移植时错抄成 (1,1,45)).
> 修复: `gcc-msm8939-camss-mclk.patch` 重写该表 —
> `F(9600000,P_XO,3,0,0)` / `F(19200000,P_XO,1,0,0)` (XO 直出,
> 前摄精确 19.2) / `F(23880000,P_GPLL0,1,2,67)` (上游原档保留) /
> `F(24000000,P_GPLL0,1,3,100)` (MND 3/100, 后摄精确 24M, stock) /
> `F(66670000,...)` 原样保留. rcg2 MND 语义:
> rate = parent × 2/(hid+1) × m/n; `qcom_find_freq` 需升序表,
> 设 rate 走 CEIL — r50 起 19.2/24 均精确命中, 警告应消失.
> 注: mclk0/mclk1 共用该表 (两档位同修).

#### r49 实测 → "/dev/media0 不见了" 的解释 (非 bug)

> 6.12 camss `camss_probe` 逻辑: `camss_of_parse_ports` 把每个
> port 的远端 (sensor) 注册成 v4l2-async 匹配; 若 num_subdevs>0,
> `media_device_register` 只在 async notifier 的 complete 回调
> (`camss_subdev_notifier_complete` 尾部) 执行 — 即**所有**匹配
> 的 sensor 子设备都绑定后 media0 才出现. r49 起 DTS 有两个
> endpoint → ov13850 无驱动 (Step 3) + ov5693 probe 失败 →
> complete 挂起 → 无 /dev/media0 属预期. Step 3 驱动落地后
> media0 与 v4l-subdev* 一并出现. (若想先只调前摄看拓扑, 可把
> &camss 里的 port@0 块临时删掉 — 异步匹配就只剩 ov5693, 绑定
> 后 complete 触发, media0 带单链拓扑出现; 单 disable ov13850
> 节点没用, endpoint 仍会进 notifier 等待.)

#### r50 实测 → standby 极性根因 (pkgrel 51 修复)

> 彻查 stock: `msm8939-camera-pb1770m.dtsi` 原生就有
> `qcom,cam-power-seq-*` 上电序列 (camera_v2 dt 解析+执行,
> 无需 .so). 执行器语义 (msm_camera_dt_util.c): SENSOR_GPIO
> 条目 = legacy `gpio_set_value(gpio_num, config_val)` **raw
> 物理电平**, 无任何极性翻译; vreg/clk 分支另执行; delay 单位 ms.
>
> **[r52 补注]** 这套 DTSI 序列块在文件尾部被
> `/delete-node/ qcom,camera@0/@1` + 重定义干掉了 (生效节点无
> cam-power-seq, stock 运行时序列来自 sensor .so), 所以下列
> 序列只是"被删残片"的语义参考; 但 r52 实测 (前摄 ACK@0x10)
> 最终证明 r51 依它推导的 ACTIVE_LOW 组合**实际有效**.
>
> stock 前摄 (camera@1): L6(10ms)→L17(10)→L2(10)→RESET=1
> (10)→STANDBY=1 (10)→MCLK 24MHz(5); 后摄 (camera@0): L6(1)→
> L17(1)→L2(5)→MCLK 24M(5)→RESET=1(5)→STANDBY=1(10).
> 两者都挂 `qcom,cci-master = <0>` (bus 3, 我们的接线正确).
>
> 物理语义实锤: 两模块 STANDBY/RESET — cfg_val=1 → 物理高
> = "释放复位/传感器活跃"; 故模块 **standby 是 active-LOW**
> (拉低=睡眠, 高=活), reset 同为 active-low.
> ⇒ 上游 gpiod 语义下 `powerdown-gpios` 必须标 **ACTIVE_LOW**
> (驱动逻辑 0=释放 standby → 物理高=活). 我们此前把
> powerdown 标 ACTIVE_HIGH → 驱动"释放 standby"时物理输出
> LOW = 把传感器拉进睡眠 = r50 窗口内 NACK 的根因
> (也解释了竞态扫描全空).
>
> r51: 前后摄 `powerdown-gpios` ACTIVE_HIGH→ACTIVE_LOW
> (reset 两枚保持 ACTIVE_LOW). stock 24MHz vs 上游 19.2MHz
> 偏差维持原记录 (MCLK 表修复后 19.2M 精确可用).

```dts
&camss { status = "okay"; ports { port@0 { reg = <0>;
    csiphy0_ep: endpoint { data-lanes = <0 2>;
      remote-endpoint = <&ov13850_ep>; }; };
  port@1 { reg = <1>; csiphy1_ep: endpoint { data-lanes = <0 2>;
      remote-endpoint = <&ov5693_ep>; }; }; }; };
&cci { status = "okay"; };
&cci_i2c0 {
  ov13850: camera@36 { compatible = "ovti,ov13850"; reg = <0x36>; /* 占位, Step 3 定音 */
    clocks = <&gcc GCC_CAMSS_MCLK0_CLK>; clock-names = "xvclk";
    assigned-clocks = <&gcc GCC_CAMSS_MCLK0_CLK>;
    assigned-clock-rates = <24000000>;
    avdd-supply = <&pm8916_l17>; dovdd-supply = <&pm8916_l6>;
    dvdd-supply = <&pm8916_l2>;
    reset-gpios = <&tlmm 35 GPIO_ACTIVE_LOW>;
    powerdown-gpios = <&tlmm 34 GPIO_ACTIVE_LOW>;
    rotation = <90>; orientation = <0>; /* 后摄, 待定 */
    port { ov13850_ep: endpoint { data-lanes = <1 2>;
      link-frequencies = /bits/ 64 <300000000>;
      remote-endpoint = <&csiphy0_ep>; }; }; };
  ov5693: camera@10 { compatible = "ovti,ov5693"; reg = <0x10>; /* 实测 chip-id 0x5690 (r52) */
    clocks = <&gcc GCC_CAMSS_MCLK1_CLK>; clock-names = "xvclk";
    assigned-clocks = <&gcc GCC_CAMSS_MCLK1_CLK>;
    assigned-clock-rates = <19200000>; /* 上游表按 19.2M, 非 stock 24M */
    avdd-supply = <&pm8916_l17>; dovdd-supply = <&pm8916_l6>;
    dvdd-supply = <&pm8916_l2>;
    reset-gpios = <&tlmm 28 GPIO_ACTIVE_LOW>;
    powerdown-gpios = <&tlmm 33 GPIO_ACTIVE_LOW>;
    rotation = <270>; orientation = <1>; /* 前摄, 待定 */
    port { ov5693_ep: endpoint { data-lanes = <1 2>;
      link-frequencies = /bits/ 64 <419200000>;
      remote-endpoint = <&csiphy1_ep>; }; }; }; };
```

pinctrl 沿用 `msm8939.dtsi` 已有的 `camera_rear_default` /
`camera_front_default` (mclk+reset+standby 引脚全对上).
注意 `orientation`/`rotation` 语义待运行时按成像方向修正.

### Step 2 现场记录 (r48-r52)

- **r48/r49**: camss 内核层 + DTS 落地, boot 干净; ov13850 无驱动属预期,
  ov5693 probe 报 `-6` @ reg 0x300a; `/dev/media0` 不出现属 6.12 上游行为
  (async complete 需所有端点绑定, 已在 r50 节解释).
- **r50**: `gcc-msm8939-camss-mclk.patch` 修 MCLK 表, 实测 mclk0/1 = 精确 19.2M.
- **r51**: standby 极性修复 (powerdown 33/34 改 ACTIVE_LOW, 依据 stock
  raw 上电序列尾部 STANDBY=物理高 ⇒ active-LOW). 之后 `-6` 仍在,
  且 8 轮竞态扫描 (probe 窗口 ~7.5ms, 驱动上电尾段
  `powerdown 0 → reset 0 → usleep(5-7.5ms)` 后立刻读 ID) 对 0x36/0x6c
  **零 ACK**, 连 EEPROM 0x6d 也从未应答过 — 窗口太短, 竞态命中率不足,
  无法判死地址假设.
- **r52 (调试补丁 `ov5693-bringup-retry.patch`, bring-up 用完删)**:
  `ov5693_detect()` chip-id 读取重试 ~2.5 s (25×100ms), probe 失败路径不变,
  但窗口从 7.5ms 拉长到 2.5 s, 足够手动跑全地址扫描:

  ```
  apk add i2c-tools
  rmmod ov5693
  ( for i in $(seq 1 12); do i2cdetect -y -r 3; sleep 0.2; done ) &
  modprobe ov5693     # 2.5 s 内 sensor 保持唤醒 (claimed/上电/MCLK/复位释放)
  kill %1 2>/dev/null
  ```

  结果解读:
  - **有 ACK** → 那就是真实地址 (0x6d=EEPROM 同模块同轨, 顺手验证轨); 改 DTS
    `reg` 出下一版.
  - **全部 NACK** → 地址假设彻底死亡, 排查转向: 供电轨真实状态
    (`/sys/class/regulator/*/state`, 找 l6/l17), GPIO 33/28/27 占用与电平
    (`gpioinfo`), MCLK enable (`clk_summary`); 两轮极性组合 (r50 的
    ACTIVE_HIGH 与 r51 的 ACTIVE_LOW) 都已失败, 剩余假设 = 轨未实到模块 /
    模块损坏 / CCI-I2C 电气问题.
- **r53 (实测定址, Step 2 收官)**: r52 的 2.5 s 窗口内
  `i2ctransfer -f -y 3 w2@0x10 0x30 0x0a r2` 返回 `0x56 0x90`
  = **OV5693 chip-ID** — 前摄真实地址 = **0x10**, 且 r51 的极性/供电/MCLK
  组合被一并实证 (ACK 发生在驱动持锁窗口内). 后摄 standby 浮空 = 睡眠,
  窗口内全地址无 ACK → 后摄地址无法离线测得, reg 暂置 0x36 占位,
  Step 3 probe 定音; DTS 已改 (前 camera@10, 后 camera@36), pkgrel 53,
  本地 dtb 编译通过.
  **测量工具备注**: `i2cget` 不支持 16 位寄存器地址 (SMBus 单字节命令,
  0x300a 直接拒收), 之前用 `i2cget 0x36 0x300a` 的竞态扫描是无效实验;
  16 位寄存器一律 `i2ctransfer -f -y 3 w2@<addr> <hi> <lo> r2`.
- **stock HAL 深挖结论 (slave 地址静态不可得)**:
  `libmmcamera2_sensor_modules.so` 的 `.data.rel.ro` 有模块注册表
  (`{"ov13850","ov5693"}` 名字数组 + 11 个 .text 入口函数),
  `.data` 内嵌两份 per-sensor 描述块: `sensor_name[32]` + `actuator_name[32]`
  ("ov13850"/"dw9761", ×2 = 双分辨率配置) + AEC 浮点参数表 (增益/线数回归系数),
  **无 slave_info/地址**; `libmmcamera_ov5693.so`/`libmmcamera_ov13850.so`
  只有寄存器表, 也没有名字块. 下游 DTS (qrd 变体) 只给 eeprom 地址
  (`sunny_q13v04b@0x20` / `sunny_q5v41b@0x6d`), sensor 地址由 HAL 从
  sensor_lib_t 运行时注入, 静态任何途径都拿不到 → r52 全扫描是终局判据.
  (副产品: 内嵌描述块是 Step 3 DW9761 actuator 的现成参数来源, 二期取用.)

### Step 3 — 后摄 ov13850 驱动 (新文件, 参考 BUILD.md 模式进 pmaports)

以 Fricke 驱动的寄存器表为准 (init + 2112x1568 + exposure/gain),
骨架照抄上游 `ov5693.c` (标准 supplies + `xvclk` +
`reset/powerdown` + `v4l2_async_register_subdev_sensor`,
link-freq 300MHz, pixel-rate 按 vt 240MHz 折算).
stock 全幅分档 (4208x3120) 首版不做.

#### r54 (pkgrel 54): 驱动落地 (`camera/ov13850.c` + `.kconfig`)

- **骨架**: 主线风格, 照抄 `ov5693.c` v6.12 结构 — regmap-cci
  (`devm_cci_regmap_init_i2c`, CCI_REG8/16/24), runtime PM
  (autosuspend 1s), `v4l2_async_register_subdev_sensor`,
  `check_hwcfg` (2 lane + link-freq 300M 校验), supplies
  `avdd/dovdd/dvdd` (= l17/l6/l2), GPIO `reset`(35)/`powerdown`(34)
  = ACTIVE_LOW (r51/r52 已实证的极性), mclk0 = 24 MHz.
- **寄存器表的来源分类** (stock 表 ≡ RK r2a + Lenovo 特调):
  - global init = **stock vendor 表 240 条** (从
    `libmmcamera_ov13850.so` 解码, 8B 块格式; 与 RK r2a 逐位一致
    的部分直接照搬, 只在 44 处差异中分类):
  - **取 RK 值** (流时序/PLL, 保证与模式表自洽):
    `0x3012`(0x41→0x21), `0x3801..0x3815` 全族 (stock 默认开窗
    是 1056x594, RK 是 2112 窗; 模式表每次 start 都会重写窗),
    `0x3834/0x3837` (MIPI 时序), `0x4601`, `0x5001`;
  - **留 Lenovo 值** (本模块模拟/OTP/校准特调):
    `0x3605/0x3702/0x370a/0x372f/0x3718(final 0x1c)`,
    `0x3501/0x3502`(默认曝光, ctrl 会覆盖), 0x4020 族/0x5401/0x5405
    (被模式表重写或在 stock 内为最终写值),
    + RK r2a 没有的 26 条 `0x371x..0x3782` 块与 10 条 `0x5xxx` 块;
  - **不引入** RK-only 的 `0x4006/0x4007/0x5000` (stock 无, 不猜).
- **模式表**: RK 下游原值 — `2112x1568@30` (HTS 0x12c0, VTS
  0x0680, 2x2 binning) + `4224x3136@7.5` (VTS 0x0d00) 两档,
  SBGGR10_1X10, RAW10, 2 lanes @ 600 Mbps/lane (link-freq 300M).
- **controls**: 曝光 = `CCI_REG24(0x3500)` 行数<<4 (RK 注释同
  ov5693: "4 LSB are fractional"), ANALOGUE_GAIN =
  `0x350a/b` 11bit raw (0x10=1x, 0xf8=15.5x, RK 语义),
  VBLANK 田 `VTS = val + height`, HBLANK 只读 (HTS 固定),
  TEST_PATTERN `0x5e00=0x80`.
- **detect() 自带 2.5s 重试窗口** (驱动内建, 不再需要 patch):
  chip-id 读失败每 100ms 重试共 25 次 — 模块全程保持上电,
  userspace 可扫总线; 成功即 short-circuit.
- **probe 顺带读 `0x302a` 记录 chip revision** (若 stock 表是
  r2a 对应版, 后摄 rev 变体问题可直接从 dmesg 诊断).
- **接线**: APKBUILD source += ov13850.c/.kconfig (占位 hash), 
  prepare() 复制到 `drivers/media/i2c/` + 追加 Makefile/Kconfig行,
  内核 config `CONFIG_VIDEO_OV13850=m`, pkgrel 54.
- **本地交叉编译验证**: 完整 6.12.1 源树上
  `make drivers/media/i2c/ov13850.o` (aarch64 cross-gcc) 通过,
  零警告零错误.

#### r55 (pkgrel 55): runtime PM 类型混淆 oops 修复

- **bug**: `ov13850_sensor_suspend/resume` 把
  `dev_get_drvdata(dev)` 的返回 (= `v4l2_i2c_subdev_init` 塞进去的
  **`&ov13850->sd` subdev 指针**) 直接当 `struct ov13850_device*`
  用 — 字段偏移全错位, `->reset`/`->supplies` 读到 sd 结构内部
  的野值, `gpiod_set_value_cansleep` 解引用即
  "Unable to handle kernel paging request" oops.
- **触发时机**: probe 成功后 `pm_runtime_put_autosuspend` →
  autosuspend 1 s 到点跑 `runtime_suspend` ≈ udev 冷插加载模块
  ~1 s 后 oops, 杀掉模块加载线程 → 后续设备初始化断 →
  表现为"开不了机".
- **fix**: 恢复 ov5693.c 的两步取指
  (`to_ov13850_sensor(dev_get_drvdata(dev))`), pkgrel 55.

### Step 4 — 验证 (无需固件, 纯寄存器驱动)

#### r54 首启观测

1. `dmesg | grep ov13850`:
   - 成功 = `ov13850 3-00xx: OV13850 rev 0x?? detected` + 无错
     (前摄静默样式). **两枚 sensor 都绑定后 `/dev/media0` 与
     `v4l-subdev*` 立即出现** (异步匹配 complete — Step 2
     收官后的里程碑).
   - 失败 = `ov13850 3-0036: chip id read failed after 2.5s window`
     (dt reg=0x36 是占位, 真地址需扫) → 立即跑:
     ```sh
     rmmod ov13850
     ( for i in $(seq 1 60); do
         for a in $(seq 8 126); do
           T=$(i2ctransfer -f -y 3 w2@0x$a 0x30 0x0a r2 2>/dev/null) \
             && { echo "ACK@0x$(printf %02x $a) -> $T"; }
         done
       done ) &
     modprobe ov13850; wait
     ```
     `0xd8 0x50` = ov13850 真身 → 改 DTS reg → 下一个 pkgrel.
2. `media-ctl -p` 看拓扑, `media-ctl -l` 建 link.
3. `v4l2-ctl --stream-mmap` 经 VFE RDI 抓 RAW 帧.
4. 时序对不上时先降级: 单 lane / 降 link-freq / 降分辨率
   逐个二分, lane 顺序与 `data-lanes` 互换对照.

#### r55-r59 出帧后: 帧内容位级损毁的破案记录 (核心现场)

三级链接已闭环 (csiphy0→csid0→ispif0:RDI→vfe0_rdi0, 2112x1568 pBAA,
~30fps 稳定出帧), 但解码出的像素呈**确定性损毁签名**:

```
像素值域 = {0(63%), 1/2/3(~12%), 172-179(~25%)} 共 11 个离散值
位面: bit6/bit8/bit9 恒 0; bit5==bit7; bit0-4 同步翻动
Bayer: R/B 偏向 {176,178,2,179}; G 偏向 {172,173,1,174} (倾斜非分类)
字节位面: MSB 字节(b0-b3) 存活~26% (值几乎只有 0x2b/0x2c),
          尾字节(b4) 存活~45% (值 = 单 bit 对 {02,08,20,40,80})
跨重启/跨天逐像素一致 ~76%; 相邻帧仅 63.7% 相等 (36% 逐帧抖动)
列剖面 = 类图像块状 (无周期峰, 块长直方图峰在 4 列)
```

**关键排除实验 (全部活体完成, 无需重编内核):**

1. **lane_assign 编码锤定** (`camss-csid.c: csid_get_lane_assign`):
   `lane_assign |= data[i].pos << (i*4)`; CSID `CORE_CTRL_0(bit0-3) =
   lane_cnt-1 | lane_assign<<4`. DTS `<0 2>` → lane_assign=0x20 →
   reg=0x201 = 活体读值 ✓. 即 RX 拓扑 = 时钟 pair1 / 数据 pair0,2.
2. **lane 重排热扫** (qmmio poke 改 CSID 0x1b08004):
   - `[2,0]`(0x21) → 流即刻死 (包头在 lane0, 错位即丢帧)
   - `[0,1]`(0x101) → 2 帧后挂死 (pair1 = 时钟 lane 无数据)
   - `[0,3]` 类推预期同死; **lane1 唯一能出帧的 pair = 2**
   ⇒ 物理 lane 拓扑与 DTS 完全一致, **lane 映射非损毁源**.
   注意: 错映射会卡死 CAMSS (VFE sof timeout), 只能 reboot 恢复.
3. **sensor 寄存器 A/B 电池** (i2c 直接改活体, 读回确认生效):
   - S1-S5: 0x3820(02→01)/0x3821(05→06)/0x370a(26→a9)/0x372f(90→88)
     /0x3718(10→1c)/0x4601(04→83)/0x4603(00→01)/HTS 4800→2400
     /VTS 1664→3328/0x4022-25(07,5f,08,44→04,d7,05,bc): **帧零变化**
   - 曝光 ÷16 (0x3501/02 → 0x02/0x9a): **帧零变化**
   - 增益 ×8 (0x350a/0b → 0x04/0x00): **帧零变化**
   ⇒ **像素数据不随 sensor 模拟路径任何参数变 → 数据不是感光阵列输出**
4. **stock 库解码对照** (libmmcamera_ov13850.so `.data`, 22KB 纯数据库):
   stock 模式 A = 同 2112x1568 (HTS 2400 VTS 3328), 0x3012=0x41
   (sunxi 注释 "MIPI 4 lane"; RK 二 lane 板 = 0x21; 本机 2-lane 线),
   0x4601=0x83/4602=0x22/4603=0x01 — **我方公共表本就有这些值**,
   是 RK per-mode 覆写表把它改成 04/00 (但 A/B 证明无关).
   stock 曝光更新序列带 group-hold 包裹 `{3208=0x10 … 3208=0xa0}`,
   后续驱动对帧要补 (写组先后 launch).
   手机侧 CCI 语义纠错: **16-bit 寄存地址 + 8-bit 数据**
   = 写用 `w3@0x36 <hi> <lo> <val>`, 读用 `w2@0x36 <hi> <lo> r1`;
   `w4` 会把第 3/4 字节写进相邻两个寄存器 (曾把 4602 写花).

**已试未通的路 (记录, 防重复):**

- CSID test generator: 链接使能时控件 `Resource busy`; 断链设上后
  直流启动 = VFE sof timeout; 未解 (TG	throws 未带起来).
- 前摄 ov5693 走 csiphy1→csid0 同路: SOF timeout, 前摄链自身未 bring-up.
- hot-poke 期间 v4l2-ctl 日志空 = stdout 全缓冲, 不代表没出帧.

**当前工作假设:** 损毁发生在 CSID→ISPIF→VFE→WM 的数字路径上 —
管道以视频速率自产 8 符号字母表的**确定性数字载荷**, 而非透传阵列数据
(Bayer "通道劈裂" = 位置 mod4 伪迹, 非真色彩通道). 下一步方向:
1. 对拍 stock camss `camera_v2` (msm-csid/ispif/vfe3.10 源) 的
   RDI raw-dump 寄存器序列, 与上游 vfe-4-1/csid-4-1/ispif 逐一对照.
2. 逐字 tap: qmmio 读 CSID 溢出计数 / ISPIF pack / VFE RDI 寄存器
   (0x2c/0x30 IRQ 掩码, bus master 全集) 与 stock 对照.
3. 若怀疑 settle/链速率, 对照 stock 0x4837=0x19 与我方 vfe 时钟参数.

**运维注意 (本节实操教训):**

- 杀**流中**的 v4l2-ctl 会夹死 CAMSS, 只能 reboot; 40s 恢复.
- reboot 清 tmpfs, /tmp 脚本与抓帧全部散失, 重推.
- 长任务 = 设备端脱离 (`nohup/setsid` + 轮询 done 文件), 防 ssh 超时
  连带杀流; 但 busybox 环境下嵌套引号最容易碎 — 复杂命令一律
  写脚本→scp→`echo 123456 | sudo -S sh /tmp/x.sh`.
- 大文件回拉 = 设备端 `dd` 切帧 (帧 = 4,139,520 B) 再 scp_from.
- `journalctl -k` 才能看到 CAMSS 的 sof timeout 字样 (dmesg 无权限).
- 手机侧也会 OOM 重启（用户警示已应验：仅 ping 通、sshd 拒接 >40min =>
  需人工 power-cycle；tmpfs 抓帧随重启散失；本地重要产物一律先
  camera-ref 持久副本, /tmp 只作 scratch）.

### r60 本地持久库 + stock↔上游寄存器序列全对拍（设备掉线期完成）

- **本地持久化**（用户警示 OOM 重启已应验）：`/tmp` 被清。参考源码/工具 =
  `~/workspace/agent-workspace/camera-ref/`（stock camera_v2 + dtsi、上游 camss-6.12、
  devssh/scp 助手版、safari15.sh）。助手改走固定 venv：
  `camera-ref/tools/.venv/bin/python /tmp/opencode/devssh.py <s> '<cmd>'`
  （pexpect 4.9 无 `Timeout` 属性，旧代码按宽 catch 处置）。
  设备 tmpfs 上 ~270MB raw 证据随手机掉线不可取（待恢复后优先回拉）。
- **基址澄清（真相）**：我方 DTS（msm8939-camss-dtsi.patch）＝ stock msm8939-camera.dtsi：
  vfe0=0x1b10000、vfe0_vbif=0x1b40000、ispif=0x1b0a000、csid0=0x1b08000、
  csid2=0x1b08800、csiphy0=0x1b0ac00、driver patch = CAMSS_8x16 + csid×3。
  上一轮 safari14 用了 8974 的 0x1a00000/0x1b31000 = **错误基址（无效）**；
  safari15 已按正确基址重写：VFE 全 0x400 单窗 + VBIF 0x200 + ISPIF 0x500
  ＋ CSID 0x100 ＋ CSIPHY 0x200 ＋ DS 0x988×24 词＋残询 0x1b31000×64 词。
- **stock init 写全集**（vfe 从 DTS "qcom,vfe40" 读出）：QOS 0x2c4-0x2e0 =
  0xAAA5AAA5×7+0x0001AAA5 —— **上游 4-1.c L743-749 同值同写，排除**；
  DS 17 项 @vfe+0x988..0x9c8 = 0xCCCC1111×16+0x103 —— 上游不碰（stock-only）；
  VBIF(vfe_vbif) 0x30/0x34 = 0xfff/0x555000 —— 上游不碰。
- **数字路径差异终版**（live=0x9/0x10007/0x155/0x0 为本机活体实测）：
  1. CSID ctrl1 @0x1b08008: live=0x9（上游 csiphy<<17|0x9）；stock=phy_sel<<17|0xF
     （bits[1:2] 语义待查，可疑度低：数据链路活着）。
  2. VFE RDI_CFG_0 @0x1b102e8: live=0x10007（MIPI_EN 0x3｜RDI_EN BIT2｜FB BIT16）；
     stock 在 +n 写处 `&=0x70003` 保留复位值 → 若复位 [1:0]=0 则 = 0x10004。
     差异仅 bits[1:0]。
  3. VFE UB_CFG wm0 @0x1b1007c: live=0x155（上游 depth=1023/3=341，写法无 -1）；
     stock = `ub_offset<<16|(wm_ub_size-1)`，单流占满 = 0x0A77（2679）。**深度差 8×**。
  4. VFE WR_BUFFER_CFG wm0 @0x1b10084: live=0x0（**现在实锤**：上游
     vfe_wm_frame_based() 只置 WR_CFG bit1，从不写 +0x14/+0x18）；
     stock frame-based = wpl<<16|(h-1)<<4|burst = 0x029461F3（660/1567/3）。
     **结构类首选**（burst/步进几何 ↔ mod4 位级伪迹签名）。
  5. WR_IMAGE_SIZE @0x80: stock frame-based 同样不写（非 frame-based 才写）→ 同 0，排除。
  6. CSIPHY settle（CFG2=0x10/CFG3=0x0b live）：stock 值待由 P7 A/B 对照。
- **poke 优先级**（一个 poke 一轮 16 帧抓帧 → 与基线比 pinned 值）：
  P3 BUFFER_CFG=0x029461F3 → P2 UB=0xA77 → P4 RDI bits[1:0]→0（0x10004）
  → P1 ctrl1=0xF → P5 VBIF=0xfff/0x555000 → P6 DS=0xCCCC1111… → P7 csiphy 扫。
  P3-P6 经 qmmio 中流写，风险=流挂则 reboot；P1 同类注意。

## 5. 风险

- 从地址/ lane 顺序是解码 + 惯例值, 必须 `i2cdetect` + 抓帧复核.
- 前摄 MCLK 用 19.2M (偏离 stock 24M): sensor 支持 6-27MHz,
  上游 PLL 表按 19.2M, 是正确选择; 若抓帧时序异常再试 24M
  并按比例修正 link-freq.
- 全幅 13MP 分档主线 VFE410 带宽与 camss 驱动是否吃得下,
  首版不验证, 先稳定 2112x1568.
- AF (DW9761) / 闪光灯 (LM3646) / EEPROM 全部二期, 不 blocking.
