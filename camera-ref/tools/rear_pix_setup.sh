#!/bin/ash
# ============================================================================
# rear-pix-setup.sh
# Configure the rear camera (OV13850) VFE PIX output pipeline so that the
# video node (msm_vfe0_video3) emits NV12 2112x1568 directly consumable by
# ffmpeg / any V4L2 consumer.
#
# Usage:
#   ash /home/user/rear-pix-setup.sh [exposure] [gain] [test_pattern]
#     exposure       sensor rows, default 2400  (max ~3320 in this mode)
#     gain           analogue_gain raw, default 64  (0x80 = 1.0x)
#     test_pattern   0 = real scene (default), 1 = 8-colour vertical bar
#
# After success it writes /home/user/rear-pix.env (source it, then use
# $REAR_VIDEO with ffmpeg).
# ============================================================================

# --- self-elevate to root if needed ----------------------------------------
if [ "$(id -u)" != "0" ]; then
  SPASS="${SUDO_PASS:-123456}"
  exec sh -c "echo '$SPASS' | sudo -S ash '$0' $*"
fi

EXP="${1:-2400}"
GAIN="${2:-64}"
TP="${3:-0}"

case "$EXP"  in ''|*[!0-9]*) echo "BAD exposure: $EXP"; exit 64;; esac
case "$GAIN" in ''|*[!0-9]*) echo "BAD gain: $GAIN"; exit 64;; esac
case "$TP"   in 0|1) ;; *) echo "BAD test_pattern: $TP (0|1)"; exit 64;; esac

UP=/proc/uptime
M()  { echo "[MRK] rear-pix $1 $(cut -d' ' -f1 $UP)"; }
die(){ echo "SETUP_FAIL: $1"; exit "${2:-1}"; }

# --- guard: no active stream consumer -------------------------------------
if ps aux 2>/dev/null | grep -E 'v4l2-ctl|ffmpeg' | grep -v grep >/dev/null; then
  echo "ABORT: a stream consumer is already running:"
  ps aux 2>/dev/null | grep -E 'v4l2-ctl|ffmpeg' | grep -v grep
  die STALE_STREAM_PROCESS 3
fi
M start

# --- 1. sensor bind + power -----------------------------------------------
H=/sys/bus/i2c/devices/3-0036
if [ ! -e "$H/driver" ]; then
  echo 3-0036 > /sys/bus/i2c/drivers/ov13850/bind 2>/dev/null \
    || die "bind ov13850 failed" 4
fi
echo on > "$H/power/control" 2>&1 || die "sensor power on failed" 4
sleep 0.2
M sensor_power

# --- 2. resolve media / video / subdev nodes -------------------------------
MEDIA=$(ls /dev/media* 2>/dev/null | head -1)
[ -n "$MEDIA" ] || die "no /dev/media* node" 2
VN=""; PS=""
for d in /sys/class/video4linux/video*; do
  [ -e "$d/name" ] || continue
  case "$(cat "$d/name")" in msm_vfe0_video3*) VN="/dev/${d##*/}";; esac
done
for d in /sys/class/video4linux/v4l-subdev*; do
  [ -e "$d/name" ] || continue
  case "$(cat "$d/name")" in ov13850*) PS="/dev/${d##*/}";; esac
done
[ -n "$VN" ] || die "msm_vfe0_video3 not found (kernel too old? need pkgrel>=76)" 2
[ -n "$PS" ] || die "ov13850 subdev not found" 2
chmod a+rw "$VN" "$PS" 2>/dev/null
echo "[INFO] media=$MEDIA  video=$VN  subdev=$PS"
M nodes_resolved

# --- 3. media graph: disable rdi0 + front leftovers, then enable PIX chain ---
# (EBUSY rule: same ispif source pad -> disable before enable)
disable_link() { media-ctl -d "$MEDIA" -l "$1" >/dev/null 2>&1 || true; }
enable_link()  { media-ctl -d "$MEDIA" -l "$1" || die "link enable failed: $1" 6; }

