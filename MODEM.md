# MODEM.md — modem (MPSS/QDSP6) 移植记录

PB1-770N 的基带是 MSM8939 集成 modem（MPSS.DPM.2.0.c12-00057-M8936FAAAANUZM-
1.51002.3, 2016-04 构建，8936/8939 家族）。本文记录主线侧打通、固件提取、
用户空间链路与遗留限制。

## 结果快照 (2026-10-16)

- PIL: `4080000.remoteproc` running（MBA booted + mpss 加载，无 watchdog）。
- QRTR/WWAN: `/dev/wwan0qmi0`、`/dev/wwan0at0/1`（rpmsg_wwan_ctrl），
  `qmicli -d /dev/wwan0qmi0` 可见 DMS/NAS/UIM/WDS 等全部服务。
- ModemManager: modem 对象创建成功（plugin qcom-soc，net+qmi+2×at 共 11 口），
  IMEI/运营商/SIM 全部读出。
- **短信收发 OK**（`mmcli --messaging-*`，本地卡 registered/home）。
- **通话不可用**：这颗 modem 固件**不支持 VoLTE**（stock/LineageOS 下同样
  打不了电话）—— 属固件限制，不是移植层问题（详见"通话为何搁置"）。
- 移动数据：bam-dmux netdev 已注册（见下），调通与否未验证（stretch）。
- Q6 媒体音频：卡换成 `msm8916`（MultiMedia1..4 + CS-Voice）。2026-10-22:
  UCM/混音器失配已定位并修复（卡名失配 + `RDAC2 MUX` 缺设定，改用
  `soc-qcom-msm8916-ucm`，见 `BOOT-STATUS.md` "耳机声道解决"），待上机复测。

## OpenRC 开机启动项（2026-10-24 起由包自动接线）

曾需每次刷机后手动开启（`rc-update add <svc> default`）——当时拉进来的任何
包都不负责启用它们：

 - modemmanager
 - rmtfs
 - msm-modem-uim-selection

根因核对（2026-10-24，对 edge 镜像 2026-10-18 rootfs）：`msm-modem-uim-
selection-openrc`/`rmtfs-openrc` 只装 initd+conf.d，**不带 rc-update 脚本**
（已核验 rootfs `lib/apk/db/scripts.tar.gz`）；`postmarketos-base-ui` 只在
plasma-mobile/sxmo/gnome 这类 UI 的 openrc 子包里启用 modemmanager，
fbkeyboard 不启用；ModemManager 本体也没有任何依赖拉入（见下节）。

现状：device 包（8-r9）`package()` 直接提供三个符号链接
`/etc/runlevels/default/{modemmanager,rmtfs,msm-modem-uim-selection}`
（同 `phoenix-usbgadget` 的 `local` 链接模式）。runlevel 全部取 **default**：
上游 `soc-qcom-modem-openrc.post-install` 的口径（uim-selection 的 initd
写的是 `need rmtfs`/`before modemmanager`，同为同 runlevel 语义），且
2026-10-16 的启动时间线（mpss 于 t≈60s——用户态起跑后——上线）与
`rmtfs -s` 在 default 阶段拉起 mss remoteproc 的行为吻合。已手动启用过的
存量 rootfs 升级该包时为幂等 no-op。systemd 根文件系统不读
`/etc/runlevels`，这几行只是无害的多余文件。

q6voiced 仍由上游 `soc-qcom-msm8916-rproc-openrc` 的 post-install
（`rc-update add q6voiced default`）负责，本包不重复接线。**注意**：
2026-10-18 构建的 fresh rootfs chroot 里，该 post-install（27-r0）存在
但并未产生 `/etc/runlevels/default/q6voiced` 链接，而同一事务的
`postmarketos-base-ui-openrc` post-install 正常生效——原因未查明（疑与
安装事务内 `-openrc` 子包和其 target 的提交顺序有关）。新刷镜像首启后应
核对 `rc-update show`：modemmanager/rmtfs/msm-modem-uim-selection 应已
因本包接线在列；q6voiced 若缺就补 `rc-update add q6voiced default`
（不影响短信，只影响通话音频——对这颗固件本就无可验证场景）。

