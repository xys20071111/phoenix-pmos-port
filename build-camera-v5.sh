#!/bin/sh
# Build + sideload kernel #78 (pkgrel 77):
#   - front-camera CSID/ISPIF payload observability (r81-F): counters, IRQ raw,
#     gated stock-parity A/B knobs. Defaults unchanged.
#   - rear Stage-B: pix_full_isp now defaults to 2 (B2 full ISP = true colour);
#     set pix_full_isp=0 at runtime for the legacy demux-only Stage-A path.
# After build+sideload: reboot, then we run the front payload round and the
# Stage-B scene captures.
set -e
PKG=linux-postmarketos-qcom-msm8916
HOST=172.16.42.1   # USB; WiFi fallback: 192.168.2.189
echo "== build $PKG (pkgrel 77, kernel #78) =="
pmbootstrap build --arch aarch64 "$PKG"
echo "== sideload to $HOST =="
pmbootstrap sideload --host "$HOST" "$PKG"
echo "== done: reboot the device to load kernel #78 =="
