#!/bin/zsh
# run.sh <run name> [Section/Key=value ...]: boots a PS3 ISO in an aPS3e build on the Odin 3, plays the scenario
# and writes the measured segments to runs/<run name>/.
#   PKG       aPS3e package (default: this fork, aenu.aps3e.odin3; aenu.aps3e is the stock build)
#   SCENARIO  scenarios/<SCENARIO>.txt (default gowa); its `iso` line names the ISO in the SD card ps3 folder and
#             its `serial` line the game whose per-game config the edits go to
#   ISO_URI   the URI handed to aPS3e instead of file://<SD card ps3 folder>/<iso> (the stock build only opens the
#             content:// URIs of the folder it was granted)
#   TARGET    frame rate the game aims for, for the slow-frame count (default 30)
#   KEEP=1    leave the game running at the end
#   HOLD=1    keep the lock for the next run of a series (the series releases it)
#   LOCK_OWNER  lock owner (default ps3)
# The edits are written over the game's per-game config (or the global one), restored at the end.
# Scenario lines: iso <file> | serial <ID> | wait <s> | btn <name> [ms] | stick|rstick <x> <y> <ms> |
#   tap <x> <y> | repeat <n> <line> | shot <name> | until <s> <logcat regex> | measure <label> ... end
A=~/Library/Android/sdk/platform-tools/adb
D=${0:A:h}
NAME=$1; shift
OUT=$D/runs/$NAME
PKG=${PKG:-aenu.aps3e.odin3}
SC=$D/scenarios/${SCENARIO:-gowa}.txt
F=/storage/emulated/0/Android/data/$PKG/files/aps3e
ROMS=/storage/FBDD-1F17/Emulations/ROMs/ps3
T=/data/local/tmp/aps3e
ACT=aenu.aps3e.EmulatorActivity
[ -f $SC ] || { echo "no scenario $SC"; exit 2; }
ISO=$(sed -n 's/^iso //p' $SC)
SERIAL=$(sed -n 's/^serial \([^ #]*\).*/\1/p' $SC)
CUSTOM=$F/config/custom_cfg/$SERIAL.yml
rm -rf $OUT; mkdir -p $OUT || exit 1

OWNER=${LOCK_OWNER:-ps3}
${D:h:h:h}/vita3k/tools/odin3/lock.sh take $OWNER || exit 1
# The player's per-game config comes back at the end
trap '$A shell pkill -f $T/poll.sh; [ "$KEEP" = 1 ] || { $A shell am force-stop $PKG
  $A shell "rm -f $CUSTOM; [ -e $CUSTOM.player ] && mv $CUSTOM.player $CUSTOM"; }
  [ "$HOLD" = 1 ] || ${D:h:h:h}/vita3k/tools/odin3/lock.sh release $OWNER' EXIT

focused() { $A shell dumpsys window | grep mCurrentFocus | grep -q "$PKG/$ACT"; }
pad() { focused || { echo "ABORT: aPS3e lost the focus" | tee -a $OUT/summary.txt; exit 1; }; $A shell sh $T/pad.sh "$@"; }
shot() {
  print "$1 $($A shell date +%T.%N | cut -c1-12)" >> $OUT/shots.txt
  $A exec-out screencap -p > $OUT/$1.png && sips -Z 960 $OUT/$1.png --out $OUT/$1.png >/dev/null
}

$A shell mkdir -p $T
$A push $D/pad.sh $D/poll.sh $T/ >/dev/null
$A shell am force-stop $PKG
# Start every run from a cool GPU so thermal throttling does not depend on the previous run
for i in $(seq 60); do [ $($A shell cat /sys/class/kgsl/kgsl-3d0/temp | tr -d '\r') -lt 42000 ] && break; sleep 2; done

$A shell appops set $PKG MANAGE_EXTERNAL_STORAGE allow 2>/dev/null
$A shell "[ -e $CUSTOM.player ] || { [ -e $CUSTOM ] && cp $CUSTOM $CUSTOM.player; }"
# The edits apply over what the player uses for this game: its per-game config, else the global one with the
# game's database config, which the fork applies at boot (God of War Ascension hangs after its intro without it)
DB=()
$A pull $CUSTOM.player $OUT/base.yml >/dev/null 2>&1 ||
  { $A pull $F/config/config.yml $OUT/base.yml >/dev/null || exit 1; DB=(--db $SERIAL); }
