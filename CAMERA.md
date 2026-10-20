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

**r61 勘误后终版** (decode2.txt, `/tmp/opencode/pbfw/ex/`, 全字段重验证):
后摄 6 分档 (out_info_array + res_settings_array), lane 全部 **4 lane**
(csi_lane_params {lane_assign 0x4320, lane_mask 0x1f, settle?, 0};
0x3012=0x41 遍布全部 7 表; Mi8937 rolex 的 ov13850 XML 同值三方印证):

| 模式 | 输出 | HTS×VTS | vt_pclk | op_pclk | max_fps | MIPI 位率/lane |
|---|---|---|---|---|---|---|
| m0 | 4208x3120 | 4512×3328 | 448M | 480M | 30 | 1.2 Gbps (link 600M) |
| m1 | 2112x1568 | 2400×3328 | 240M | 256M | 30 | 640 Mbps (link 320M) |
| m2 | 2104x1184 | 2400×3328 | 240M | 256M | 30 | 640 Mbps |
| m3 | 1056x594 HFR60 | 2968×1348 | 240M | 240M | 60 | 600 Mbps (link 300M) |
| m4 | 1056x594 HFR90 | 2968×898 | 240M | 240M | 90 | 600 Mbps |
| m5 | 1056x594 HFR120 | 2968×674 | 240M | 240M | 120 | 600 Mbps |

op_pixel_clk 语义 = MIPI 输出像素率 (bit/lane = op×10/4): m1 的 256M →
640Mbps/lane 与 {m0 480M→1.2G, HFR 240M→600M} 三组全闭合; vt/op 差 =
sensor 内部行周期时钟 vs CSI 发送时钟. 表内 AEC 种子: 3500-3502 = 
{m1/m2: 0x006000 (1536 行), m0: 0x00C000 (3072), HFR: 0x0029A0 (666)},
增益种子 350a/b=0x80. init_settings = {0103=1, delay 0x32} 单条 +
217 表 (表首 0103=1 重复); stock group-hold 单例 {3208=0x10}/{3208=0xa0}
(驱动首版未加, r62 后视撕裂补). slave_info: i2c 0x6c (8-bit) = 0x36/7bit.
lens: f=3.68, f/2.2, hFOV 65.45°vFOV 50.65°.

勘误记录: 前版 "2 lane @600Mbps/link 300M; 仅 2112x1568 一档; vt=448M/op=480M
在老表里写成 449,581,056/481,644,544 等错值" 全部由 r61 全字段重解码取代
(旧错 = 手工 u32 对齐位数不同, 0x3012 "stock=0x13" 的误读同批作废).

前摄: **2592x1944**, line=2672, frame=1984.
上游 ov5693 驱动自带该分档 (LINK 419.2MHz, PIXEL 167.68MHz,
XVCLK 19.2MHz), 与 stock 行场参数同量级, 运行时复核.

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

### r61 stock 全表 verbatim 实施 + 4-lane DTS（纯离线完成，待设备验证）

- **触发**：用户指示「开始实施」，采用 r60 前定案的 rework 方案
  （verbatim stock 表 + 4-lane DTS），实施前全部先经离线复核。
- **mainline 4-lane 可行性锤定**（免改 camss patch 的依据）：
  1. `camss-csiphy-2ph-1-0.c`：`csiphy_get_lane_mask = 1<<pos 逐 lane | CLK`，
     `lanes_enable` 按 data_lanes 逐 lane 写 LNn_CFG2/CFG3，**无 lane 数上限**；
     寄存器窗口 0x200 覆盖 LN4（0x104）。
  2. `camss-csid-4-1.c`：`lane_cnt-1 | lane_assign<<4`，nibble 直通，无校验拦截；
     `csid_get_lane_assign |= data[i].pos<<(i*4)` 与 stock 0x4320 编码同构。
  3. `v4l2-fwnode.c`(v6.12) CSI2 解析只查重复、无 lane 值上限（数组 8 槽），
     `<0 2 3 4>` 全链路合法。
  4. stock 8916/8939 camera dtsi 同地址同 compatible（csiphy-v3.1@1b0ac00），
     同代 PHY 硬件 4-lane 有出厂背书（m0 = 1.2Gbps/lane）。
  结论：**camss patch 零改动**，纯 DTS 表达。
- **link-freq 基准锤定**：out_info vt/op 全字段重解析（脚本），
  m0 vt=448M/op=480M、m1/m2 240M/256M、HFR 240M/240M（此前 449,581,056
  等 = 手工 u32 错值，作废）。op×10/4 = 每 lane 位率：1.2G/640M/600M，
  DDR link = 600/320/300 MHz，三组闭合（m3 240M/2968×1348/60fps 自洽）。
  settle 由 sensor 的 V4L2_CID_LINK_FREQ 运行时取（csiphy ep 无需 link-freq）。
- **`camera/ov13850.c` 重写**（995→1236 行）：
  - 7 表全部 verbatim（2107 条寄存器，脚本提取零转写）：
    init 217（首条 0103=01 重申单例）+ m0 249 + m1/m2 241 + m3/m4/m5 240；
    边界对拍全部严丝合缝（init 止于 m3 首、m4 止于 out_info、m0 止于 0x6124）。
  - 模式表 = stock res_settings_array 顺序重排：m1(默认)/m2/m0/m3/m4/m5，
    附 hts/vts/pclk/link_freq_idx/行数 exp_def（表内 AEC 种子 >>4：
    1536/1536/3072/666）与 regs/nregs 指回 verbatim 表。
  - 控制：LINK_FREQ 三项 int-menu + PIXEL_RATE per-mode（set_fmt 时
    `__v4l2_ctrl_s_ctrl` 同步 → csiphy settle/VFE floor 随模式走）；
    hblank 随模式 [hb,hb] 锁定（不再 read-only 旗标 + s_ctrl 直写路径）。
  - init 流程补 stock 语义：`0103=0x01` 单例 + 50ms（wrapper delay 0x32）
    再进 217 表；group-hold {3208=0x10}/{0xa0} 单例**暂不加**（stock 内核
    不处理 group，由 userspace daemon 负责，语义存疑，列 r62 后实验项）。
  - detect() 2.5s 重试窗保留（reg 0x36 现有 stock slave_info 0x6c/8-bit 背书）。
  - 顺手修 r54 期潜伏 bug：s_stream STOP 分支与 sensor_init 的
    `cci_write(..., &ret)` 在 `ret` 未初始化时传入 —— v4l2-cci 读 `*err`
    非零即**静默跳过写入**（乱码栈值）。两处 `ret = 0` 后再传。
- **DTS**：csiphy0_ep `data-lanes=<0 2 3 4>`（=stock 0x4320 同构）、
  ov13850_ep `data-lanes=<1 2 3 4>` + link-frequencies 三值全列；
  reg 0x36 注释升级为 stock slave_info 佐证。BUILD.md r61 行同步。
- **离线验证**：表内抽查 380c/d/e（HTS/VTS）、3012=0x41 全表、
  4601/4603/4837 三族（init族/m0/HFR族各就位）、连续重复写条数与 blob 一致
  （init 1/m1 13/m0 21）。证据已回拉持久库：`camera-ref/ov13850-stdlib/`
  （ex/ = blob+decode2.txt+stock_tables.c+chromatix 库，headers/ = 结构头；
  /tmp 侧 1.6G 系统镜像不回拉，提取物即证据本体）。
- **r61 设备轮（待用户 go-ahead）**：刷入后 (a) i2c 寄存器 dump vs stock 表
  逐条对拍（发给 0x36/7bit，流关闭态）；(b) m1 2112x1568@30 首流出帧
  （link 320M、4 lane、settle 自动），像素内容 vs r55-r59 损毁签名。
- **r62 展望**：若 m1 干净 → HFR/m0 逐档点亮 + VFE P3-P7 poke 复核；
  若仍损毁 → 损毁源锁定数字路径（P3 BUFFER_CFG 优先级不变）。

### r61 设备轮（已执行）：init 对拍达标、首流出帧、损毁根因=WR_BUFFER_CFG 缺写

- **内核 #69 部署**（用户手build+side-load；`uname -v`=#69 SMP PREEMPT
  Oct 19 05:19:10 2026）：probe 日志 `ov13850 3-0036: OV13850 rev 0xb2 detected`。
- **目标 (a) 状态对拍 ≡ 达标**：probe 态 i2c dump（216 条去重）vs stock init 表
  **215/216 全同**，唯一差 `0x0103`（软复位自清位，预期内）。
- **设备工具链**：apk 装 `v4l-utils`+`python3`（tsinghua edge 镜像可达）；
  `media-ctl` 无 Alpine 包 → python3 ioctl 等价物（链接+subdev fmt，
  ABI 值以宿主 linux-libc-dev C 探针锤定：IOC_ENUM_ENTITIES=0xc1007c01、
  SETUP_LINK=0xc0347c03、SUBDEV_S_FMT=0xc0585605；entity_desc 256B，
  dev major/minor 在 off72/76；link_desc 52B flags@40）。
  实体 ID 稀疏（csiphy0=1/csid0=7/ispif0=25/vfe0_rdi0=31/sensor≈96|190，
  随 boot 漂移，python poll 等 bind~0.4s）。/dev/v4l-subdev 由 entity maj/minor
  readlink 解析（v4l-subdev0..11）。
- **DT 运行时复核**：/proc/device-tree 里 rear csiphy0 ep `data-lanes=<0 2 3 4>`
  生效（r61 DTS 活体）；/boot dtb 含 data-lanes 块 ✓。
- **目标 (b) 首流出帧 = 管线级达标**：csiphy0→csid0→ispif0→vfe0_rdi0（3 链接、
  全链 SBGGR10_1X10/2112x1568、/dev/video0 pBAA），
  32 帧 @**30.04fps**、/tmp/L61a.raw=132,464,640B=精确 32×(2112×1568×1.25)
  pBAA 几何；流后 i2c m1 态 dump **227/228** —— 唯一差 `0x350b`
  （AGC 实时增益 0x80→0x10=1.0x，传感器 AEC 硬件活体证据，非偏差）。
- **像素级判定 = 仍损毁（新签名，区别于 r55-59）**：
  - 恰好 **50.08% 字节恒零**（32/64 帧零占比恒定 0.500-0.501/persistent），
    列向均匀、4px 相位均匀；有效数据呈 **「写 1KB / 跳 1KB」2KB 周期交替**，
    逐行相位漂移（边界步进 -592/+432 交替，净 -160B/2行 ≈ -80B/行）；
    row4 ≡ row0 平移 -320B（已验证）。
    非零半区解包后为离散值簇（0/4/5/16/20/64/80/260/261/544…，
    与 r55-59 的 11 值 bit6/8/9 卡 0 签名**不同**）。
  - 证据：`camera-ref/frames/r61/`（L61a.raw+L61b.raw+sha256+dumps+
    r61g_regs.txt 本轮寄存器 dump）。
- **活体寄存器（r61g 中流 /dev/mem mmap 读，r61g_out.txt 已归档）**：
  - CSID0：CORE_CTRL_0=0x00043203（**lane_assign 0x432 直通 ✓ 4-lane 生效**）、
    CID_0_CFG=0x23（DECODE_FMT=2 即 UNCOMP_10BIT|ISPIF|RDI ✓）、TG 灭；
    IRQ_STATUS=0xdd（流活跃中断在跳）。
  - CSIPHY0：5 个 lane 块同值（CFG2=0x10/CFG3=0x0b/settle 相同一套）✓。
  - VFE0：wm0 WR_CFG=0x3（enable+**frame-based**）、UB_CFG=0x155
    （=1023/3 depth+0 offset，与主线 gen1 公式精确一致）、
    PING/PONG=0xff400000/0xff800000、WR_IRQ_SUB=0xffffffff、
    **WR_IMAGE_SIZE=0x00000000、WR_BUFFER_CFG=0x00000000** ←← 损毁直因。
- **根因（stock 内核实锤闭合）**：stock `msm_isp40.c axi_cfg_wm_reg` 的
  **frame-based 分支同样写 WR_BUFFER_CFG** = `wpl(fmt,width)<<16 | (h-1)<<4 |
  burst_len`（8916/8939 burst=3），clear 路径亦有清零；主线
  `vfe_wm_frame_based()` 只置 WR_CFG bit1，**从不写 +0x18** → 写主控
  以 0 几何跑 → 每个 2KB 窗丢一半 burst（1KB 空块交替）。
  - **r60 备忘更正**：0x029461F3 是按 4B 字的错误推导，**从未被 poke**（幸）；
    寄存器 [31:16] 实为 **8B 组/行**，stock 公式 `CAL_WORD(width,5,32)`
    等价 (w×10/8)/8 组 → 2112px = **330** → 正确目标值
    **0x014A61F3**（330<<16 | 1567<<4 | 3）。
  - 顺带发现：主线 `vfe_word_per_line()` 只认 NV/YUYV，RAW 一律返回 0
    （PIX 路径另一坑，本补丁未触碰）。
- **修复（已落盘待用户构建）**：`camss/camss-vfe41-frame-based-geometry.patch`
  （repo 为主本，pmaports 已接线+sha512+pkgrel 68→69）：
  gen1 ops 增**可选** op `wm_set_frame_geometry`，仅 vfe-4-1 实现
  （SBGGR10P 族 → wpl=(w*5+31)/32; reg=wpl<<16|(h-1)<<4|3），
  vfe-gen1.c RDI 分支在 wm_frame_based 后**判空调用**；4-7/4-8 不受影响。
  补丁对 pristine camss 副本 patch -p1 干净通过（内容 diff 复核一致）。
