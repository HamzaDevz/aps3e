# Runs on the device: sh poll.sh <seconds>. One CSV row per second: GPU load and clock, CPU clocks of the small
# (policy0) and prime (policy6) clusters, hottest CPU and GPU zones, battery and USB input power.
K=/sys/class/kgsl/kgsl-3d0; B=/sys/class/power_supply/battery; U=/sys/class/power_supply/usb
P=/sys/devices/system/cpu/cpufreq
CZ=""; GZ=""
for z in /sys/class/thermal/thermal_zone*; do
  case $(cat $z/type 2>/dev/null) in cpu-*) CZ="$CZ $z/temp";; gpuss-*) GZ="$GZ $z/temp";; esac
done
echo t,gpu_busy,gpu_mhz,c0_mhz,c6_mhz,cpu_tmax,gpu_tmax,throttling,batt_ma,batt_mv,usb_ma,usb_mv
i=0
while [ $i -lt $1 ]; do
  echo $(cut -d" " -f1 /proc/uptime),$(cut -d" " -f1 $K/gpu_busy_percentage),$(( $(cat $K/gpuclk) / 1000000 )),$(( $(cat $P/policy0/scaling_cur_freq) / 1000 )),$(( $(cat $P/policy6/scaling_cur_freq) / 1000 )),$(cat $CZ 2>/dev/null | sort -n | tail -1),$(cat $GZ 2>/dev/null | sort -n | tail -1),$(cat $K/throttling),$(( $(cat $B/current_now) / 1000 )),$(( $(cat $B/voltage_now) / 1000 )),$(( $(cat $U/online) * $(cat $U/current_now) / 1000 )),$(( $(cat $U/voltage_now) / 1000 ))
  sleep 1
  i=$((i+1))
done
