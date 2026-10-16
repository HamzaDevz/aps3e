#!/bin/zsh
# probe.sh <out dir> <seconds> [apk]: installs the APK if given, boots God of War Ascension in the fork with the
# RPCS3 log on, takes a screenshot every 10 s and keeps the log. Call it while holding the Odin lock.
#   ISO_URI  URI handed to aPS3e instead of file://<ISO>;  CFG  per-game config pushed for the run, removed after
A=~/Library/Android/sdk/platform-tools/adb
OUT=$1; SECS=$2; APK=$3
PKG=${PKG:-aenu.aps3e.odin3}
F=/storage/emulated/0/Android/data/$PKG/files/aps3e
ISO='/storage/FBDD-1F17/Emulations/ROMs/ps3/God of War - Ascension (USA) (En,Fr,Es,Pt).iso'
mkdir -p $OUT || exit 1
[ -n "$APK" ] && { $A install -r $APK | tail -1; $A shell appops set $PKG MANAGE_EXTERNAL_STORAGE allow; }
$A shell am force-stop $PKG
$A shell rm -f $F/logs/rp3_log.txt
CUSTOM=$F/config/custom_cfg/BCUS98232.yml
[ -n "$CFG" ] && $A push $CFG $CUSTOM >/dev/null
$A shell input keyevent KEYCODE_WAKEUP
$A logcat -c
$A shell am start -n $PKG/aenu.aps3e.EmulatorActivity -a aenu.intent.action.APS3E --ez enable_log true \
  --es iso_uri "'${ISO_URI:-file://$ISO}'" >/dev/null
for i in $(seq $((SECS / 10))); do
  sleep 10
  $A exec-out screencap -p > $OUT/t$((i * 10)).png && sips -Z 480 $OUT/t$((i * 10)).png --out $OUT/t$((i * 10)).png >/dev/null
done
$A pull $F/logs/rp3_log.txt $OUT/ >/dev/null
$A logcat -d > $OUT/logcat.txt
$A shell am force-stop $PKG
[ -n "$CFG" ] && $A shell rm -f $CUSTOM
grep -E '^·[EF] ' $OUT/rp3_log.txt | grep -v 'Image resource' | cut -c1-160 | tail -5