- **事故记录（3 次相关 reboot）**：
  - r61e/f 连挂：根因 = **camss MMIO 在 runtime-suspend（时钟关）时
    裸读 → 总线 fault panic**（post-stop 读取直致，pstore 无 ramoops 可查）。
    **规则（铁律）**：camss MMIO 只允许**中流**读（r61g 中流读全程无害）；
    停流/未 bind 态禁读。
  - r61e/f 还误用了旧版 mediasetup.b64（无 readlink 解析、对 media0 打
    subdev ioctl → ENOTTY + v4l2-ctl 空流 hang）；mediasetup.b64 已重写为
    正确版（wait-loop + readlink 解析）并持久化。
  - busybox **无 devmem applet**；/dev/mem 需 sudo；qmmio 已从内核移除，
    MMIO 通道 = python3 mmap /dev/mem（中流态）。
- **r62 计划兑现**：用户构建 pkgrel=69（build #70）刷入后 16-32 帧复测——
  mid-stream 读回 WR_BUFFER_CFG 期望 **0x014A61F3**（wm0 +0x84）；
  帧期望：1KB 空块消失、真实图像内容（Bayer 噪声+非零连续分布）。
  通过 → 按原计划 HFR/m0 推进（点亮 13MP m0 时对 VFE410 带宽保持观察）。

### r62 设备轮（已执行）：几何寄存器已生效（读回 0x014A61F3 一字不差），但损毁签名不变 → 头号嫌疑转为 UB 深度差 8×

- **内核 #70 部署**（用户手 build+side-load；`uname -v`=#70 SMP PREEMPT
  Oct 19 08:24:36 2026）。
- **r62a 全流程一次通过**（正确版 mediasetup + 32 帧 + 中流 MMIO 读）：
  `/tmp/L62a.raw` = 132,464,640B = 精确 32 帧几何，**30.05fps**，
  sha256 `7887df18…c45c09b`，pull 至 `camera-ref/frames/r62/`
  （含 r62a_regs.txt 全寄存器 dump）。
- **活体寄存器（中流）**：VFE wm0 `WR_BUFFER_CFG(+0x84)=0x014a61f3` ✅
  （=330 组/行 | 1567 | burst 3，与推算完全一致 → v1 补丁逻辑生效）；
  `WR_IMAGE_SIZE(+0x80)=0`（stock 帧基模式同样不写，符合设计）；
  `WR_CFG=0x3`、`WR_IRQ_SUB=0xffffffff`；
  **UB_CFG 仍=0x155**（341 组，v1 未触碰）；PING/PONG=0xffc00000/0xff000000
  （4MB-slot 池，12MB 间隔，正常）。
- **像素级判定 = 损毁签名不变（与 L61a 完全同构）**：
  - 零占比仍 ~50.08%/帧恒定；row-profile 自相关 **7 行周期 0.983** 不变；
    逐行边界序列与 L61a 同步（1024,432,…，净 −80B/行）；
    f0 vs f1 零掩码匹配 0.9983（**按 buffer 确定性重放**，与内容无关）。
  - 结构解码成立：写主控真实行 stride = **2560B**（= 2640−80，逐行相位漂移
    −80B/行 与 (330−320)×8 吻合），即主控未按我们写的 330×8=2640 走——
    **WR_BUFFER_CFG 值（0 或 0x014A61F3）都不改变其行为** → 该寄存器是
    必要条件之一但非充分。
  - 载荷字节谱变脸（L61a: 0x04+0x05 主导；L62a: 0x04 主导 1.65M +
    位交替族 0x55/0x66/0x56/0x65/0x95/0x99/0xaa @10-31K）→ 内容位图
    在跳写路径上确实随景物/曝光变化（活数据），结构骨架恒定。
- **根因修正（头号嫌疑 = r60 清单 P2）**：stock `msm_isp40.c` 实锤——
  `VFE40_UB_SIZE_8939=3072 / _8916=2048`（**16B 单位**）；
  `VFE40_TOTAL_WM_UB_8939=2680`（8916=1656）；单活动流时
  `cfg_axi_ub_equal_default` 把 `min_wm_ub+全部 prop` 全部给该 WM →
  寄存器值 = `0<<16 | (2680-1)` = **0xA77**（与 r60 备忘值精确吻合）；
  wm_base 同主线（0x6C+0x24×i，UB 在 +0x10）。主线 gen1 给 RDI
  `get_ub_size()/3 = 341` → **深度差 7.9×**。r62 证明仅几何位补足不够；
  UB 环过浅 + 高像素率下主控丢 burst 为当前最优解释（实验待证）。
- **修复 v2（已落盘待构建）**：op 更名 `wm_set_frame_based_geometry`
  （gen1.h/gen1.c/4-1.c 同步更名），函数内新增 **UB 全额分配**：
  以顶层 camss compatible 判 SoC（设备 DT 已验证：camss 顶层=
  `/proc/device-tree/soc@0/isp@1b0ac00`，compatible=`qcom,msm8939-camss`），
  8939→写 `2679`、其余（8916）→`1655`（offset 0，仅 RDI 分支调用，
  不触碰 PIX 路径）。patch -p1 对 pristine 干净通过 + b 树逐字节一致。
  pmaports：sha512 `e7fc8844…3e51`、pkgrel 69→**70**。
- **事故记录**：无（全程无 reboot；devssh 引号坑 2 次小抓狂，base64 推送惯例化解）。
- **r63 计划**：用户构建 pkgrel=70（build #71）刷入后**同 r62a 菜单**——
  mid-stream 期望 `UB_CFG=0x00000A77` 且 `WR_BUFFER_CFG=0x014A61F3` 双证；
  帧判定：若零占比 ≪50%、1KB 空块消失 → UB 根因坐实，进入 HFR/m0 阶梯；
  若仍漂移 → stride 之谜续破，备选实验=CSID TG 测试图发生器。

### r63 设备轮（已执行）：UB 全额分配生效（0x00000A77 读回）但签名不变 → 骨架测量实锤「线性 2KB 周期跳写、行结构完全无关」

- **内核 #71 部署**（uname -v = #71 SMP PREEMPT Oct 19 08:54:32 2026）。
- **r63a 一次通过**：复用 r62a 菜单（reuse 脚本尾巴 echo R62A_DONE，文件名
  L62a 拉回后归档为 L63a）；132,464,640B、**30.60fps**、
  sha256 `46926275…a81a11` → `camera-ref/frames/r63/`（含 r63a_regs.txt）。
- **mid-stream 双证**：`UB_CFG(+0x7c)=0x00000A77` ✅（stock 单流全额、
  v2 补丁按 compatible=msm8939 分支正确走 2679）+
  `WR_BUFFER_CFG(+0x84)=0x014a61f3` ✅；`WR_IMAGE_SIZE=0`、
  `WR_IRQ_SUB=0xffffffff` 不变。
- **像素判定：签名纹丝不动**（与 L61a/L62a 同构）：零占比 0.501/帧、
  7 行自相关 0.983、前导边界序列逐项相同。
- **【重要修正 r62 结论】跳过骨架的线坐标周期测量（32 帧聚合零占比，
  骨架=全帧恒零位 = 0.5000 整）**：候选周期扫描 P=1024..8192，
  **P=2048 偏差 = 0.0000**（4096/6144/8192 同因其倍数），其余全部 ~0.49。
  → 写主控沿**线性 DDR 流每写 1KB 跳 1KB**；r62 的「master stride 2560B、
  −80B/行漂移、(330−320)×8 数理」全是 2640≈2048/2053 行混叠造成的假象，
  **没有任何行周期成分**。WM 寄存器全组（几何/UB/anything）对该行为零影响。
- **证据等级重排**：丢弃点在寄存器不可见的实体侧。两大模型族不可由缓冲
  内容区分（写位置/内容在「UB 丢段」vs「上游只送偶数 KB」两种叙事下
  观测等价）→ 判别必须靠**行为实验**（换源/换速率/换 lane 数/TG）。
- **事故**：无 reboot；devssh 引号坑 ×1（grep 管道组合取舍，base64 惯例化解）。

### MMIO 白名单（铁律 v2，r64a 事故后固化）

- **r64a 事故（设备 reboot ×1）**：扩展 dump 越孔径——
  ① ISPIF 基址抄错：读 `0x1b02000` 系（8939 ISPIF 实际在 `0x1b0a000`）；
  ② CSID dump 超长：`csid0` reg 映射仅 **0x100B**（DTS 实测），我读 0x120
  dwords=0x480B 越界。无映射地址 = NoC fault = panic：机理与 r61
  runtime-suspend 读同族（都是裸读器件/总线的不可见态）。
- **白名单（= 8939 DTS reg 表 verbatim，中流只许读这些）**：
  | 器件 | base | len（DTS） |
  | csiphy0 | 0x1b0ac00 | 0x200 |
  | csiphy0_clk_mux | 0x1b00030 | 0x4 |
  | csiphy1 | 0x1b0b000 | 0x200 |
  | csid0 | 0x1b08000 | **0x100** |
  | csid1 | 0x1b08400 | 0x100 |
  | **ispif** | **0x1b0a000** | 0x500 |
  | csi_clk_mux | 0x1b00020 | 0x10 |
  | vfe0 | 0x1b10000 | 0x1000 |
  | csid2 | 0x1b08800 | 0x100 |
  | vfe0_vbif | 0x1b40000 | 0x200 |
- 规则：任何 dump 的 (base, n×4) 必须 ⊆ 表内孔径；表外一律禁读（含
  「应该是同一块」的猜测地址）；写寄存器默认亦限流内且按轮另行评估。

### r64 设备轮（已执行，两段）：ISPIF 首采证（正确基址、无事故）+ VBIF/QOS 织物层实锤复位默认值 → 补丁 v3 = stock 织物三表

- **r64b**（ISPIF 修正基址后首跑，全程干净）：L64b = 132MB、29.08fps、
  sha `9b221dde…f61bcd`；骨架同 P=2048 但**与 r61-r63 相位翻转**
  （此轮 [0,1024) 写满 / [1024,2048) 全零；r64b PING=0xff400000 与 r61 同址，
  相位与 buffer 地址无关 → 跳相为本会话流启动瞬态，跨轮轮转）。
  `frames/r64/`（L64b.raw + r64b/r64c_regs.txt）。
- **ISPIF（0x1b0a000）中流值**：CTRL_0=0（PIX0_LINE_BUF_EN 未置，RDI 无关）、
  IRQ_MASK_0=0x0a492000、IRQ_STATUS_0=0x8000（bit15 活跃）、
  INTF_INPUT_SEL=0、INTF_CMD_0=0xfffffdff、CID_MASK 组=0——无行宽/块宽型
  寄存器，ISPIF 自身无列级可调项 → 非头号嫌疑。
- **r64c（织物层窗户纸捅破）**：
  - VFE **BUS_BDG_QOS 0x2c4-0x2e0**：主线**定义了 0xaaa5aaa5 常量却从不写**；
    stock 8939 DT = 0xAAA5AAA5×7 + 0x2e0=0x0001AAA5（qos-regs 表）。
  - **DS 表 0x988-0x9c8**（stock `ds-settings`=0xCCCC1111×16 + 0x103）：live
    全 0x11111111/0。
  - **VBIF（vfe0_vbif=0x1b40000）**：`0x30`、`0x34` live=0/0，stock 写
    `0x00000fff / 0x00555000`（vbif-regs/settings）。
  - 注：stock `vfe_base+0x30/0x34` 是 TOP IRQ 清除组，与 VBIF+0x30/0x34
    完全两码事（不同基数）。
  - 织物层的 IN_WR_LIM_CONF0/1/2（0xC0-0xC8）live=0x10/0x10100000/0x101000、
    DDR_OUT_MAX_BURST(0xD8)=0x707、ARB_CTL(0xF0)=0x30——stock 8916/8939
    不写这些（除 DT 两表），维持不动。
- **补丁 v3（r64 收尾产物，待构建）**：
  1. `vfe_configure_bus_fabric`（4-1.c）：QOS 八组 + DS 十七组 + VBIF
     0x30/0x34，**全 stock verbatim**，挂在 `vfe_enable_irq_common`
     （幂等，每 power-on 跑）。
  2. `struct vfe_device` 增 `void __iomem *base_vbif`；
     `msm_vfe_subdev_init` 里可选映射 `vfe0_vbif`（byname 失败即 NULL，
     上游 8916 DT 无此 reg 亦安然跳过）。
  3. 补丁现覆盖 5 文件（gen1.h/gen1.c/4-1.c/vfe.h/vfe.c），
     patch -p1 + 五文件 b 树一致验证 ✓；sha512 `f6c94a19…f17d10`、
     pkgrel 70→**71**。
- **r65（下一轮）**：用户构建 pkgrel=71（build #72）→ 同菜单捕 32 帧 +
  mid-stream 加读 QOS 0x2c4-0x2e0 + VBIF 0x30/0x34（白名单内）三证库存值；
  帧判定入口不变（零占比骤降 = 坐实织物根因；不变 = 转 E-lane/E-rate
  行为实验，织物层 parity 即告完成）。

### r65 设备轮（已执行）：🎉 半损坏终局——织物层 stock 对拍后出图，根因链收官

- **内核 #72（pkgrel=71）部署**（uname -v = #72 SMP PREEMPT
  Oct 19 09:46:09 2026）。
- **织物三证读回全部命中 stock 值**（v3 补丁按设计生效）：
  - BUS_BDG_QOS 0x2c4/8×4：`0xAAA5AAA5×7 + 0x0001AAA5`（此前 live 全 0 重置）；
  - DS 表 0x988..：`0xCCCC1111…`（此前 0x11111111）；
  - VBIF 0x30/0x34：`0x00000fff / 0x00555000`（此前 0/0）。
  WM 双证维持（UB=0x00000A77、WB=0x014A61F3）。
