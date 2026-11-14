#!/bin/bash
# analyze.sh STYLE VIEW REGIONSET [variant=on] — compare parity/<style>-<view>-off1 vs off2 (run-to-run noise) and vs <variant>
# at every captured tick (compare.py: MAD/>8 share per region, diff x8, contact sheet in parity/cmp/).
D=$(cd "$(dirname "$0")" && pwd); S=$1 V=$2 R=$3 X=${4:-on}
mkdir -p $D/parity/cmp
for f in $D/parity/$S-$V-off1-t*.png; do
  t=${f##*-off1-}; t=${t%.png}
  echo "== $S $V $t"
  VIEW=$R python3 $D/compare.py $D/parity/cmp/$S-$V-$X-$t off1:$f off2:$D/parity/$S-$V-off2-$t.png $X:$D/parity/$S-$V-$X-$t.png
done
