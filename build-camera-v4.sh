#!/bin/sh
# Build + sideload kernel #77 (pkgrel 76): rear-camera Stage-B full ISP.
# Adds demosaic/WB/colour-correction/chroma-enhance+RGB->YUV and optional
# LINEARIZATION/BLC to the bayer PIX path, ALL gated via module params and OFF
# by default (pix_full_isp=0 keeps the current demux-only behaviour).
# After build+sideload: reboot the device, then the Stage-B validation round runs
# (pix_full_isp=1/2/3, pix_linearization=1, etc.).
set -e
PKG=linux-postmarketos-qcom-msm8916
HOST=172.16.42.1   # USB; WiFi fallback: 192.168.2.189
echo "== build $PKG (pkgrel 76, kernel #77) =="
pmbootstrap build --arch aarch64 "$PKG"
echo "== sideload to $HOST =="
pmbootstrap sideload --host "$HOST" "$PKG"
echo "== done: reboot the device to load kernel #77 =="
