#!/bin/sh
# Build + sideload the r77 camera PIX kernel patch (pkgrel 74 / kernel #75).
# Run this in a normal user terminal (pmbootstrap needs its usual sudo/chroot).
# After it finishes: the device must be rebooted to load the new kernel
# (the agent can do that over ssh if you prefer, or: ssh user@172.16.42.1 sudo reboot).
set -e
PKG=linux-postmarketos-qcom-msm8916
HOST=172.16.42.1   # USB; WiFi fallback: 192.168.2.189
echo "== build $PKG (pkgrel 74) =="
pmbootstrap build --arch aarch64 "$PKG"
echo "== sideload to $HOST =="
pmbootstrap sideload --host "$HOST" "$PKG"
echo "== done: reboot the device to load kernel #75 =="
