#!/bin/bash
# mkvar.sh NAME PATCH.py — experiment SPIR-V: copies the worktree's shaders/vulkan to NAME/src, runs PATCH.py NAME/src
# (edits the GLSL in place), compiles the RT scene shaders into NAME/spv over a copy of build-rel's SPIR-V (bench.py --shaders).
set -e
W=/mnt/2nd4TB/IKV-wt-w29-topkapi3; D=$(cd "$(dirname "$0")" && pwd)/$1
rm -rf "$D"; mkdir -p "$D/src" "$D/spv"
cp $W/shaders/vulkan/* "$D/src/"; cp $W/build-rel/shaders/vulkan/*.spv "$D/spv/"
python3 "$2" "$D/src" ${@:3}
for s in scene_rt.frag scene_rt_guided.frag temporal.frag; do glslc --target-env=vulkan1.2 -I "$D/src" "$D/src/$s" -o "$D/spv/$s.spv"; done
echo "$D/spv"
