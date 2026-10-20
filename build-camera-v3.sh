#!/bin/sh
# Build + sideload the r77-F front-camera instrumentation kernel (pkgrel 75 / kernel #76).
# Defaults keep current behaviour; it only adds stream-on diagnostic logs and
# runtime lane-override params so the front-camera lane-map hypotheses can be tested.
# Run in a normal user terminal (pmbootstrap needs its usual sudo/chroot).
# After it finishes: reboot the device (ssh user@172.16.42.1 sudo reboot) to load #76.
set -e
PKG=linux-postmarketos-qcom-msm8916
HOST=172.16.42.1   # USB; WiFi fallback: 192.168.2.189
echo "== build $PKG (pkgrel 75, kernel #76) =="
pmbootstrap build --arch aarch64 "$PKG"
echo "== sideload to $HOST =="
pmbootstrap sideload --host "$HOST" "$PKG"
echo "== done: reboot the device to load kernel #76 =="
