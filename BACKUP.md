# TWRP 备份步骤 (PB1-770N, 可清空但先备份以便回退)

 cihaz 已接受清空，但 boot/modem/persist 必须先备份，否则变砖无法恢复基带。

1. 手机进 TWRP (Lineage 自带 recovery 或已刷 TWRP):
   adb -s $SERIAL reboot recovery

2. recovery 下确认 root shell:
   adb shell id   # 应为 uid=0(root)

3. 备份关键分区到 /tmp 再 pull 到电脑:
   adb shell "dd if=/dev/block/mmcblk0p19 of=/tmp/boot.img bs=4096"
   adb shell "dd if=/dev/block/mmcblk0p20 of=/tmp/recovery.img bs=4096"
   adb shell "dd if=/dev/block/mmcblk0p1 of=/tmp/modem.bin bs=4096"
   adb shell "dd if=/dev/block/mmcblk0p12 of=/tmp/modemst1.bin bs=4096"
   adb shell "dd if=/dev/block/mmcblk0p13 of=/tmp/modemst2.bin bs=4096"
   adb shell "dd if=/dev/block/mmcblk0p24 of=/tmp/persist.img bs=4096"
   mkdir -p /tmp/opencode/phoenix-backup
   for f in boot.img recovery.img modem.bin modemst1.bin modemst2.bin persist.img; do adb pull /tmp/$f /tmp/opencode/phoenix-backup/$f; done
   ls -lh /tmp/opencode/phoenix-backup/

4. 同时提取硬件信息 (TWRP 下无 SELinux 限制):
   adb shell "cat /proc/cmdline; ls /sys/class/graphics/fb0/; cat /sys/class/drm/card0-DSI-1/modes; ls /sys/bus/i2c/devices/; dmesg | grep -i -E 'panel|dsi|touch|syna|ft5|novatek|atmel'" > hwinfo.txt

5. Fastboot 检查 lk2nd 可刷性:
   adb reboot bootloader
   fastboot devices
   # 不要先 flash，先 `fastboot boot lk2nd.img` 测试
