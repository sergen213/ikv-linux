#!/bin/bash
# [RUNS="off1 off2 on main"] runparity.sh STYLE VIEW... — same-build parity captures (sparse foliage/glossy off = "off", twice
# for the noise floor, then "on"; "main": the unmodified main c78fcd0 build in ../IKV-wt-w29-topkapi3-base) on the private kwin socket ikv-topkapi3, via parity.py (flock -x per run). Output: parity/<style>-<view>-<run>-t<tick>.png
export WAYLAND_DISPLAY=ikv-topkapi3 IKV_DEV_SECRETS=$HOME/.local/share/ikv-dev/secrets-w26b
D=$(cd "$(dirname "$0")" && pwd); B=/mnt/2nd4TB/IKV-wt-w29-topkapi3/build-rel
STYLE=$1; shift
declare -A ARGS=(
  [ring]="--zone topkapi --at 0,285 --camera 0.6,0.35,46"
  [close]="--zone topkapi --at -8,262 --camera 0.6,0.25,14"
  [tower]="--zone topkapi --at 0,285 --camera 2.2,0.2,16"
  [park]="--zone eminonu --at 386,935 --camera 0.6,0.35,30"
  [walk]="--zone topkapi --at -8,262 --camera 0.6,0.25,20 --walk 10,0"
  [parkwalk]="--zone eminonu --at 386,935 --camera 0.6,0.35,30 --walk 12,12"
)
OFF="--env IKV_RT_SPARSE_FOLIAGE=0 --env IKV_RT_SPARSE_REFLECTION=0"
for v in "$@"; do
  for run in ${RUNS:-off1 off2 on}; do
    E=; BB=$B; [[ $run == off* ]] && E=$OFF; [[ $run == main ]] && BB=/mnt/2nd4TB/IKV-wt-w29-topkapi3-base/build-rel
    python3 $D/parity.py --build $BB --out $D/parity --tag $STYLE-$v-$run --style $STYLE ${ARGS[$v]} $E
  done
done
