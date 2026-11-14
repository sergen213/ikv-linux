#!/bin/bash
# final_bench.sh — alternated A/B: main c78fcd0 (../IKV-wt-w29-topkapi3-base) vs wave29-topkapi3, ring and Drezin, the user's
# settings (frame generation on) and without frame generation, at the 60 Hz virtual output's FIFO (as the user runs it) and
# at full load (IKV_VSYNC=0, matched GPU clock). Labels fin-<build>-<vsync>-<round>-<spot>.
D=$(cd "$(dirname "$0")" && pwd)
declare -A B=([main]=/mnt/2nd4TB/IKV-wt-w29-topkapi3-base/build-rel [new]=/mnt/2nd4TB/IKV-wt-w29-topkapi3/build-rel)
for r in 1 2; do for v in fifo full; do for b in main new; do
  E=; [ $v = full ] && E="--env IKV_VSYNC=0"
  $D/bench.sh ${B[$b]} fin-$b-$v-$r vk-user,vk-user-nofg ring,far $E
done; done; done