- **🎉 帧判定：完好**（L65a 32 帧、30.05fps、sha `6d1daf19…f1ade0`）：
  - 零占比 **0.0021-0.0027/帧**（r61-r63 恒 0.5009）→ 1KB/2KB 跳写消失；
  - P=2048 骨架两半 = 0.0023/0.0023，周期性彻底瓦解；
  - 像素层（MIPI 顺序解包 MSB 对齐 RAW10）：**零像素 = 0.0**，
    min/max/mean = 13/35/21.32 ——暗场景量化带合理（手机镜头朝下）；
  - **拜耳 2×2 网格 = 真传感器结构**：EE/OE = 20.97/22.39、
    EO/OO = 22.39/19.53——G 双通道相等 + R/B 微差，完全是 Bayer 特征
    （若有错位/半帧伪迹不会出现此对称结构）；
  - 帧间差 0.341-0.344（暗噪主导的活流，帧间继续变化）✓；
    全程帧均值 27.9±0.1（AEC 稳定）✓；字节 0-255 全谱 ✓。
- **根因链收官（交付）**：阶段一 = WR_BUFFER_CFG 缺写（主线 gen1 从不写
  帧基几何）+ UB 深度缺（主线 341 vs stock 2680）+ **织物三表缺**
  （QOS/DS/VBIF 全复位默认）——三者全部由
  `camss-vfe41-frame-based-geometry.patch`（v3，pkgrel 71）补齐后
  首次出图。修复现覆盖 5 文件，repo+ pmaports 双接线完成。
- **证据**：`camera-ref/frames/r65/`（L65a.raw + r65a_regs.txt 织物三组
  活体值 + L65a_f0_gray.png 初渲染）。
- **r66（下一轮，预置）**：**实拍验证轮**——请用户举起机器对准明亮
  纹理景物（点屏/灯/室温良好光）捕 16 帧；工作站渲染 PNG + Bayer 均衡
  通道统计 + 期望可见景物。通过 → HFR 模式阶梯（m3-m4）→ 13MP m0
  （观察 VFE410 带宽）→ AF/闪光灯二期。

### r66 设备轮（已执行）：🎉 ov13850 主线内核首张真实影像——管线验证全线闭环

- 用户将镜头对准笔电屏幕后捕获 L66a（16 帧、30fps、sha
  `08510f6f…8b9a4`、16×4,139,520B 精确、零占比 0.002 全绿、活流差 0.31）。
- **工作站渲染 + 人眼判定 = 实景照片**（`frames/r66/L66a_f0_auto.png`，
  工作站/agent 均已入眼看图）：笔电（A 面发光 logo、屏幕面板边框、
  键盘按键阵列、屏幕反光斜条）、浅色桌面、左上深色物体。像素 min/max/
  mean = 12/55/19.96（AEC 压暗目标），1%/99% 百分位拉伸（16→28）后
  内容全显。
- **判读教训（记入方法论）**：byte-mean ≠ 亮度判定；10-bit 数据占量程
  ~2-4% 时一律用百分位拉伸再判——r65 的「暗场景合理」判断正确但
  r66a 一度误读为「无内容」，渲染取证后修正。
- **fb 预览脚本（用户提议，r67a 已验证管线跑通 24 帧 0 掉帧）但屏幕
  不可见**：根因 = 桌面合成器 60fps 整屏重绘覆盖 /dev/fb0 写入
  （存活 <16ms）。可修复方案（备选，未实施）= 渲染窗口期
  `kill -STOP` 合成器 + 渲染 + `kill -CONT`。此路非验证必经。
- **根本交付态**：后摄 ov13850 m1（2112×1568 RAW10@30fps）在主线
  6.12 + v3 补丁下**出真实影像**；修复三件套（帧基几何+UB 全额+
  织物三表 QOS/DS/VBIF）全部由
  `camss-vfe41-frame-based-geometry.patch`（pkgrel 71，5 文件）承担，
  pmaports 接线完成，repo 侧文件亦为最新。阶段 4a（RDI m1）= 完成。
- **后续阶梯**：HFR 模式（2-lane 高帧率档）、13MP m0 全幅（VFE410
  带宽观察）、前摄 ov5693 同法验证；彩色成片 = userspace（libcamera/
 ()<<" Bayer 解插值+AWB）范畴，内核侧不受此限。

### r68 设备轮（已执行）：前摄链两大拦路虎拔除——media 工具链自研打通 + CSID1 上电根因修复（pkgrel 72，待用户重编内核）

- **r68a 首试现象**：链路设置 csiphy1→csid1 / csid1→ispif1 均 OK，
  `ispif1→vfe0_rdi0` enable 报 **-EBUSY**；流挂死 0 字节；ssh 超时
  （诱因之一 = 脚本里 `--list-formats-ext` 在 pBAA 节点上做 1×1-8191
  连续枚举，永不结束——**以后禁用该参数**）。
- **EBUSY 真因（对照实验定位）**：r68a 挂死的 v4l2-ctl 变成
  **D 状态不可杀进程**，持有 video0 的 media pipeline；
  `media_pad_is_streaming(sink)` → 链路 setup 恒 -EBUSY，
  S_FMT/REQBUFS 也 busy。`kill -9` 无效 → **重启恢复**。
  教训：**ssh 超时 ≠ 设备脚本停止**，r68a 尾部的 unbind 在 pkill 后
  继续执行造成图谱churn；禁绑定已绑定 sensor 的轮次设计继续有效。
- **媒体控制器 uapi 破译（本内核实测）**：`media_pad_desc`=20B、
  `media_link_desc`=52B、`media_links_enum`=40B（含尾部
  `reserved[4]`），ENUM_LINKS ioctl = `0xc0287c02`。自研 python
  ioctl 工具（枚举实体/设链/ENUM_LINKS dump/子设备 S_FMT）全部
  打通——设备上 `media-ctl -p` 损坏不可用，今后一律用本工具链
  （`ov13850-stdlib/mediasetup_front.b64` 等已更新）。
- **-EINVAL(-22) 根因（本轮核心发现）**：`video0` open 时
  `v4l2_pipeline_pm_get` 沿启用链扫 s_power，`csid_set_power` 调
  `csid->res->parent_dev_ops->get(camss, csid->id)`，而
  `vfe_parent_dev_ops_get` 的实现是
  `ret=-EINVAL; if (id < vfe_num) ret=vfe_get(&camss->vfe[id]);`
  ——**CSID id 直接当 VFE 索引用**。csid0(id=0)→vfe0 ✓（后摄
  一直没事），**csid1(id=1) ≥ vfe_num=1 → 恒 -EINVAL**（前摄链
  `video0` open 必死，dmesg = `Failed to power up pipeline: -22`）。
  上游 8x16 表虽列 CSID1 但主线从未被使用，故 bug 从未暴露。
  对照实验：同一 boot 下后摄链 enable+open 成功、切前摄链同
  open 即 -22，铁证。
- **修复（pkgrel 72）**：`camss-msm8939-driver.patch` 增设
  `vfe0_parent_dev_ops`（get/put/get_base_address 一律映射到
  `camss->vfe[0]`），`csid_res_8x39` 三条 CSID 全部改指该表；
  对纯净 6.12.1 camss.c `patch --dry-run` 通过。pmaports
  pkgrel 71→72 + sha512 已更新。**需用户重编内核（#73）后
  跑 r68d 验证前摄。**
- **取证与工具遗留**：实体→/dev 节点映射用 chardev devinfo
  readlink 直查（本 boot venus 抢占 video3/4，rdi0 节点仍为
  video0，但**节点号每 boot 会漂移，禁止硬编码**）；r68 期间
  未做任何 MMIO 操作。






### r68d 设备轮（已执行）：-22 修复生效——video0 首次可开；真流无帧、后端总线楔死，以前摄管线为界首次全深度取证

- **内核 #73（pkgrel=72）部署 + 开机既验证**：`video0 open 成功`
  （无 "Failed to power up pipeline: -22"），链路 setup 全 OK、
  9 个 pad fmt 全落位 2592×1944/0x3007。**csid1 上电修复 = 生效**。
- **真流依然无帧**（set-fmt 成功、STREAMON 后 v4l2-ctl 卡 DQBUF、
  /tmp/L68c.raw 0 字节、sensor runtime_status=active= s_stream 已到
  sensor）。趁挂死流做**中流 MMIO 只读**（DTS 白名单内），证据链：
  - `csid1(0x1b08400)`：CORE_CTRL_1(0x08)=0x00020009
    = csiphy_id=1/0x9 ✓；CID_LUT_VC0(0x10)=0x2b ✓；CID0_CFG(0x20)
    =0x23（ISPIF_EN|0x1 + RDI_EN|0x2 + decode=RAW10 + RDI_MODE=
    RAW_DUMP）✓；**IRQ_STATUS(0x68)=0x28000055 非零且递增**
    = MIPI 数据确实到达 CSID1；
  - `csiphy1(0x1b0b000)`：0x04/0x08/0x0c/0x10/0x2c 非零（lane 活动态）；
  - `ispif VFE0 块`：IRQ_MASK_0(0x208)=0x0a492000 ✓；
    **INTF_INPUT_SEL(0x244)=0x10 = RDI0 的 CSID 选择位=CSID1 ✓**；
    INTF_CMD_0(0x248)=0xfffffdff = bits9:8=01
    = **CMD_ENABLE_FRAME_BOUNDARY（RDI0 等帧界已开启）**；
    RDI_INTF_0_CID_MASK(0x264)=0x1（VC0）✓；RDI_INTF_0_STATUS(0x2d0)
    =0xe（bit0≠1 = 接口活动）✓；RDI1/RDI2/PIX 均空闲 0xf；
  - `vfe wm0`：WR_CFG(0x6c)=0x3（RDI path+FRM_BASED）✓；
    **WR_BUFFER_CFG(0x84)=0x01957973 —— bits31:16=0x195=405=2592 宽
    RAW10 wpl ✓（与后摄 0x014A=330 同构）→ v3 帧基几何对 2592 泛化
    验证通过**；WR_IRQ_SUBSAMPLE(0x8c)=0xffffffff ✓；
    **但 WR_PING/PONG(0x70/0x74)=0xff800000/0xff000000（跨 2s 完全
    冻结）+ IRQ_STATUS_0(0x38)=0（无 ping/pong 切换）**
    **IRQ_STATUS_1(0x3c)=0（无 BUS_OVERFLOW）→ VFE 总线 0 写;
  - **后续再读 VFE 寄存器页 = SIGBUS（总线楔死）——教训：真流挂死
    稳定后 VFE 寄存器访问即自毁，MMIO 快窗取证只读
    csid/csiphy/ispif/clk_mux 安全页。**
- **r68d 中流实证结论**：数据活过 sensor→csiphy1→csid1→ispif(RDI0
  接口已选 csid1、CMD=等帧界、接口活动)，死在 **CSID 帧界判定→
  VFE WM 触发**之间；疑问收敛至(1)真流数据本身未构成有效帧界
  （DT/LUT/FS-FE 层），(2)共享 RDI0 接口 mux 的某些位残留。

### r68e/r68e2 设备轮（已执行）：CSID1 片内测试图判定轮——🎉 前摄链路全绿，根因缩至传感器/PHY 侧

- **r68e**：开 TG 控件报 `Resource busy`——读码确认
  `csid_set_test_pattern`: **csiphy→csid 链接使能时禁止开 TG**
  （sink pad 有启用链接即 -EBUSY）。
- **r68e2（fresh boot）**：链路搭建后**先关闭 csiphy1→csid1**、
  再置 `test_pattern=1`（动态解析 csid1 节点，避免节点号漂移）：
  **12 帧全捕，75,582,720B = 12×6,298,560（2592×1944 RAW10 精确）**
  ——TG 递增图案逐行锯齿、10-bit 全域均匀（mean 511, zero%≈0.4%）
  → **解包器校验一致**（`frames/r68/L68e_f0.raw +
  L68a_tg_f0_crop.png`，裁窗渲染纹理锯齿清晰）。
- **判定意义**：TG 数据走同一通路（csid1→ispif RDI0 接口→VFE wm0
  →内存）+ 同套 2592 帧基几何 → **前摄管线/几何/打包全线正确**。
  r68d 的失效只可能来自 **csiphy1/ov5693 实流数据源**。
- **r68d 的真相修正**：TG 流使 VFE 正常写、总线不死 → r68d 的
  SIGBUS 总线楔死 = **真流数据路径特有**（对齐(1)号假设：
  真流无有效帧界长流打底 → VFE 等待死锁 → 总线失能）。

### r68f–r68g2 设备轮（已执行）：取证轮三连翻车 → 工具链硬伤逐一暴露并修复

- **必须先躲避的三类雷**（均实证）：
  1. **挂死流进程连 timeout 的 SIGTERM 都不听**——TERM+KILL 双杀后
     若仍活，唯一出路 = 立刻 reboot（r68a 已知）；
  2. **bg 的 v4l2-ctl 未加 `</dev/null`**：继承 ssh 通道 stdin →
     DQBUF 挂死后 ssh 永不 EOF → devssh 空转超时（"看起来像设备死亡"）；
  3. **devssh.py 旧实现：设备重启/断网后 ssh 不主动退出**（无
     keepalive，连接半开直挂到 TCP 超时），且**只在连接关闭时
     一次性打印** → 介质丢失＝全部输出蒸发——用户指出后重构：
     `ServerAliveInterval=3/CountMax=2/TCPKeepAlive/ConnectTimeout=10`
     （设备死亡 ≤9s 内 EOF）+ **流式逐块打印实时 flush** + 显式
     `DEVSSH_TIMEOUT_DEADLINE/DEVSSH_EOF` 标记 + finally 强关。
