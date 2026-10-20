#!/bin/sh
# Build + sideload kernel #79 (pkgrel 78): FRONT-CAMERA MIPI data-type fix.
# The front OV5693 real stream was sending CSI-2 long packets with data type
# 0x12 instead of RAW10 0x2B (CSID unmapped_hdr=0x120ca834: DT=0x12,
# WC=0x0CA8=3240 = one 2592x10b line), so no frame reached VFE. This kernel
# writes the stock value of sensor reg 0x4307 (0x30 instead of mainline 0x31).
# Also keeps the #78 diagnostics knobs (payload counters etc.).
# After build+sideload: reboot, then we test the front camera (TP + real scene).
set -e
PKG=linux-postmarketos-qcom-msm8916
HOST=172.16.42.1   # USB; WiFi fallback: 192.168.2.189
echo "== build $PKG (pkgrel 78, kernel #79) =="
pmbootstrap build --arch aarch64 "$PKG"
echo "== sideload to $HOST =="
pmbootstrap sideload --host "$HOST" "$PKG"
echo "== done: reboot the device to load kernel #79 =="
