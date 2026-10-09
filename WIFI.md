# WiFi (WCNSS/pronto + wcn36xx) 调试记录 - PARKED

## 现象
- WCNSS PIL 正常启动, 版本查询 OK (WCNSS 1.5 / 1.2), mac 随机 (NV 无效).
- `WCN36XX_HAL_DOWNLOAD_NV_REQ (55)` 发出去 (SMD 抓包确认 3088B 首片),
  芯片零回包 (`HAL <<<` 计数永远 0), 10s 超时 x2.
- ~100s 后 `dog.c:1682 Watchdog task starvation` -> crash -> (已关 auto-recovery).
- 附带伤害: crash 循环会永久攥住 rtnl 锁 (`ip`/`getty` 卡死在 rtnl_dumpit D 状态).

## 已排除
- NV 文件内容: 原厂 persist 版 (29KB, 稀疏) 与 db410c 版 (31KB, CAFEBABE+dense) 行为完全一致.
- RF 供电: 下游反推出 iris 四路 (vddxo=l7 1.8V, vddrfa=s3 1.3V, vddpa=l9 3.3V,
  vdddig=l5 1.8V), 已接入 DTS, 无 dummy 警告, 行为不变.
- iris 写法: 与树内正常设备一致 (wcnss 子节点 + compatible), probe 无报错.
- DTB: 自研 phoenix DTB 与 kiwi/a7 surrogate 行为一致 (通道级问题, 非板级).
- WCNSS 固件文件齐全 (modem 分区 wcnss.mdt + b00/b01/b02/b04/b06/b09/b10/b11).

## 抓包 (SMD_DUMP)
- TX: `37 00 00 00 10 0c 00 00 ...` (type=55, ver=0, len=3088) + NV 表.
- RX: 无.

## 待试 (按优先级)
1. 带数据去 #msm8916 问: 同样 zero-RX 的 pronto 案例? (怀疑 2016 版 Lenovo WCNSS
   固件与主线 DOWNLOAD_NV 协议对不上, 或 RF 实为 WCN3660.)
2. 试 iris compatible 换 wcn3660 (注意 vddpa 2.9-3.0V / vdddig 1.2V, l9 固定 3.3V
   可能设压失败, 先看 probe 报什么).
3. 下游 TWRP dmesg 找 prima RF 版本行 (TWRP 不起 WiFi, 没抓到).
4. BT 是否活 (同 WCNSS, 另一通道): 若 BT 活则是 WLAN HAL 任务专属问题.

## 当前状态
- /boot 跑 nowcnss DTB (wcnss disabled), 系统健康, getty/ssh/`ip` 正常.
- RF 版 DTB 留在 /boot (`msm8939-lenovo-phoenix.dtb.rf-tested`, 48529B).
- 调试残留 (userdata, 重启不丢): /etc/systemd/system/wcnss-norecover.service (enabled),
  /etc/modprobe.d/wcnss.conf? (实际文件名: wcn36xx.conf, debug_mask=12).
- 远程复现: 切回 rf DTB (`cp *.rf-tested` 覆盖), reboot, `dmesg | grep HAL`.
