# WiFi (WCNSS/pronto + wcn36xx) 调试记录 - PARKED

## 现象
- WCNSS PIL 正常启动, 版本查询 OK (WCNSS 1.5 / 1.2), mac 随机 (NV 无效).
- `WCN36XX_HAL_DOWNLOAD_NV_REQ (55)` 发出去 (SMD 抓包确认 3088B 首片),
  芯片零回包 (`HAL <<<` 计数永远 0), 10s 超时 x2.
- ~100s 后 `dog.c:1682 Watchdog task starvation` -> crash -> (已关 auto-recovery).
- 附带伤害: crash 循环会永久攥住 rtnl 锁 (`ip`/`getty` 卡死在 rtnl_dumpit D 状态).

## 已排除
- NV 文件内容: 原厂 persist 版 (29KB, 无魔数, 稀疏) 与 db410c 版 (31KB,
  CAFEBABE+dense) 行为完全一致.
- WCNSS 固件版本: modem 分区版 (2016, M8936FAAAANUZM) 与 stock 线刷包版 (2015)
  行为完全一致 (OVERRIDE 机制验证生效, 版本查询同为 1.5/1.2).
  stock 固件从 `/mnt/local_data/pb1-770n/NON-HLOS.bin` 提取, 工具见 `fatread.py`.
- RF 供电: 下游反推 iris 四路 (vddxo=l7, vddrfa=s3, vddpa=l9, vdddig=l5),
  stock DTB 交叉确认一致, 无 dummy 警告, 行为不变.
- iris 写法: 与树内正常设备一致, probe 无报错.
- DTB: 自研 phoenix DTB 与 kiwi/a7 surrogate 行为一致 (通道级问题, 非板级).
- stock 原厂 DTB 交叉验证: panel 初始化仅差 2 字节 gamma + 1 条命令;
  wcnss/iris/USB/传感器供电映射与我方一致, DTS 参数可信.
- WCNSS 固件文件齐全 (modem 分区 wcnss.mdt + b00/b01/b02/b04/b06/b09/b10/b11).

## 抓包 (SMD_DUMP)
- TX: `37 00 00 00 10 0c 00 00 ...` (type=55, ver=0, len=3088) + NV 表.
- RX: 无.

## 待试 (按优先级)
1. 带数据去 #msm8916 问两个尖锐问题:
   a. 当前主线 6.x 上还有能跑的 pronto WLAN (wcn36xx) 设备吗?
      (怀疑整条 WLAN_CTRL 路径在新固件/新内核下已无人验证.)
   b. WCN3620 vs WCN3660 如何区分? (若本机实为 3660, RF 初始化全错.)
2. 黑名单测试结论: wcn36xx 拉黑后 PIL 根本不起 (无 crash, idle) —
   说明 remoteproc 启动由 wcn36xx 触发, crash 需要 NV 交互.
   (测试后 extlinux 已清 module_blacklist, /boot 已换回 nowcnss.)

## 当前状态
- /boot 跑 nowcnss DTB (wcnss disabled), 系统健康, getty/ssh/`ip` 正常.
- RF 版 DTB 留在 /boot (`msm8939-lenovo-phoenix.dtb.rf-tested`, 48529B).
- 调试残留 (userdata, 重启不丢): /etc/systemd/system/wcnss-norecover.service (enabled),
  /etc/modprobe.d/wcnss.conf? (实际文件名: wcn36xx.conf, debug_mask=12).
- 远程复现: 切回 rf DTB (`cp *.rf-tested` 覆盖), reboot, `dmesg | grep HAL`.