python3 -I $D/setyml.py $OUT/base.yml $OUT/config.yml $DB "$@" || exit 1
$A shell mkdir -p ${CUSTOM:h}
$A push $OUT/config.yml $CUSTOM >/dev/null || exit 1

$A shell input keyevent KEYCODE_WAKEUP; $A shell wm dismiss-keyguard
$A logcat -c
$A shell am start -n $PKG/$ACT -a aenu.intent.action.APS3E --es iso_uri "'${ISO_URI:-file://$ROMS/$ISO}'" >/dev/null
for i in $(seq 60); do focused && break; sleep 1; done
focused || { echo "ABORT: the Emulator activity never got the focus" | tee $OUT/summary.txt; exit 1; }
echo "pkg=$PKG iso=$ISO scenario=${SC:t} edits=$* start=$(date +%T)" > $OUT/context.txt

game_layer() {
  local n
  for n in ${(u)${(f)"$($A shell dumpsys SurfaceFlinger --list | tr -d '\r' |
      grep -oE "SurfaceView\[$PKG/${ACT//./\\.}\]\(BLAST\)#[0-9]+")"}}; do
    print -r -- "$($A shell "dumpsys SurfaceFlinger --latency '$n'" | awk 'NF==3 && $2>0 {c++} END {print c+0}') $n"
  done | sort -rn | head -1 | cut -d' ' -f2-
}
SEG=""
seg_begin() {
  SEG=$OUT/$1; mkdir -p $SEG
  # Looked up again every segment: the game can recreate its surface (a new BLAST layer) after the boot
  LAYER=$(game_layer)
  # ${PKG}, not $PKG: zsh would read :e as the extension modifier
  PID=$($A shell pidof ${PKG}:emu | tr -d '\r')
  $A shell "cat /proc/$PID/task/*/stat" > $SEG/threads_start.txt
  $A shell sh $T/poll.sh 3600 > $SEG/poll.csv &
  # SurfaceFlinger keeps the last 128 presents: sample every second
  { while [ ! -f $SEG/.stop ]; do $A shell "dumpsys SurfaceFlinger --latency '$LAYER'"; sleep 1; done } > $SEG/sf_latency.txt &
  T0=$SECONDS
}
seg_end() {
  $A shell "cat /proc/$PID/task/*/stat" > $SEG/threads_end.txt
  echo $((SECONDS - T0)) > $SEG/duration
  touch $SEG/.stop; $A shell pkill -f $T/poll.sh; wait
  python3 -I $D/summarize.py $SEG ${TARGET:-30} | tee -a $OUT/summary.txt
  SEG=""
}
step() {
  case $1 in
    iso|serial|""|\#*) ;;
    wait) sleep $2;;
    btn|stick|rstick) pad "$@";;
    tap) focused || { echo "ABORT: aPS3e lost the focus" | tee -a $OUT/summary.txt; exit 1; }
      $A shell input swipe $2 $3 $2 $3 250;;
    repeat) local n=$2; shift 2; for i in $(seq $n); do step "$@"; done;;
    # until <s> <regex>: waits up to <s> seconds for a logcat line (the end of the PPU/SPU compilation)
    until) local s=$2; shift 2
      for i in $(seq $s); do $A logcat -d | grep -qE "$*" && break; sleep 1; done
      echo "until '$*': $i s" | tee -a $OUT/summary.txt;;
    shot) [ -z "$SEG" ] || { echo "a screenshot during a measure stalls SurfaceFlinger" >&2; exit 2; }; shot $2;;
    measure) seg_begin $2;;
    end) seg_end;;
    *) echo "unknown scenario line: $*" >&2; exit 2;;
  esac
}
# Read the whole file first: adb shell would swallow the rest of the scenario from stdin
for line in "${(@f)$(<$SC)}"; do step ${=line%%\#*}; done
[ -z "$SEG" ] || seg_end
$A logcat -d > $OUT/logcat.txt