disable_link "'msm_ispif0':1 -> 'msm_vfe0_rdi0':0 [0]"
disable_link "'msm_csiphy1':1 -> 'msm_csid1':0 [0]"
disable_link "'msm_csid1':1 -> 'msm_ispif1':0 [0]"
disable_link "'msm_ispif1':1 -> 'msm_vfe0_rdi0':0 [0]"
disable_link "'msm_ispif1':1 -> 'msm_vfe0_rdi1':0 [0]"
disable_link "'msm_ispif1':1 -> 'msm_vfe0_rdi2':0 [0]"

enable_link "'msm_csiphy0':1 -> 'msm_csid0':0 [1]"
enable_link "'msm_csid0':1 -> 'msm_ispif0':0 [1]"
enable_link "'msm_ispif0':1 -> 'msm_vfe0_pix':0 [1]"
M links_set

# --- 4. unified SBGGR10_1X10 @ 2112x1568 on every pad ----------------------
FMT="fmt:SBGGR10_1X10/2112x1568 field:none"
for ent in "'ov13850 3-0036':0" "'msm_csiphy0':0" "'msm_csiphy0':1" \
           "'msm_csid0':0" "'msm_csid0':1" \
           "'msm_ispif0':0" "'msm_ispif0':1" \
           "'msm_vfe0_pix':0" "'msm_vfe0_pix':1"; do
  media-ctl -d "$MEDIA" --set-v4l2 "$ent [$FMT]" >/dev/null 2>&1 \
    || echo "[WRN] fmt set failed on $ent"
done
M fmt_set

# --- 5. video node format ---------------------------------------------------
v4l2-ctl -d "$VN" --set-fmt-video=width=2112,height=1568,pixelformat=NV12 \
  >/dev/null 2>&1 || die "v4l2 set-fmt failed on $VN" 5
M videofmt_set

# --- 6. sensor controls -----------------------------------------------------
v4l2-ctl -d "$PS" --set-ctrl=test_pattern="$TP" >/dev/null 2>&1
v4l2-ctl -d "$PS" --set-ctrl=exposure="$EXP" >/dev/null 2>&1
v4l2-ctl -d "$PS" --set-ctrl=analogue_gain="$GAIN" >/dev/null 2>&1
M ctrls_set

# --- 7. ISP knobs: ensure B2 + sane defaults (idempotent) -------------------
P=/sys/module/qcom_camss/parameters
[ -e "$P/pix_full_isp" ]        && echo 2 > "$P/pix_full_isp"        2>/dev/null || true
[ -e "$P/pix_linearization" ]   && echo 0 > "$P/pix_linearization"   2>/dev/null || true
M isp_defaults

# --- 8. persist env for later ffmpeg runs ----------------------------------
cat > /home/user/rear-pix.env <<EOF
export REAR_MEDIA="$MEDIA"
export REAR_VIDEO="$VN"
export REAR_SUBDEV="$PS"
EOF
chmod a+r /home/user/rear-pix.env

# --- 9. verification -------------------------------------------------------
echo "---- verification ----"
v4l2-ctl -d "$VN" --get-fmt-video 2>&1 | grep -iE 'Width|Height|Pixel Format|Size Image'
echo "pix_full_isp   = $(cat $P/pix_full_isp 2>/dev/null || echo n/a)"
echo "test_pattern   = $(v4l2-ctl -d $PS --get-ctrl=test_pattern 2>/dev/null)"
echo "exposure       = $(v4l2-ctl -d $PS --get-ctrl=exposure 2>/dev/null)"
echo "analogue_gain  = $(v4l2-ctl -d $PS --get-ctrl=analogue_gain 2>/dev/null)"
M done
echo "REAR_PIX_SETUP_OK  video=$VN  subdev=$PS  media=$MEDIA"
echo "Source /home/user/rear-pix.env then run ffmpeg against \$REAR_VIDEO"