- **ash `exec >> 文件` 静默自杀（本轮最大工具发现）**：设备
  busybox ash 中 `exec >> /home/user/X.log 2>&1` 后**后续全部
  不执行、无任何报错输出**（`set -x` 追踪到 `+ exec` 即死；
  5s 微测隔离复现：`echo BEFORE_EXEC` 后即停）。**设备脚本
  禁用 exec 重定向**，改用调用点重定向
  `sudo -S ash script.sh >> /tmp/R.log 2>&1`（微测验证等效可行）。
- **取证转储通道定为：/tmp（实测跨重启存活）+ /home/user（ext4 rw，
  touch/python 写均验证）双写**；轮末 sync+cp。
- **r68g2a/r68g3 仍未产出日志**（exec 排除后仍静默早死，疑点更多，
  触发条件待定：疑似与脚本长度/后台子进程/TG 消息纠缠）——
  下轮改用 `set -x` 全程追迹 + /tmp/rA.log + 轮末端 cp，一次
  分段验证定位。

### r68g 关键旁证（已执行，半途而废但抓到铁证）

- **`/tmp/L68g_tg.raw` = 0 字节（22:18）**：**该次 fresh boot 上
  TG 捕获本身也挂死**（r68e2 明明 12 帧全通！），此后 P2/P3 全被
  video0 僵尸占满（EBUSY），结尾未到自重启分支 → **设备 ~3 分钟后
  自行 panic 重启**。两次"设备自行重启"均 = **真流/挂死流留下的
  楔死总线 + 僵尸进程 → 内核 panic**（另一次 = r68f 脚本内
  STUCK_REBOOT 保护性重启，设计内）。
- **开放疑问**：TG 在"干净 fresh boot"上为什么偶发挂死（r68e2 通过
  而 r68g/r68f-v1 通过前次 boot 的空镜像仍成功一次）——
  **下轮优先解决**：TG 首流 + 后 fps/时长参数重测；同时考虑
  `--stream-skip=0/2`、count、boot 后绝对时延等变量的系统对照。

### r68 系列当前收敛结论 + 下轮预案（r68h）

- **阶段判定**（按用户阶梯"前摄验证→彩色→13MP→HFR"）：
  1. ✅ 前摄管线修正（csid1 上电，pkgrel 72）：
  2. ✅ 前摄 image 链路+几何（r68e2 TG 12 帧判定）：
  3. ⛔ 真流数据路径（csiphy1/ov5693 侧）：
  **唯一活跃矛盾点** = 真流帧界缺失。候选 =
  (a) sensor 输出线率/DT/FS-FE 与 CSID LUT 不匹配；
  (b) 含 7-9 csid 错路（csiphy1→csid0 旧假设炮灰）残余；
  (c) T 这条路脱解后交错/裸帧不带有效 VC。
- **r68h 预案**（全部带 `</dev/null`、/tmp 日志、真流快窗 MMIO、
  结尾保护性 reboot；MMIO 只读安全页）：
  - A) TG 基准 + REAR 参考真流（同 boot）：取"真实数据良好帧"的
    csid0/ispif 取证基准（r66 配置），记录 IRQ 位模式 = 模板；
  - B) 前摄真流快窗：STREAMON 后 0.8s/3.3s 两次 csid1/csiphy1/
    ispif 快照 + CLKMUX(0x1b00020) 验证 select_clk_mux 写入；
    对照 A 的位模式找差位；
  - C) 传感器侧证据：ov5693 中流 readback（I2C 刷流配置帧）+
    csiphy1 GRP 寄存器 + TN 内部帧率（via IRQ dt 报文中）对照。
  - success（E）判定：csid1 IRQ 状态差位 = MISMATCH 类型指纹
    → 指向 root cause（帧界 vs DT 映射 vs mux）。

### r68g2→r68h 阶段（用户超出套件外的取证迭代,核心教训已固化）

- **开机后当轮 r68h v3 三杀已定位**：`/dev/mem` mmap 读**流中任何窗**
  ⇒ CPU D-state 总线悬挂（**无 oops/panic/pstore dump**）→ 外部 watchdog
  ~1-3min 硬复位。v3a 全窗/v3b csid0x100/v3c csid0x70 全死在
  `p1_mmio_dump`（rear 流中），否定"窗深"变量 ⇒ **流中一律禁止 MMIO**。
  历史复核确认：mid-stream MMIO **从未在任何一轮成功过**（r68e2=纯
  capture，r67a=buffer 分析器，r68g/r68f 输出为空）。
- **MMIO 验证深度（mmap 路径）≠ DTS 窗口宽度**：验证过的安全集 =
  csiphy0/1 ≤0x40、csid0/1 ≤0x70、ispif [0x1b0a200,0x340)、
  clkmux [0x1b00020,0x34)。qmmio devkit(r15) 的更宽读数**不能外推**到
  mmap 路径。MMIO 的可行时机见下方 H5 假说（v4 实证流后读亦死，早期
  "只允许 idle/流后"结论已作废）。
  **mid-stream 状态取证改走 /proc/interrupts 计数采样（procfs 安全）+
  0.8s 处一次 /dev/i2c-3 SENSE dump（CCI 独立总线）**。
- **pstore 死亡签名**：楔死不产 panic → dmesg-ramoops 只会留当前 boot
  早期 oops（pm8916_bms_vm 那类）；"干净无 panic 的死亡"本身即
  D-state 悬挂指纹（watchdog 硬复位的旁证）。
- **开机启动形状（launch shape）判定**：
  - ❌ `ssh "echo pw | sudo -S sh X >> log 2>&1"`（append 形状，2 次复现
    于 2-4s 内接端挂死+死亡）——r68g2 记录的先前"微测验证等效"结论
    **被推翻**，禁用；
  - ✅ 通道 stdout 形状 `ssh "... sudo -S sh X 2>&1" > HOSTFILE`（G2 形状，
    实证脚本可全程跑完）；日志分析走事后文件拉取。
  - 设备脚本内禁 `exec >>`、禁 pkill（wedge 检测直接 abort）。
- **qmmio（debugfs 读数器）已除役，不得复活**：用户确证其在使用中
  **频繁直接引发内核恐慌**，已从内核移除。r15 时代的成功读数实为
  高险路径（读窗曾远至 VFE 0x1b10000 / VBIF 0x1b40000 / ISPIF 全
  aperture）。#73 上唯一读通道 = `/dev/mem` mmap。
- **深度教训：每个 MMIO 读的死亡矩阵（mmap /dev/mem）**：
  1. 流中读 = 死（v3a/b/c，全在 `p1_mmio_dump`）；
  2. 流后读 = 死（v4 `p1_post_mmio`，rear 捕获完成后立即读）；
  3. boot 内首个流之前的 idle 读 = rear ✓ / tg ✓ / **front ✗**（v5i
     `front_idle` 死）。
  死亡指纹一致：无 oops/panic/pstore → D-state 总线悬挂 → watchdog
  ~1-3min 硬复位。
- **统一假说 H5（待 v5w 验证）**：读死亡发生在"该设备经历过
  runtime-resume→autosuspend 周期（GDSC 已塌缩）"的窗口；boot 后从未
  被 resume 的设备保持带电、可读。v5i 的 front-idle 死亡 = CSID1 曾被
  pattern-on/off 走过一轮 resume→suspend；rear/tg-idle 通过 = 设备尚未
  走过该周期。**v5w 验证协议 = 开机把全部 camss 平台设备
  `power/control=on`（禁 autosuspend）后再读**；同时记录每个
  runtime_status 变化。
- **IDLE 寄存器快照几乎全是零**：CSID/ISPIF/CSIPHY 配置寄存器由驱动在
  streamon 时写入；idle 期读回仅 CLKMUX（0x1b00020=0x0f, 0x1b0002c=
  0x3f）非零。⇒ idle dump 证据价值有限，但 front-idle 死亡本身=
  重要事实。
- **双写纪律再升级：cp 必须 **紧跟 sync**（每产物）。v5i 全部 /tmp+
  /home 拷贝因楔死吃掉 ~30s 写回窗口而变成 **0 字节 inode**（ext4
  data=ordered 懒写回被没收）；v4 的 irq_rear.txt 能幸存纯属 sha256
  读路过促 flush 的运气。规程：`cp … && sync` 或写后立刻 sync。
- **pm8916_bms_vm oops 与读数无因果（新论证）**：地址空间（SPMI/PMIC
  电源管理数据结构）与相机读窗无交集；时间戳 = boot 内 t≈10.3s
  探测器阶段（9.99s ov5693 依赖环、10.04s Goodix、10.3s bms 线程），
  距读数时刻（uptime 700-800s）差数分钟。今天多次楔死无 panic 不写
  pstore → ramoops 环中旧 oops 持久滞留 → "每次相机轮后看到它"的
  错觉。A/B 法：v5w 前后各存 pstore 基线对比时间戳是否翻新。
- **stock ov5693 库解码完成**（`o5693-stdlib/stock_tables.c`）：
  唯一硬件 res_settings（257 项，2592×1944，HTS=0x0a70，VTS=0x7c0），
  格式 {u16 reg,u8 val,u8 dl} u32-LE 步长 8；m0 位于文件内
  @0x2954/257 + @0x2778/11 + @0x3394/10；组寄存器 {3208=0x10/0xa0}；
  AEC 覆盖集 {3808,380a,380c,380e}。stock 与上游差：
  stock 写 PLL 0x3001/0a,0x3002/80,0x3006/00,0x3011-0x3015（上游不写）；
  0x3098-0x309b = stock 03/1e/01/02 vs 上游 02/19/02/01（stock 24MHz
  xvclk vs 我方 19.2MHz，属预期差）；上游 pclk=167.68MHz 固定
  HTS=2688 VTS=2078 crop16/6，stock ~159MHz/2672/1984 crop0/0。
  离线对拍器 `/tmp/opencode/cmp_sense.py`（live vs 上游预期 vs stock）就绪。

### r69–r72 设备轮（已执行与修正）：解码 bug 全链修复 + 真白墙验证（用户三次纠错主导）

- **⚠ r69"彩色里程碑"作废**：用户指出图像内容与物理场景（白墙/充电器/路由器，
  遮挡=全黑）不符，追问后实锤**三条管线 bug + 一个设置缺位**：
  1. **位序反了**：pBAA 实际 = MIPI RAW10 打包（bytes0–3 = 像素高 8 位，
     byte4 = 低 2 位 LSB 在前），我按反序解 → 平滑信号压到 1/4 底 + 抖动位
     放大成满幅盐噪/伪彩环（"彩虹环"= 纯解码伪影）。证据：遮挡帧
     bytes0–3 median=4（98% 恒值）、byte4 median=69——反序解出 median 260
     的假底座。**金标准对拍**：正确位序解 r66 帧 = 与当日成功黑白参照
     （笔记本+桌面）结构完全一致 ✓。
  2. **欠曝**：手动通路无 AEC，传感器跑 exposure 默认 1536 行 +
     增益 0x10(1x)，室内白墙只比黑台阶高 2–4 counts（r66 能出图全靠
     画面里点亮的笔记本屏）。ov13850 暴露完整 ctrl：exposure
     (4–3324 行, max=vts−4) + analogue_gain(0x10–0xf8, 实益=reg/16)；
     BL 台座为增益后级（遮挡下 gain×8 不放大 median 只放大噪尾 ✓ 实测）。
     **可用组合 = exp 3320 + gain 0xf8**(v4l2-ctl --set-ctrl 流前设置，
     s_stream 的 handler_setup 会刷入硬件)。
  3. **demosaic fill 两次踩坑**：v3 cascade fill 跨通道污染 + 留洞；
     v4 通用 diag4 对 G 位同样留洞（R 真实采样点(odd,odd)相对 G1 位
     是**轴向**邻居而非对角）→ 渲染图 50% R/B=0 → 全图假绿偏。
     正解 = **逐相位核**：G1 位 R 竖直对/B 水平对，G2 反向，
     B 位取 R 四对角（反之亦然），G 用四轴向。墙帧渲染均值
     R248/G249/B246 中性 ✓。
- **设备轮证据链**（raw 留档 camera-ref/frames/）：
  - r70 遮挡帧两次（blocked/blocked2 均 16.6–16.8 四通道全等全平
    = 真黑参考 ✓ 与 LineageOS 遮挡黑一致；第一次实为对暗处未遮实）
  - r71 曝光梯子 A(默认1536/1x)/B(1000行/1x)/C(+gain8x)：list-ctrls
    回读确认写入、增益后级 BL 行为、暗电流随积分微降
  - r72 白墙帧（exp3320+gain248, sha b9f69b8c…）：median 207、p99 472、
    R/G=0.52 B/G=0.74 且空间均匀（全局通道响应差而非镜内色渐晕）；
    减 BL(16.8)+WB+正确 fill 后 = **中性白墙，色调/内容/几何与物理
    场景一致**（右上角贴纸物件、右缘线缆、墙面痕迹）。
- **工具沉淀**：`camera-ref/tools/decode_ov13850.py`
  （正式版解码：MIPI 位序 unpack + 逐相位 bilinear + 暗参考 BL +
  中值 WB + 共享窗 gamma tonemap；三个金标准验证注解在文件头）。
- **下一步候选**：a) 彩色物体试拍（验证全彩与 CCM 需求）；
  b) 墙帧做逐通道 lens shading LUT；c) 13MP 全幅(4224×3136)替代模式
  曝光上限更高；d) HFR 模式（exp_def 666 行系）；前摄诊断保持冻结
  （v5f 方案留档 tools/r68h_f_skipped.sh）。

## 5. 风险

- 从地址/ lane 顺序是解码 + 惯例值, 必须 `i2cdetect` + 抓帧复核.
- 前摄 MCLK 用 19.2M (偏离 stock 24M): sensor 支持 6-27MHz,
  上游 PLL 表按 19.2M, 是正确选择; 若抓帧时序异常再试 24M
  并按比例修正 link-freq.
