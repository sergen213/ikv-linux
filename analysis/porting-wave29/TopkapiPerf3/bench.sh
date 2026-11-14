#!/bin/bash
# bench.sh BUILD TAG [backends] [spots] [extra bench.py args...] — wave 29 TopkapiPerf3: Topkapı Ruh Taşı ring vs Drezin at
# the user's resolution (2743x1543), view 7, camera yaw 0.6 pitch 0.35 dist 46, on the private kwin_wayland --virtual
# socket ikv-topkapi3 (60 Hz), via analysis/porting-wave28/Horizon/bench.py (flock -x per run, IKV_GPU_TIMING=1).
B=$1; T=$2; BK=${3:-vk-user,vk-user-nofg}; SPOTS=${4:-ring,far}; shift 4 2>/dev/null
export WAYLAND_DISPLAY=ikv-topkapi3 IKV_DEV_SECRETS=$HOME/.local/share/ikv-dev/secrets-w26b
O=$(cd "$(dirname "$0")" && pwd)/metrics
mkdir -p "$O"
declare -A AT=([ring]="0,285" [far]="0,-150" [stones]="-8,262" [park]="386,935")
declare -A ZONE=([ring]=topkapi [far]=topkapi [stones]=topkapi [park]=eminonu)
# GPU shader clock while a client of BUILD runs (median reported): frame generation halves the rendered rate on the 60 Hz
# virtual output, so a lower clock would inflate every pass.
clock() { F=$(ls /sys/class/drm/card1/device/hwmon/*/freq1_input); : > "$1"
  while :; do pgrep -f "$B/ikv-client" >/dev/null && echo $(( $(cat $F) / 1000000 )) >> "$1"; sleep 0.1; done; }
for s in ${SPOTS//,/ }; do
  clock "$O/$T-$s.sclk" & CK=$!
  python3 /mnt/2nd4TB/IKV/analysis/porting-wave28/Horizon/bench.py --build $B --out $O --label $T-$s --zone ${ZONE[$s]} --at ${AT[$s]} \
    --camera 0.6,0.35,46 --view 7 --res 2743x1543 --ticks 500 --backends $BK "$@" 2>&1 | grep -E "^$T-"
  kill $CK
  echo "$T-$s sclk median MHz: $(sort -n "$O/$T-$s.sclk" | awk '{a[NR]=$1}END{print a[int(NR/2)+1], "n=" NR}')"
done