对已刷机但 device 包还没升到 r9 的系统，一条命令补齐（幂等）：

```
for s in modemmanager rmtfs msm-modem-uim-selection; do rc-update add $s default; done
```

## 用户空间链

device 包 depends（本地追加）：

```
firmware-lenovo-phoenix-modem  # /lib/firmware/{mba.mbn,modem.mdt,modem.bNN}
soc-qcom-msm8916-rproc         # msm8916 通用包的子包: msm-modem + q6voiced 依赖
q6voiced                       # 通话音频(对这颗固件而言无意义, 保留依赖链)
msm-modem msm-modem-uim-selection   # uim 槽位选择; 拉入 rmtfs libqmi qmi-utils
modemmanager                   # SMS/运营商标识; fbkeyboard+OpenRC 无包代拉
```

`ModemManager`（2026-10-24）改为 device 包 depends **显式**依赖。旧口径
"由 `postmarketos-base-ui` 依赖带入"在 OpenRC/fbkeyboard 镜像上不成立：
base-ui-62 的依赖表只有 `postmarketos-base-ui-modemmanager` 等子包组，
仅当 MM 已装才投递配置文件，本地 fresh rootfs 的包数据库里根本没有
modemmanager 本体（设备上 2026-10-16 起用 MM，是当时在设备上手动 apk add
的最可信解释）。rmtfs 走 QRTR（内核
`qcom,rmtfs-mem` + `QCOM_RMTFS_MEM`），dts 的 rmtfs 区域放大到 stock 的
0x160000（dtsi 默认 0xe0000 不够 EFS 用）。开机启动接线见上节。

## DTS 要点

- `#include "msm8916-modem-qdsp6.dtsi"`（同 msm8939-samsung-a7 用法）：
  使能 `&mpss`/`&mpss_mem`/`&bam_dmux`/`&apr`/`&memshare`，把 `&sound` 换成
  `qcom,msm8916-qdsp6-sndcard`（MultiMedia1..4 + CS-Voice 前端），`&lpass`
  置 reserved —— modem 起来后 LPASS 归 Q6，direct-LPASS 旁路彻底失效。
- `&mpss_mem = 0x5600000 @0x86800000`（86 MiB，照抄 stock
  `modem_adsp_region`；提取的 modem ELF 实际跨度 85 MiB）。
- 旧 `&sound`（apq8016-sbc-sndcard, direct-LPASS）整块删除；
  `&pm8916_codec` 板级属性（micbias/MBHC/HPHL）保留给 Q6 后端用。

## 固件提取

stock modem 分区（NON-HLOS, mmcblk0p1）是 FAT 镜像，`fatread.py` 提取：

```
python3 fatread.py backups/modem.bin ls IMAGE            # MBA.MBN MODEM.* ...
python3 fatread.py backups/modem.bin extract IMAGE/MBA.MBN
python3 fatread.py backups/modem.bin extract IMAGE/MODEM.B05
# FAT 名全大写，提取后统一改小写；b 文件索引即 MODEM.MDT 的 PHDR 索引
```

需要的集合（与 FAT 逐一对应，PHDR 索引 27 段缺 7/10/11/24）：
`modem.mdt`, `modem.b00..b26`（21 个实际存在的文件）, `mba.mbn`。
打包进 `firmware-lenovo-phoenix-modem` 子包，直接放 `/lib/firmware/`
（`qcom_q6v5_mss` `request_firmware` 请求的就是 `mba.mbn`/`modem.mdt`/`modem.bNN`）。

## bam-dmux netdev 补丁（本移植特有）