- 全幅 13MP 分档主线 VFE410 带宽与 camss 驱动是否吃得下,
  首版不验证, 先稳定 2112x1568.
- AF (DW9761) / 闪光灯 (LM3646) / EEPROM 全部二期, 不 blocking.

## 6. 硬件 PIX 路径 (VFE ISP) — 调查与计划 (r76 规划)

### 6.1 动机

- 主线路线现在只有 RDI (raw 直写), 需要 Wan 端做完整 ISP 才是"能用相机";
  VFE 4.1 有完整 ISP (demosaic/WB/gamma/scale), 驱动侧 `line_num=3` 只供
  RDI, PIX 线 (line 3) 没实例化.
- 硬件出 NV12 后, ffmpeg/任何 V4L2 消费者原生可用, 这是"能用相机"的正解;
  软件解码管线 (decode_ov13850.py) 反正保留给金标准对照.

### 6.2 调查结论 (全部对拍过源码/设备)

- **实体命名规则** (`camss-vfe.c msm_vfe_register_entities`): VFE line i
  的 subdev 叫 `msm_vfe%d_rdi%d` (i<PIX) 或 `msm_vfe%d_pix` (i==PIX),
  视频节点一律 `msm_vfe%d_video%d`. 设备实测实体:
  csiphy0/1, csid0/1/2, ispif0/1, `msm_vfe0_rdi0`, `msm_vfe0_video0`
  → 我们的"video0"就是 line0 RDI 的视频节点, **没有 pix 是因为资源表**.
- **卡点 = 资源表**: `camss-msm8939-driver.patch` 里 msm8939_resources
  直接复用 `vfe_res_8x16` (V4L2 `line_num=3`), PIX 线根本没创建.
- **主线 gen1 PIX 代码是齐的**: `camss-vfe-4-1.c` 有
  - `VFE_0_CAMIF_*` 完整 CAMIF 序列 (0x2f4 cmd / 0x2f8 cfg / 0x300 frame
    / 0x304 windows / 0x30c subsample / 0x31c status)
  - `VFE_0_MODULE_CFG_DEMUX/CHROMA_UPSAMPLE` 模块使能位
  - `VFE_0_DEMUX_CFG/GAIN/EVEN_CFG/ODD_CFG` demux 编程
  - `vfe_enable_irq_pix_line`, NV12/NV21 plane 尺寸计算
    (`vfe_get_wm_sizes` NV12/NV21 分支)
- ** formats_pix_8x16 = NV12/NV21/NV16/NV61** (camss-vfe.c 172 行起).
- **媒体图接线全自动**: `camss_link_entities()` 按 `line_num` 循环生成
  csiphy→csid→ispif→vfe(idx 0..3) 全链, 一改 line_num=4,
  `msm_vfe0_pix` subdev + `msm_vfe0_video3` 节点 + ispif→vfe pix pad
  链接全部自动出现. DTS 不用动 (csi0_pix clock 我们已预留).

### 6.3 三个真缺口 (主线代码 vs 硬件需求)

1. **CAMIF 无 bayer 分支** (`vfe_set_camif_cfg`): switch 只认
   YCbYCr/YCrYCb/CbYCrY/CrYCbY 四种 YUV pixel pattern
   (0x4..0x7 写 `VFE_0_CORE_CFG` 0x01c).
   - **下游参照**: `msm_isp40.c msm_vfe40_cfg_camif` 写同代偏移:
     0x1C=pattern|input_mux<<16, 0x300/0x304/0x308=frame/window,
     0x2F4/0x2F8=cmd/cfg, 0x30C=subsample. 逐位对得上主线偏移.
   - **bayer pattern 值**: 下游 `include/media/msmb_isp.h`
     `ISP_START_PIXEL_PATTERN`: RGRGRG=0, GRGRGR=1, BGBGBG=2, GBGBGB=3,
     YUV_YCbYCr=4 — 与主线 YUV 宏 0x4-0x7 次序完全一致 →
     **BG (底层列) = 0xBGBGBG = 值 2** (我们传感器 CFA:
     B(e,e) G(e,o) G(o,e) R(o,o) → 行0=BGBG 行1=GRGR → BGGR列型 = 2).
2. **DEMUX 无 bayer branch 并缺值**: `vfe_set_demux_cfg` 只写 YUV 四种
   even/odd pattern (0x9CAC 等). bayer 的 DEMUX_EVEN_CFG/ODD_CFG 值
   (0x9C9A 族?) **主线/下游内核都没写**.
   - 下游 msm_isp40.c 全文不碰 demux 寄存器 = 下游架构里 ISP 模块参数
     (demux pattern/增益) 是**用户态 HAL 经 MSM_VFE_REG_CFG ioctl**
     写进去的 (chromatix), 内核只是执行器 → 参考值不在内核源里.
   - 计划: bayer even/odd cfg 值按 "nibble 选择通道" 语义试错枚举
   (0x9C9A/0xC9CA/0xAC9C/0xCA9C 4 个候选 × 校验顺序, 一次重启=1 试).
3. **ISP 模块使能**: ~~主线没人写 0x018 MODULE_CFG~~ **实现时更正**:
   `vfe_set_module_cfg` (4-1.c) 存在且写全部四模块位
   (DEMUX/CHROMA_UPSAMPLE/SCALE_ENC/CROP_ENC), gen1 pix enable 流
   (camss-vfe-gen1.c, `vfe_enable_output` 的 PIX 分支) 已完整接管
   module/camif/realign/xbar/demux/scale/crop/clamp/camif_cmd 全序 —
   所以 patch 只需补 bayer 分支, 模块使能无需动.

### 6.4 patch 方案 (`camss-vfe41-pix-bayer.patch`, 增量 patch)

1. camss.c: 新 `vfe_res_8x39` 表 (拷贝 8x16, `line_num = 4`),
   msm8939_resources 改指该表. (csiphy/csid/ispif 维持 8939 现有表.)
2. camss-vfe-4-1.c:
   - 宏补 bayer: `VFE_0_CORE_CFG_PIXEL_PATTERN_BGBGBG 0x2` 等 4 档;
   - `vfe_set_camif_cfg` 增加 SBGGR10_1X10 → 0x2 (BGGBG) 分支
     (sink mbus code 判据);
   - `vfe_set_demux_cfg` 增加 bayer 分支: 写试错字节 ME:
     gain 0x80×4 通道 (与软管线一致基准), even/odd cfg 从候选集;
     (demux 是 bayer→RGB 的唯一在片转换, 增益 = 后续 WB 校准点)
3. 不动 ispif.c: 主线 ispif 已有 pix overflow 处理; 若 streamon 后
   卡死, 再对比下游 `msm_ispif` pix intf 编程补一段.
4. 可选: demux 增益暴露 module param (对齐我们 r72 标定 1.92/1.34),
   硬件色彩起点与软管线同基线.

### 6.5 验证路线

1. 编内核 (pkgrel +1) + sideload + 重启 → `ls /sys/class/video4linux/`
   应多出 `msm_vfe0_video3`; `media-ctl -p` 应有 msm_vfe0_pix 实体.
   **r76 现状: 补丁已落盘** (见 6.7), 待用户 build #73 + sideload.

### 6.6 pbwrap 内核模块 (已放弃, 留档)

- 尝试过完整 v4l2loopback 式内核包装 (pbwrap.c, ~1000 行, BTF 齐活,
  vermagic 对齐), insmod 三连修: ① struct module 尺寸 = BTF 配置门控
  (DEBUG_INFO_BTF[_MODULES] 影响布局, 必须与运行内核一致);
  ② libelf-dev 主机补装后 tools 构建 ok; ③ 主机 pahole 生成的模块 BTF
  与设备内核 BTF (chroot pahole 版本不同) 不一致 → 剥 .BTF/.BTF.base;
  ④ 最终失败于 CFI: pmOS 内核 CONFIG_CFI 锁间接调用, gcc 编的外置
  模块不进 CFI 表 → `do_one_initcall CFI failure` 只能放弃.
- 文件: camera/pbwrap/{pbwrap.c,lut.h,Makefile} (留档不进构建);
  lut.h = 1024 表 gamma 1/2.2, 与 decode_ov13850.py 同源.
- 软解码管线 (decode_ov13850.py) 继续当金标准/对照工具.

### 6.7 r76 补丁 (已落盘, pmaports)

