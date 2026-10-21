#!/bin/sh
# Build + sideload kernel #80 (pkgrel 79): front-camera runtime register overrides.
# Adds the ov5693 module param `stream_regs` (comma separated REG=VAL hex pairs)
# written at every stream start, after the global table / mode config and before
# 0x0100=1. Empty by default = unchanged behaviour. This lets us bisect the
# remaining front-camera MIPI output problem (no RAW10 long packets reach CSID)
# across the stock register table without rebuilding for every candidate.
# After build+sideload: reboot, then we bisect (a few reboots per round).
set -e
PKG=linux-postmarketos-qcom-msm8916
HOST=172.16.42.1   # USB; WiFi fallback: 192.168.2.189
echo "== build $PKG (pkgrel 79, kernel #80) =="
pmbootstrap build --arch aarch64 "$PKG"
echo "== sideload to $HOST =="
pmbootstrap sideload --host "$HOST" "$PKG"
echo "== done: reboot the device to load kernel #80 =="