坑：ModemManager 核心对 QMI modem **无条件要求 net 口**（`mm-base-modem.c`：
"Failed to find a net port in the QMI modem"），而上游 `qcom_bam_dmux` 只在
modem 发 `BAM_DMUX_CMD_OPEN` 时才注册 netdev。8916 家族的固件启动时会主动
宣布全部数据通道（上游作者 Gerhold 的 patch 说明），但这颗 2016 版 8939
固件**不会** —— 通道要等数据业务激活才开。结果是 MM 拒建 modem →
没人发 WDS 请求 → 通道永远不开，死锁。

解法：`modem/qcom-bam-dmux-register-netdevs.patch` —— probe 时直接注册
`wwan0..wwan7`（`dev_port` = 通道号，供 MM 的 WDS Bind 映射），通道握手
不变（ndo_open 发 OPEN / 远端 OPEN attach）。副作用可控：8 个 RAWIP
pointopoint 接口常驻 `ip link`，down 状态无流量。

## 验证时间线

```
dmesg:
  [60.06] remoteproc0: Booting fw image mba.mbn, size 230272
  [60.11] qcom-q6v5-mss: MBA booted without debug policy, loading mpss
  [61.89] remoteproc0: remote processor 4080000.remoteproc is now up
  [62.1 ] qcom,apr: Adding APR/GPR dev: aprsvc:service:4:{3..b}   # Q6 音频全挂上
  [62.6 ] wwan wwan0: port wwan0qmi0/wwan0at0/wwan0at1 attached
mmcli -m any:
  firmware: MPSS.DPM.2.0.c12-00057-M8936FAAAANUZM-1.51002.3 (2016-04)
  ports: wwan0(net) wwan1..7(net) wwan0at0/1(at) wwan0qmi0(qmi)
  IMEI/SIM/operator 全部读出; 收发短信 OK
```

q6asm-dai 的 `command[0x10dac] not expecting rsp` 是 PipeWire 与 APR 就绪
竞速的良性警告（PCM 打开时序），无实害。

## 通话为何搁置

这颗 modem 固件（MPSS.DPM.2.0.c12 / 8936FAAAANUZM 2016-04）**不支持
VoLTE**，stock Android 与 LineageOS 在这台设备上同样打不了电话 —— 不是
移植层的缺失。q6voiced + CS-Voice 声卡链路（`&q6voicedai CS_VOICE`）与
q6voiced 守护进程已就位，若将来换到带 VoLTE/CS 语音能力的固件或网络，
通话音频的路由零工程量（音频只是没有可验证的场景）。

## 漫游卡教训

一张漫游卡注册被拒：`network rejection error: location-area-not-allowed`
(GMM cause 13)。属网络侧对该 PLMN 在此位置区的漫游限制（换本地的卡立即
registered/home），不是移植缺陷。调试期间容易被误判成"modem 没通"。

## 后续可做

- Q6 音频复测（播放链路已修, 待上机复测）：`msm8916-audio-init.sh` 卡名
  已修 `phoenix`→`msm8916`、手写 `ucm2/phoenix` 已删并改用通用
  `soc-qcom-msm8916-ucm`（device-8-r8, 见 `BOOT-STATUS.md` 2026-10-22 节）
  —— 耳机"不分左右"根因即卡名失配 + `RDAC2 MUX` 缺设定。剩余：装包
  复测播放/录音、通话音频（本固件无 VoLTE 仍无可验证场景）。
- 移动数据（stretch）：`wwan0`（bam-dmux）走 QMI WDS bind → NM/MM bearer；
  上游 patch 文档口径是 Raw-IP only（Ethernet mode 对多数固件是坏的）。
- GPS（`&gps_mem`/`memshare` 已使能，LOC/PDS 服务在 QRTR 里可见
  （service 16），需要 loc 服务栈）。
- modem 与 wcn36xx 长期共存稳定性（wcnss blacklist 仍保留，未解锁验证）。