两份补丁已进 `pmaports/.../linux-postmarketos-qcom-msm8916/`,
pkgrel 72→73 (内核 #73), sha512 已更新, 交叉编译全链通过
(ksrc `make M=drivers/media/platform/qcom/camss modules`, LD+BTF 无错).

1. `camss-msm8939-driver.patch` (重写): msm8939_resources 的
   `.vfe_res` 从 vfe_res_8x16 (line_num=3) 换为 **新增
   vfe_res_8x39 (line_num=4)**; 其余 csiphy/csid/vfe0_parent_dev_ops
   结构不动.
2. `camss-vfe41-pix-bayer.patch` (新): gen1 VFE 的 bayer 三缺口——
   - `CORE_CFG_PIXEL_PATTERN_BAYER_*` 宏 0x0-0x3 (下游
     `msmb_isp.h ISP_START_PIXEL_PATTERN` 次序, BGGR=BGBGBG=0x2)
     + `vfe_set_camif_cfg` 全 bayer 码表分支 (8/10/12bit 四相位);
   - `vfe_set_demux_cfg` bayer 分支 + **模块参数**: demux_even/
     demux_odd (选择子, 默认 0x9cac 首猜) 与 demux_gain0/1
     (增益字 0x8080 默认 = 原值), 运行期调 `/sys/module/qcom_camss/
     parameters/*` 可免重编试错;
   - `formats_pix_8x16` 增补 4 个 SBGGR10_1X10 族 sink 条目
     (NV12 视频节点映射; pix 线 sink 码表原本只有 YUV, rdi 表不受影响).
- 设备脚本 `/tmp/opencode/r76_pix.sh`: media 图 = csiphy0→csid0→
  ispif0→**msm_vfe0_pix** (rdi0 链接显式关闭以保 ispif 首链接解析
  = PIX0), pad 链 fmt=SBGGR10 2112x1568, video3 NV12 抓 3 帧,
  实体缺失时报 NEW_KERNEL_REQUIRED 不重启, 卡死保护 reboot 照旧.
- 首帧预期 = 可能色块/相位错误 (selector 未知), 用帧统计+已知白墙
  反推 selector 映射, 然后重跑参数而不重编.


### 6.8 r76a/b (内核 #74): PIX 首亮成功 + 亮度链全通

用户 hand-build #73 内容后 `uname -v` 报 #74 (pkgrel 73),实体全数就位:
`msm_vfe0_pix` (ids 58)、`msm_vfe0_video3` (major 81 minor 3)、rdi0/1/2、
video0-2、ov5693+ov13850 全 bind。**PIX 通路 buffer/IRQ 层全通,零卡死零 dmesg 报错。**

- **r76a 首亮** (/tmp/opencode/r76_pix.sh): media 图 = csiphy0→csid0→
  ispif0→msm_vfe0_pix, video3 抓 NV12 3 帧 ×4967424 = 2112×1568×1.5
  **精确几何** (Y 1568 行 ×2112, chroma 784 行 ×2112 UB).
- **r76b 白墙** (+exposure 3320 行 / gain 248 版 r76_pix_b.sh):
  - Y med=46 max=105,列向梯度 col0 med=29 → lastcol med=7 = 透镜 shading
    真实结构;行列无相位条纹。
  - **亮度尺度与 r72 金标准吻合**: raw10 白墙 med 207/p99 472 ÷4 = 52/118
    ≈ 实测 46/105 → **10→8 位截断路径正确** (Y = v>>2)。
- **真异常**: 白墙 U=V=41 (应 ≈128) → 色度平面全错;后续确认 U/V = 完全相同
  (c1 彩条证实) → **chroma 双平面同源,U/V 写同一路数据,色度路由是头号缺口**。

### 6.9 demux gain 寄存器字段破案 (r76b 后查源码 + r76c 作废记录)

`camss-vfe-4-1.h` 162-167 行,已核实:

```
VFE_0_DEMUX_GAIN_0        0x428:  CH0_EVEN = 0x80<<0   CH0_ODD = 0x80<<16
VFE_0_DEMUX_GAIN_1        0x42c:  CH1      = 0x80<<0   CH2     = 0x80<<16
```

0x80 = gain 1.0。**上轮 255 等单字节值写入会把其他字段清零** —
r76c c2/c4 捕获因此作废 (c3=c1 重复, c1/c2 有效),**正确写法 = 十进制
8388736 (0x00800080 = 两字段各 0x80)**,必须同时写满 32 位双字段。

demux EVEN/ODD cfg (selector) 主线 YUV 常量 0x9CAC/0xC9CA/0xAC9C/0xCA9C
= 同一组 [9,c,a,c] nibble 的 4 相位旋转。TP 上换 selector 无感 (各相位
同色, TP 本身对 selector 不敏感,不构成反证);现值 0x9cac。

### 6.10 r76d: RDI 彩条 ground truth 采集成功 (+ ispif 换链 EBUSY 教训)

- **首跑翻车**: r76d 的链表没先禁 pix,`ispif0→rdi0` enable 遇 EBUSY
  (主线 `ispif_link_setup` 第一条: 同一 source pad 已有 enabled link 时
  拒绝再次 enable,RDI 从未被真正启用 → v4l2-ctl 首流挂死 →
  TAG_HUNG_PROTECTIVE_REBOOT 保护性重启)。
- **教训**: pix/rdi 同一 ispif source pad,**换链必须先禁后启,顺序固定**。
  这与 r76a 里"先禁 rdi0 再启 pix"的既有写法互为镜像,当初未对称推广。
- **mg76me.py 已统一参数化** (python3 /tmp/mg76me.py rdi|pix): pix =
  先禁 rdi0 再建 csiphy0→csid0→ispif0→pix; rdi = 先禁 pix 再链到 rdi0。
  mg*.py 的 link() 已有 try/except EBUSY 容错; r76e 因此也需要同一套。
- 重启后复跑成功: `/home/user/76d/L76d_rdi.raw` 3 帧 pBAA raw10
  (2112×1568, 4139520 B/帧, --stream-skip=2),设备端 sha256 存档。

**OV13850 test_pattern=1 (Vertical Color Bar Type 1) 真值** — 用
pBAA 解包 (byte0-3 = 每 px 高 8 位, byte4 = 低 2 位 frag byte; SBGGR10:
B(e,e) G(e,o) G(o,e) R(o,o)) 从 RDI 帧解出,中心带 rows 600-900 px
(= 相阵列 rows 280-390),每 bar 取中部 232px 莫氏窗口:

| bar | 颜色 | B   | G1  | G2  | R   |
|-----|------|-----|-----|-----|-----|
| 0   | 白   | 1023| 1023| 1023| 1023|
| 1   | 黄   | 0   | 1023| 1023| 1023|
| 2   | 青   | 1023| 1023| 1023| 0   |
| 3   | 绿   | 0   | 1023| 1023| 0   |
| 4   | 紫   | 1023| 0   | 0   | 1023|
| 5   | 红   | 0   | 0   | 0   | 1023|
| 6   | 蓝   | 1023| 0   | 0   | 0   |
| 7   | 黑   | 0   | 0   | 0   | 0   |

= 经典 8 色条, G1≡G2。分析工具 `/tmp/opencode/bars_truth.py`
(注意: 它的 bar 切片曾按全尺寸列算,相位阵列是半宽/半高,当版
tab 有 bug — 上面这张表来自修正过的相位阵列切片重算)。

### 6.11 r76e: 通道判定扫 (e1-e5) + e6 白墙基线 — 结果已入 /tmp/opencode

pmaports 脚本 `/tmp/opencode/r76e.sh`,5 组 TP 彩条捕 + 白墙对照,
产物 `/home/user/76e/L76e_e{1..6}.raw` 已全部拉回主机。gain 全部按
6.9 正确 32 位写法; selector 维持 0x9cac。

| 组 | demux_gain0 (0x428) | demux_gain1 (0x42c) | 含义 |
|----|---------------------|---------------------|------|
| e1 | 8388736 (双 0x80) | 8388736 | 全通道增益 1.0 基线 |
| e2 | 8388608 (0x00800000) | 8388736 | 仅 CH0_ODD=g1.0, CH0_EVEN=0 |
| e3 | 128 (0x00000080) | 8388736 | 仅 CH0_EVEN=1.0, CH0_ODD=0 |
| e4 | 8388736 | 128 (0x00000080) | CH1=1.0, CH2=0 |
| e5 | 8388736 | 8388608 | CH1=0, CH2=1.0 |
| e6 | 8388736 | 8388736 | 白墙 (tp=0) 全 1.0 |

#### 已确证结论 (真值表 = 6.10 的 8 色 bar)

- **NV12 色度路由: U ← CH1, V ← CH2** (e4: CH2=0 → V=0/U 正常; e5:
  CH1=0 → U=0/V 正常)。上轮初判的双通道同义/交换说法修正为本条。
- **e1 == max(e2, e3) 逐点成立** (numpy array_equal 校验,含 Y 平面所有
  微结构) → Y 平面 = CH0_EVEN 流与 CH0_ODD 流的并集/max 合成。
- **U ≡ V,值完全相同**;per-bar = [255,128,128,0,255,128,128,0]
  (修正列引用后。pix_truth.py 初版 U/V 列引用 +2 位错,曾报
  [255,255,128,128,128,128,0,0] — 以本行为准)。
  = **(B+R)/2 型混合** → chroma 通道吃到的是 B 与 R 字节交错的流,
  再经 2×2/平均下采样。G 通道的没进色度。与 "U=V=41" 的白墙异常同因。
- **Y 平面 = 132px 周期交错图像,不是 264px 条条 uniform**:
  - e3 (CH0_EVEN) → Y 平面**左半** (px 0-1055) 呈 132px 粒度
    [255×132][0×132] 交替 (= B 通道图样, bar 0/2/4/6 = 白/青/紫/蓝亮);
    **右半全黑**。
  - e2 (CH0_ODD) → Y 平面**左半全黑**;右半 (px 1056-2111) 连续
    528px 255 块 (+bar 边界 2-4px 口袋),位置对应 sensor bar 4/5 区。
  - e1 = 两者逐点 max (= 含 CH0_EVEN 与 CH0_ODD 两路流)。

#### 开放问题 (未解, 待下轮)

1. Y 平面 bar 边界 ±2-4px 微口袋: e1 bar0 内实测 [0@2-4]、[0@130-131]、
   [255@132-134]、[0@135-260]、[255@261] 等 2-4px 宽的错位小口袋,
   行/列奇偶均不相关 (row700-703 RLE 完全一致)。所有已提模型
   (2:1 decimation / plain16 paired / pBAA 5-byte period / plain8-MSB)
   均有小角失配,无法全对齐。**分析在此中止 (两轮循环卡死的教训,
   首要任务是固化数据而不是反推)**。
2. CH0_EVEN/CH0_ODD 的 "even/odd" 到底指什么维度: 实测它分工跨 Y 平面
   前后半 (不是简单列奇偶/行奇偶),且 e3 的左半图样 = B 通道分布。
   selector/nibble 语义整体仍未知,e2/e3 的分工是对现象的描述而非机制。
3. CH1/CH2 来源流与 (B+R)/2 的合流方式 (寄存器级实现路径) 未定位。

### 6.12 已固化资产 + 下轮入口

脚本 (全部已在 /tmp/opencode,产物已拉回):

- `L76e_e1..e6.raw`、`L76d_rdi.raw`、`L76_r76a/b.raw`、`L76_c1..c4.raw`
  = 全套判据数据,NV12 捕获均为 4967424 B,pBAA 4139520 B/帧。
- `mg76me.py` (rdi|pix 参数化换链, EBUSY 顺序安全)。
- `r76d.sh` / `r76e.sh` (自洽版, EBUSY 修复 + 正确 gain 编码 + 通道扫)。
- `r76_pix.sh`/`r76_pix_b.sh`、`r76c.sh` (存档, gain 写法旧)。
- `pix_truth.py` (每捕获 bar 表 + fine row 剖面; 注意 U/V 列引用 bug)、
  `bars_truth.py` (RDI→CFA 相位真值, bar 切片曾按全尺寸田形有 bug)、
  `sweep_look.py` (NV12→8色条统计)。
- 补丁 `camss-vfe41-pix-bayer.patch` (pkgrel 73, pmaports) 含
  demux_even/odd/gain0/gain1 module params (param=0 用补丁内置默认)。
- pBAA 解包参考 = `/home/xys/workspace/agent-workspace/camera-ref/
  tools/decode_ov13850.py` (byte0-3 高 8 位 + byte4 frag, SBGGR10 相位);
  demosaic/白墙金标准同源在此目录。

**下轮目标** (按优先级):

1. **不再重新反推微结构**。优先走增益/色彩标定: e6 (白墙 + 正确增益
   全 1.0) 的 U_实际 = 0x80? — **先核 e6 实际值**,若白墙 Y/增益线宽 →
   有基线即可直接对标 r72 的 WB (G=1.0, R=1.92, B=1.34) 推算
   demux_gain0/1 里 CH1/CH2 该放什么值, 让 U/V 白墙 ≈128 = 中性。
   (注意 U=CH1, V=CH2 — 上轮的 (B+R)/2 分析里"把 CH1/CH2 当同一流"
   的部分以 e4/e5 为准。)
2. 若色度流确实只能同基线,考虑**改补丁默认 selector** 试错 (同 nibble
   旋转族), 看 U≠V 分离是否可能出现。这一步不需要解 Y 微结构。
3. Y 微结构 (6.11 开放问题 1-3) 留给以后,非阻塞。
- 相关 pmaports 上游 patch 索引: demux EVEN/ODD cfg 的 YUV 常量 =
  `VFE_0_DEMUX_EVEN_CFG_PATTERN_*` 族;gain = §6.9 布局。

## 7. r77 轮：stock HAL 真值恢复 + PIX bayer 配置更正（内核 #75 待构建）

> 本节取代 §6.12 的"下轮目标"推测。两条独立证据链（开源 mm-camera VFE40 ISP 源码 +
> stock blob 反汇编）1:1 吻合，PIX 色度根因已锤定。

### 7.1 ground truth 来源（可复现）

- **开源 mm-camera VFE40 ISP 硬件层**：repo `bcyj/android_tools_leeco_msm8996`
  @ `f242858a8cbb2c9ad9b5b2e53d75380ae775727a`（2017-06-10），文件集已存
  `camera-ref/stock-src/mm-camera/`（demux40/demux44 + hw/modules/{demosaic,wb,
  colorcorrect,gamma,chroma_enhan}/ + isp_pipeline40.c/hw 头）。与 stock
  `libmmcamera2_isp_modules.so` 反汇编交叉验证一致（demux40_open / 字符串 xref）。
- 关键发现：
  1. **demux 是 7 连续字的块**（0x424 period | 0x428 gain0 | 0x42c gain1 |
     0x430/0x434 R-image(3D) gains | 0x438 evenCfg | 0x43c oddCfg），整块
     VFE_WRITE 下发。
  2. **bayer 用 period=1**（YUV422 才用 3）：BGBGBG → even=0xCA / odd=0x9C；
     GBRG 0xAC/0xC9；GRBG 0x9C/0xCA；RGGB 0xC9/0xAC。我们的 SBGGR10 → 0xCA/0x9C。
     （一致性校验：selector 的 nibble 数 = period+1；YUV 四常量与主线
     `VFE_0_DEMUX_*_PATTERN_*` 逐字节相同。）
  3. **demux gain = FLOAT_TO_Q(7,x) 的 12-bit 字段**（[11:0] / [27:16]）：
     0x80=1.0，上限 ≈31.9x。**设备实测线性放大成立**（白墙 U 43.1→64.1(1.5x)
     →122.4(2.9x)，0x3FF 才削顶）→ §6.12 的"增益不可放大"结论作废（那是
     TP 彩条 0/255 饱和造成的假象）。
  4. **通道语义**：ch0Even=G_even、ch0Odd=G_odd、ch1=B、ch2=R
     （demux40.c demux_trigger_update）。
  5. **bayer 时 CHROMA_UPSAMPLE(0x18 bit3) 必须关**：stock
     isp_pipeline40.c:367-373 `if (is_bayer_input == 0) chromaUpsampleEnable=1;
     else =0;`（该块是 YUV cositing 专用），realignmentBufEnable 亦=0。
     即 bayer 的 MODULE_CFG = BIT(2)|BIT(23)|BIT(27) = **0x08800004**
     （此前 r76 写 0x0880000C 是 YUV 值）。
  6. 全彩需完整 ISP 模块集（demosaic/WB/CC/chromaEnhan/RGB→YUV 等），
     register map 已整理进 stock 报告 Appendix A（Stage B 备用）。

### 7.2 r76 输出签名的机制解释（已数值验证）

- Y 平面 = 两个 1056 宽通道图逐行拼接：左 = CH0_EVEN、右 = CH0_ODD
  （e1 == max(e2,e3) 逐字节；e3 仅 px0-1055、e2 仅 1056-2111）。
  按 YUV selector 时左边是 B 相位图、右边是 G 相位图；132px = 半分辨率下的 TP
  bar 宽（264 全分辨率），2-4px "口袋" = 水平重采样相位伪迹（RDI 真值干净）。
- 色度 = CH1/CH2 同源 `U≡V=(B+R)/2`（绿权重精确为 0，2x2 binning）。
  根因 = YUV 的 period=3/0x9CAC 选通套在 bayer 上，B/R 相位被混叠进两路
  chroma，且 YUV-only 的 chroma-upsample 块被使能。
- 另有几何缺陷：CAMIF `width*2`（YUV422 two-samples-per-pixel 语义）使一个
  VFE 行吞两行 sensor 数据（实测 2:1 垂直折叠 + 帧重复）——颜色路由修好后
  再治，已备用 `camif_width` / `scale_in_div` 运行时参数 A/B。

### 7.3 r77 设备轮（kernel #74 / pkgrel 73，patch v1 环境）

- **格式/几何扫（F1）**：只有 NV12 @ pad 几何 (2112x1568) 能 STREAMON；
  NV12 1056/784、NV21/NV16/NV61 在 STREAMON 即 -EPIPE（`camss-video.c
  video_check_format()` 从 vfe0_pix pad 反推期望格式后强制覆盖，S_FMT 的 rc=0
  不代表接受）。→ 几何只能靠内核侧解决。
- **增益曲线（F2）**：gain0=Y only、gain1=U/V only；0x40=精确减半；TP 饱和
  数据上看不出放大（见 7.1.3 的更正）。
- **bayer selector 直测（B1: 0xCA/0x9C, period 仍=3）→ STREAMON 挂死**，
  保护性重启恢复；证明错 period 不是"misroute"而是 wedge，必须内核补丁。
- **12-bit 放大（A）**：白墙 g1 = 1.0/1.5/2.88/7.99x → U=43.1/64.1/122.4/227.4
  （5.28x 因削顶偏低，39.5% 饱和），Y 不变 → 线性、CH1/CH2 专用。
- **基线 sha 不再跨 boot 稳定**：r76e_e1(=L76_c1=L76f_f1) sha `c203ae21…` 与
  本次 boot 的 BASE sha `7838e5ae…` 只在 28 条 132px 台阶处的微口袋列不同
  （U/V 100% 相同）→ 旧 sha 只作历史；跨 boot 判据用几何+路由结构，不用 sha。
- **decimal 记录**：`0x9CAC = 40108`，不是 40106（40106=0x9CAA）。r76e/f 与
  r77_sel.sh 历史命令里标 0x9cac 的 40106 实际是 0x9CAA；对 v2 无影响
  （两者都是 YUV 图案），但后续扫参数一律用十进制正确值。

### 7.4 补丁 v2（`camss/camss-vfe41-pix-bayer.patch` 全面改写，pkgrel 74）

- bayer demux：period=1 + 按 CFA 相位选 even/odd（表见 7.1.2），YUV 分支不动；
- bayer MODULE_CFG：清 bit3 → 0x08800004（YUV 仍 0x0880000C）；
- demux 7-word 块补 0x430/0x434=0（stock parity）；
- 运行时参数（0=auto，全部下次 STREAMON 生效）：`demux_period`、
  `demux_even/odd`、`demux_gain0/1`（原始 32 位 Q7 语义）、`pix_module_cfg`
  （0x18 全局覆盖）、`camif_width`（CAMIF pixels-per-line 覆盖）、
  `scale_in_div`（2 = scaler 输入减半→2:1 水平放大）；
- 验证：与 pristine camss tree `patch -p1` 干净；按 APKBUILD 顺序
  driver→frame→pix 链式应用干净；`make M=.../camss modules`（aarch64
  cross-gcc 16.2）零告警，qcom-camss.ko+BTF 成功 → 已接 pmaports pkgrel=74，
  **待用户构建 kernel #75**。
- 判据（TP 彩条）：U 变 B 图案 [255,0,255,0,…]、V 变 R 图案
  [255,255,0,0,…]、U≠V；Y 变全宽 G。若 Y 仍折叠 → `camif_width` 试
  2112/1056/2640；若 Y 仍是左右两半 → `scale_in_div=2`；若 U/V 仍同源 →
  `pix_module_cfg` 反证。用户态重建：R=V, G=Y, B=U（2x2 上采样），
  色彩不足再上 Stage B（完整 ISP）。
- 用户态工具：`camera-ref/tools/nv12_recon.py`（伪 YUV→RGB/PNG）、
  `camera-ref/tools/routing_validator.py`（TP 路由判决 OLD/NEW model）。

### 7.5 后续（Stage B / 前摄）

- Stage B：完整 ISP（demosaic→WB→CC→chromaEnhan→RGB→YUV），实现参考
  `camera-ref/tools/r76/full_isp_stageB.md`（stock 报告 Appendix A 落地版）；
  gamma/rgbLUT 需 DMI，VFE4.1 主线无 DMI 管线，先跳过。
- 前摄：真流无帧界（r68 系）仍冻结；r77 起用 stock CSID/ISPIF 源码对照诊断
  （`camera-ref/tools/front/front_cam_diagnosis.md`）。

## 8. r78 设备轮：PIX 彩色通路上线（kernel #75 = pkgrel 74，patch v2）

### 8.1 门禁与 P1 判定（TP 彩条，全部参数 auto）

- kernel `#75 SMP PREEMPT Tue Oct 20 14:00:12 UTC 2026`；8 个参数节点齐备
  （camif_width demux_even demux_gain0 demux_gain1 demux_odd demux_period
  pix_module_cfg scale_in_div，全 0=auto）；dmesg 干净；无残留进程。
- **P1 = NEW_MODEL（成功）**：`r77_cap2.sh P1 0 0 0 0 0 0 0 0 tp` →
  4,967,424 B、无挂死，`NV12 2112x1568`；sha `a218c923…`。
  - Y = **全宽相干 G**（264px 彩条 [11110000]，seam ratio 0.00，无 tiling、
    无 2:1 垂直折叠、无 132px 半分辨率伪迹；8/8 对 RDI 真值，blend 0.998）
  - U = **B**（132px 图案 [1,0,1,0,…]，blend 1.008，8/8）
  - V = **R**（[1,1,0,0,…]，blend 1.002，8/8）
  - U≠V（旧 `(B+R)/2` 签名彻底消失）
- **P2（显式 demux_period=1）sha 与 P1 完全相同** → auto 路径正确。
- **色度通路固有 0.5x**：P1 上 U/V on-level 恰为 128 = 255/2（Y 满幅），
  由 GCHK_1 交叉验证（g1 CH2=0x40：U 不变、V 精确减半）→ HW 侧用
  demux_gain1 带 2x 基底补偿，或用户态 ×2。

### 8.2 WB（demux gain 即 AWB 执行器）

- WALL_A（g0=g1=1.0）：G(Y)=53.09、B(U)=25.18、R(V)=18.34。
- 由 U=B/2、V=R/2 得 Q7 12-bit 字段：B 2.108x→0x10E、R 2.895x→0x173 →
  `demux_gain1 = 0x0173010E`（g0 保持 0x00800080）。
- **WALL_C 验证 = 中性**：Y 53.28 / U 52.35 / V 52.20（U/G .9825、V/G .9798，
  U-V 0.15，0 削顶，重复帧 ±0.1）；微调字 `0x01750110` 达 .9888/.9876；
  余下 ~1.1% 是 `floor(gain*value)` 截断（非增益误差），再叠加
  波长相关 lens shading（需 LSC，用户态可预览校正）。
- 结论：**内核无需 AWB 循环**——用户态灰世界算 gain 后写 demux_gain1 即硬件 AWB
  （`nv12_recon.py --auto-wb` 已实现并回算出与人工完全相同的 0x0173010E）。

### 8.3 彩色渲染与真实场景

- TP 彩条渲染（R=V, G=Y, B=U，chroma×2）：8 条色相全对
  （white/yellow/cyan/green/purple/red/blue/black）
  → `camera-ref/frames/r78/recon/r78_P1_chroma2x_full.png`
- 真实场景 SCENE_C（exp3320/gain248 + 校准增益）：Y/U/V=53.27/52.32/52.25，
  渲染为中性墙帧 `…/recon/r78_SCENE_C_full.png`（手机当时对着无纹理暗墙，
  画面为真实传感器图像：墙面细节、边缘物件、lens shading 均可见）。
- SCENE_D（exp1500/gain32）近黑（Y mean 6.8）——传感器曝光被 VTS 上限
  3324 限制，暗场景没有更亮档位；这是场景限制，不是通路问题。

### 8.4 用户态工具（本轮定稿）

- `camera-ref/tools/nv12_recon.py`：NV12→RGB（伪 YUV 模型 R=V,G=Y,B=U，
  UV 交错自动检测，chroma×2，`--auto-wb` 灰世界 + 增益字输出，tonemap）。
- `camera-ref/tools/routing_validator.py`：TP 路由判决（OLD/NEW/PARTIAL），
  NEW_MODEL 判据 = 相干 G + U=B + V=R + U≠V，全 8 条真值对拍。
- 两者都用 r78 证据自检通过（见 camera-ref/frames/r78/recon/）。

### 8.5 剩余工作与结论

- **后摄"能用"已达标**：主线 6.12.1 + patch v2 下，OV13850 经
  csiphy0→csid0→ispif0→VFE0 PIX 输出可重建彩色 NV12（2112x1568，
  ~30fps 链路能力不变）。限制：chroma 为 2x2（四分之一分辨率）、无
  demosaic/CC/gamma/LSC（用户态可部分补偿）；Stage B 完整 ISP 参考已备
  （`camera-ref/tools/r76/full_isp_stageB.md`，chromatix 真值）。
- **前摄**：诊断完成（`camera-ref/tools/front/front_cam_diagnosis.md`）：
  stock CSID/ISPIF 与主线逐寄存器等价，TG 测试只验证了 LUT/decode/ISPIF/VFE，
  真实流独有的 lane 映射/phy_sel/CSIPHY 从未被验证。根因排序：R1 = OV5693
  上游模式表缺 stock MIPI/输出寄存器（0x4806/0x4816/0x4831/0x5000-02/0x5046
  等，且 19.2 vs 24MHz）；R2 = 前摄 lane 映射未校验。r78 起跑运行时
  T1（sensor 测试图）× T2（i2c 回读对照）分裂实验。
- 未动：13MP m0 全幅、HFR 档、AF(DW9761)/闪光灯(LM3646)。

### 8.6 彩色质量问题：发紫色偏的定量结论（r78b，用户上手观察触发）

用户观察: 渲染照片**上半截发紫**。定量复核确认存在**空间性色度缺陷**（非整体 WB）：

- 逐行 `R/G,B/G`（8 带）呈 U 形：顶 1.23/1.16 → 中 0.88/0.88 → 底 1.12/1.17；
  同场景 RDI raw 参考行带 flat（R/G 摆动 3.4%、B/G 0.9%）→ **PIX 通路注入，非场景/光学**。
- **曝光梯子（PIX A/B/C）分离出加性色度基底**：Y=36.14E+4.54、U=32.75E+8.22、
  V=33.04E+11.38（post-HW-gain）→ 色度基底 dU≈+8.2、dV≈+11.4（去掉增益 ≈+3.9/+3.9
  个 8-bit 码）；RDI raw 梯子完全线性（三通道信号比 0.067-0.069 vs 预期 0.070，
  无基底）→ 基底是 PIX 通路产物。**用户态可减**（`nv12_recon.py --chroma-offset`）。
- **第二个缺陷（用户态不可修）**：色度行在**亮横向边缘行**出现局部爆发
  （row 511-516、629-640：U/V 为帧均 1.7-2.2x，而该处 luma 正常）→ 就是照片上的
  **品红横线**；3/9 行中值滤波只能衰减不能消除；垂直相位扫描（40 条最强 luma 边缘）
  中位偏移 0.0 → 不是简单的色度行相位错位，更像色度通路的行处理/滤波缺陷。
- 排除项：逐通道黑电平（raw p1: B≈21/G≈23/R≈19，近似一致）；Q7 增益传输
  （同场景 back-to-back：U ×2.068/2.109=98.1%、V ×2.830/2.895=97.8%，Y 不变）；
  简单色度行平移（无一致偏移）。
- **结论**：当前 demux-only PIX 路径可出彩色（TP 8/8 真值、色相正确），但真实场景
  色度质量不达标（基底 + 行爆发），要做成"能拍照"的图像质量需要内核侧继续
  （色度通路/模块配置，已备 `pix_module_cfg` 运行时 A/B：下一步测 CHROMA_UPSAMPLE
  重新使能 0x0880000C）。**同时 RDI raw + 软件 ISP（decode_ov13850.py）已能出
  自然色彩照片**（同帧对比: `camera-ref/frames/r78/final/rdi_PAIR_RDI_gold_view.png`），
  是当前可用的成像路径。
- 用户态工具已升级（`nv12_recon.py`）：`--auto-wb`（灰世界+增益字输出）、
  `--chroma-offset`、`--chroma-row-median`、`--chroma-cal`、`--lsc`、`--ccm`、
  `--chroma edge`、批量模式；残差行偏 ~13% + 亮边行爆发仍在（内核侧问题）。

### 8.7 前摄诊断结论（r77-F，`camera-ref/tools/front/front_cam_diagnosis.md`）

- **stock CSID/ISPIF 与主线逐寄存器等价**（CORE_CTRL_0/1 编码、LUT dt 0x2b、
  CID_n_CFG=(decode<<4)|3、ISPIF INPUT_SEL/CID_MASK/INTF_CMD frame-boundary、
  VFE RDI WM 几何）→ 不存在"stock 编程差异"型根因。
- **r68e2 的 TG 通过是误导**：`csid_configure_stream()` 只在非 TG 分支写
  CORE_CTRL_0（lane_cnt/lane_assign）与 CORE_CTRL_1（phy_sel），而 TG 又禁止
  csiphy→csid 链接 → r68e2 只验证了 LUT/decode/ISPIF/VFE，**从未验证 lane 映射 /
  phy_sel / CSIPHY**，而这些正是真流独有变量。
- **r78 运行时分裂实验**：
  - T2（i2c 回读 51 个寄存器）：35 匹配 / 8 失配；所有 R1"缺失 stock MIPI 写入"
    （0x4806/4816/4831/4303-4315/5000-5002/5046/4837/4307）**全部与 stock 一致**；
    失配仅为 19.2MHz PLL 表（0x3098-309b）、VTS、subsample、已知 1-bit 0x4307。
    → **R1（传感器 MIPI 表）被证伪**。
  - T1（sensor 彩条测试图）：0x5e00=0x80 已写入、0x0100=0x01 表明 sensor 在出流，
    但 VFE 零帧（4s 看门狗 → 保护重启）→ 失败在 sensor 之后的链路：
    **R2/R3 = CSIPHY lane 映射 / CSID CORE_CTRL**。
- **下一步（需一次内核构建）**：`camera-ref/tools/front/proposed-front-instrumentation.patch`
  （已交叉编译验证，补丁链 driver→ov5693-retry→frame→pix→front 零告警）：
  CSID/CSIPHY/ISPIF stream-on 动态调试日志（dev_dbg，可动态开启）+ 运行时 lane
  覆写参数（csid_lane_assign/csid_lane_cnt/csid_core_ctrl1_lo、csiphy lane 位置/
  数量置换、ov5693 可选 stock-MIPI 写入开关），全部默认=当前行为。

### 8.8 PIX 色度问题后续：chroma-upsample 反证 + 机制定位（r78c）

- **运行时 A/B 反证**（`pix_module_cfg`，kernel #75 参数）：
  - bayer 默认 `0x08800004`（demux|scale|crop）→ 正常出帧（REF_SC_CAL sha
    `1a0c50b6…`，REF_SC_NG sha `87ca0f04…`）。
  - 重新使能 CHROMA_UPSAMPLE `0x0880000C`（demux|chromaUpsample|scale|crop）→
    **STREAMON 挂死**（场景/TP 两种都挂，看门狗保护重启）→ 主线对 bayer 关闭
    bit3 是必要且正确的；该块不能作为色度修复手段。
- **stock 源码复核（camss-auditor Appendix B）**：我们使能的模块集里
  **不存在** 硬件色度 DC 偏移/增益项——demux gain 是纯 Q7 乘法
  （FLOAT_TO_Q(7) 四舍五入，quantisation ≤0.4%），VFE40 里仅有的色度 DC 偏移
  在 chroma_enhan（0x640, kcb/kcr + Y offset）与 colorcorrect（0x5D0, K0-K2），
  这两个块在我们的 pseudo-YUV 路径里**未使能**（重置值不会注入）。
  ⇒ 观测到的 +8.2/+11.4 色度基底与 U 形行偏的机制 = **demux gain 放大 B/R 黑电平
  （G/luma 不放大）+ 固定逐通道增益乘上镜头 shading**（位置相关 chroma/luma 比），
  而不是 ISP 寄存器级偏移。彻底解决需 Stage B 完整 ISP（demosaic/WB/CC/
  chromaEnhan/RGB→YUV，参考 `camera-ref/tools/r76/full_isp_stageB.md`，
  chromatix 真值 + 模块字 0x08822814）。
- 结论：**当前 demux-only PIX 路径的成像质量到此为限**（彩色/路由正确，色度质量
  不足）；可用的自然色彩成像 = RDI raw + 软件 ISP（`decode_ov13850.py`）。
  Stage B 或用户态软件 ISP 产品化是后续两条路。

### 8.9 色度基底定论：未扣除的 sensor 黑电平（BLC 未启用）

stock 源码审计（`camera-ref/tools/r76/chroma_transfer_memo.md`）给出强证据链：

- 使能的 PIX 模块集里**不存在**任何色度 DC/scale 寄存器项（demux 28 字节块只有
  period/gains/R-gain/cfg，无 offset；chroma-upsample 只有 1-bit cositing；WB 只有
  3×9-bit Q7 增益；CC 的 K 偏移在 OV13850 chromatix 里=0 且 CC 未使能；
  输出格式化主线 VFE4.1 不编程 io_format——最接近的 chroma DC 偏移只在
  chroma_enhan 的 kcb/kcr=128，而该块 bit17=0 未使能，TP 黑条 U=0 亦反证）。
- **+pedestal 机制 = 从未扣除的 sensor 黑电平**：stock bayer 恒开
  LINEARIZATION（BLC）；OV13850 BLC = {R,Gr,Gb,B}={69,65,66,69}/12-bit
  = {17.25,16.25,16.5,17.25}/10-bit（与实测 BL10=16.8 吻合）。我们的路径不做
  BLC；chroma 通路 0.5x、用户态再 ×2，于是**同一黑电平在色度上被放大两倍**：
  预测 Y=4.10/U=9.10/V=12.50（实测 4.54/8.22/11.38），**U/V 基底比
  gB/gR=0.7278 vs 实测 0.7223（1% 吻合）**——DC 偏移不会随 CH1/CH2 增益比缩放，
  未扣 BLC 会 ⇒ 定论。用户态修正必须按当前 demux 增益字换算（不能硬编码）。
- **~0.93 斜率差不是 PIX 增益**：参考模型（BL 减后 clip）的非线性 + PIX/RDI 帧
  未严格配准（block search 有 ~(32,-32) 原始像素的系统位移、相关仅 0.76-0.84）
  + demux-only 色度是稀疏/抽取（B/R 每 4 像素一个站点 + 2-tap 平均 → 0.5x），
  与全分辨率 RDI 模型对比必然有混叠残差 ⇒ **行状色度误差的根源是 demux-only
  色度抽取（无寄存器可修），只有 Stage-B demosaic 能根治**。
- 后续（内核侧候选，未排期）：在 camss 里实现 LINEARIZATION/BLC 模块编程
  （用上述 OV13850 BLC 值）可从硬件上扣黑电平；或直接上 Stage-B 完整 ISP。

### 8.10 用户态 BLC 校正落地（r78e）

- `nv12_recon.py` 新增 **`--blc`**：按 chromatix BLC 常量
  `{R,Gr,Gb,B}={69,65,66,69}`（12-bit）与**当前 demux 增益字**换算
  `offset_Y=(Gr+Gb)/2/16·gG`、`offset_U=B_blc/16·gB`、`offset_V=R_blc/16·gR`
  （`--blc-gains`/`--blc-consts`/`--gain1` 可覆盖）。对 `0x0173010E`：
  Y−4.09 / U−9.10 / V−12.50，与实测梯子 4.54/8.22/11.38 及 U/V 基底比
  0.7278 vs 0.7223（1%）吻合。
- 重渲染（bilinear chroma + BLC + 逐帧灰世界）：aimed scene 平均
  R/G/B 从 40.3/36.5/36.8（magenta）→ **36.4/36.4/36.4（近中性）**；
  暗区紫色消除。**残余行状色度误差不被 BLC 影响**（R/G 行带摆动 14.2%）
  → 确认属 demux-only 抽取混叠（§8.9），用户态只能 `--chroma-row-median`
  掩盖，根治需 Stage-B demosaic。
- **重要内核联动注意**：`0x0173010E` 是在"含基底"的数据上标定的；若未来内核
  启用 BLC/linearization，须**重新标定 AWB 字**（post-BLC 白墙给出的字显著更低，
  e.g. wall 重算 residual 0x009C008F）。
- 证据：`camera-ref/frames/r78/final/11b_PAIR_PIX_A_blc_*`、
  `blc_before_after.txt`、`camera-ref/tools/r78_colour_report.md` §4b/4c。

## 9. Stage-B 完整 ISP 实现（r80，pkgrel 76 / kernel #77 待构建）

目标：从 demux-only 伪 YUV 升级到 VFE4.1 真正 ISP bayer 链，解决
（1）色度质量（抽取混叠）与（2）未扣 BLC 的基底。

- 补丁：`camss/camss-vfe41-pix-full-isp.patch`（增量于 pix-bayer v2，单文件
  camss-vfe-4-1.c，370 行）。全部 runtime 门控、**默认关**（pix_full_isp=0 =
  现行为）：
  - `pix_full_isp`：0=off / 1=B1（demux+demosaic+WB）/ 2=B2（+CC+chromaEnh
    RGB→YUV）/ 3=B2+linearization(BLC)
  - 模块字：A 0x08800004/05（+BLC）、B1 0x08800814/15、B2 0x08822814/15
    （CHROMA_UPSAMPLE 对 bayer 恒 0）
  - demosaic：0x440 mix、0x518 interp WB、0x520 classifier 18 字、0x568
    interp G（OV13850 chromatix 真值；4 处文档位域打包错误在实现中修正，
    见 full_isp_stageB.md §10）
  - WB 0x580（r72 默认 R/B/G=246/128/172，param 可调）；CC 0x5D0 TL84 矩阵
    （Q7 含负值，identity 回退 `pix_cc_mode=0`）；chromaEnh/RGB→YUV 0x640
    TL84 系数（c 0.5732/b −0.3178/a 0.5885/d −0.16/kcb=kcr=128）
  - LINEARIZATION/BLC（`pix_linearization=1`）：17 字 knee @0x37C +
    36 字 DMI LUT（bank0；新增 vfe_dmi_write32 写 0x910/0x914/0x91c），
    OV13850 12-bit BLC {R,Gb,Gr,B}={69,66,65,69} 原值不除 4
- 验证：全补丁链 patch -p1 干净、qcom-camss.ko 在 ksrc-6.12.1 交叉编译
  零告警（含 W=1）。
- 上机验证顺序（r79 设备轮）：B0 回归（pix_full_isp=0 应等于 P1 sha）→
  BLC 暗场（Y/U/V≈0）→ B1 TP 不挂/不黑 → B2 TP 8 色真值（白 U≈V≈128 且
  U≠V、红/蓝 Y 低、绿/黄/青 Y 高，demux gain 保持 1.0 避免 WB 双乘）→
  B2+BLC 墙/场景。黑帧/花色时依次试 pix_cc_mode=0、pix_ce_mode=0、
  pix_cc_qfactor=2，并抓 dmesg violation/overflow。
- 注意：BLC 启用后旧白平衡字 0x0173010E（含基底标定）需重标定；
  用户态 chroma offset 置 0。

### 9.1 r79 设备轮结果：Stage-B 完整 ISP 在真机上跑通（kernel #77）🎉

- **B0 回归**：`pix_full_isp=0` TP 帧与 r78 P1 **sha 完全相同**（`a218c923…`）
  ⇒ 全 ISP 补丁未扰动 demux-only 路径。
- **硬件 BLC 证实**（电子暗场：曝光 1/增益 1）：Stage-A `lin=1` 基座
  Y/U/V `3.99/4.0/4.0 → 0/0/0`；全 ISP `full=3` 亦把色度基座 `V=132→128`
  清零。TP 彩条不受 BLC 影响（`B2blc` sha == `B2`）——与预测一致。
- **B1**（demux+demosaic+WB，无 CC/CE）：正常出帧、非黑，但过饱和（`interp=1`
  几乎无改善）→ 说明 CC/CE（RGB→YUV 转换）是必需环节。
- **B2**（+CC+chromaEnh/RGB→YUV = 模块字 0x08822814）：**TP 8 色全部命中真值**
  白 (254.7,126.7,128.2)、黄、青、绿、品、红 (76.7,83.4,254.9)、蓝 (28.9,251.7,106.9)、
  黑；**U≠V 8/8**（旧 `(B+R)/2` 签名彻底消失）、Y 明暗次序正确。
- **真实场景（墙）**：B2 输出 **色度中性** `U=129.7 / V=130.6`（|U−V|=2.87），
  对比 Stage-A 的横向色偏是质变。用标准 full-range BT.601 YCbCr→RGB 渲染，
  TP 彩条/墙均正确 ⇒ **主线内核第一次输出真正彩色图像**。
- **B2+BLC 的坑**：亮场景下 `full=3` 亮度 +47%、色度偏移（U142.6/V120.9）
  → linearization 曲线在亮部改变色调平衡（stock 把 linearization 绑定在
  digital gain/AGC 上）。**结论：BLC 默认关闭**（`pix_linearization=0` 已是默认），
  保持为暗场/诊断工具；若将来启用需做曝光门控。
- 过程事故（已修复并披露）：r79 首版脚本把所有 Stage-B 参数写 0，导致
  `pix_wb_gain_*=0` / cc/ce=0 → 首轮 B1/B2 黑帧；脚本改为"未显式给出则不写、
  恢复时写编译默认值"后重跑，以上结论均来自修复后的干净轮次。
- 遗留/下一步：
  1. **默认策略**：是否把 `pix_full_isp` 编译默认改为 2（B2）——需一次内核构建；
     在此之前可由用户态在开流前写 `pix_full_isp=2`。
  2. **AWB**：B2 后 demux gain 不应再用于 WB（会与 WB 模块双乘）；AWB 改为
     用户态写 `pix_wb_gain_r/g/b`（9-bit Q7）；CC 目前固定 TL84，阳光场景可能需
     D65 矩阵或自适应。
  3. 真实场景/多光源取样与调参；13MP/HFR 档；AF/闪光灯。
  4. 前摄 payload 数据面诊断（见 §8.7 更新）仍在并行。

### 9.2 真实场景彩色照片达成（r79 屏幕实拍）

- 用户将后摄对准笔电屏幕。屏幕内容在动 → 相机 bracket 各帧非逐像素可比
  （同参 5s 重拍 Y 相关 0.43），单帧统计仍有效：
  - 最佳曝光 = **2400/64**（Y p99 197、max 222、0% 削顶；预置 bracket
    3320/248 削顶 18.5%、2400/96 p99 249、1200/48 p99 82 均不理想）
  - `SCENE_Bm`（B2/full=2, 默认 WB 246/128/172）：Y 38.34 / U 118.51 / V 143.02
  - `SCENE_B0`（同取景同曝光 full=0 Stage-A）：Y 37.47 / U 14.92 / V 15.90
    （伪 YUV 编码，与 B2 色度域完全不同）
- 渲染：**B2 用标准 full-range BT.601 YCbCr→RGB**（`nv12_recon --colormode bt601`）
  → `camera-ref/frames/r79/final/07_SCENE_Bm_bt601_*`：**可辨识的真实彩色照片**
  （屏幕缩略图红/蓝/橙等颜色正确、键盘/桌面结构清晰）；Stage-A 同场景
  `08_SCENE_B0_stageA_*` 颜色明显偏（绿/品红），对比体现 Stage-B 的质变。
- **WB/CC 调参结论**（当前光线、B2）：默认 r72 WB（246/128/172）最中性
  （|U−V|=2.86）；中性 205/205 与 290/250 都更差（27.9/17.8）；CC 恒等仅让
  灰墙 |U−V| 略降（1.36）但会毁彩色 → **保持 CC TL84 (cc_mode=1)**，ccq 无感。
- **BLC 策略（定论）**：同一亮场景 full=2→full=3 的增亮随曝光下降而爆炸
  （3320/248: +47%、2400/96: +117%、1200/48: +285%）且各曝光都偏色
  （dU +13..+17 / dV −10..−26）→ **BLC 默认关闭**；只有贴近标定点
  （~3320/248）才合理，将来若启用需按 stock 做曝光/数字增益门控。
- 待办：静态画面重拍以得到逐像素对齐的 Stage-A/B 对比与更干净的照片；
  把 `pix_full_isp` 编译默认改为 2（B2）待用户确认后随下次构建生效。
