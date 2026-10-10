#!/usr/bin/env python3
"""Compile the vendored FidelityFX SDK v1.1.4 FSR 3.1.4 GLSL passes with glslc and emit a C++ header holding
the SPIR-V and the descriptor reflection the SDK Vulkan backend expects (replaces the Windows-only FidelityFX_SC).

Only the permutations IKV's contexts can request are built: upscaler HDR on/off x sharpening on/off; reference Lanczos,
low-resolution unjittered motion vectors, non-inverted depth, wave32-or-native, no FP16 (src/fsr_vk.cpp
clamps the reported device capabilities to match). Frame generation: one frame-interpolation permutation (low-resolution
unjittered motion vectors, non-inverted depth) and one optical-flow permutation (SDR input). Compile flags mirror the
SDK's CMakeCompile{FSR3Upscaler,Frameinterpolation,Opticalflow}Shaders.txt.
"""
import argparse, hashlib, os, struct, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

# Index = order of FfxFsr3UpscalerPass up to DEBUG_VIEW (ACCUMULATE_SHARPEN shares accumulate), then GENERATE_REACTIVE.
PASSES = ["prepare_inputs", "luma_pyramid", "shading_change_pyramid", "shading_change", "prepare_reactivity",
          "luma_instability", "accumulate", "rcas", "debug_view", "autogen_reactive"]
BASE = ["-DFFX_GPU=1", "-DFFX_GLSL=1",
        "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
        "-DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
        "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
        "-DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
        "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
        "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=0",
        "-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1",
        "-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0",
        "-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=0"]
# FfxFrameInterpolationPass order; pass sources sdk/src/backends/vk/shaders/frameinterpolation/ffx_frameinterpolation_<name>.glsl.
FI_PASSES = ["reconstruct_and_dilate_pass", "setup_pass", "reconstruct_previous_depth_pass", "game_motion_vector_field_pass",
             "optical_flow_vector_field_pass", "disocclusion_mask_pass", "pass", "compute_inpainting_pyramid_pass",
             "inpainting_pass", "compute_game_vector_field_inpainting_pyramid_pass", "debug_view_pass"]
FI_DEFINES = ["-DFFX_GPU=1", "-DFFX_GLSL=1",
              "-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
              "-DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
              "-DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
              "-DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
              "-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
              "-DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=1",
              "-DFFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS=0",
              "-DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=0"]
# FfxOpticalflowPass order; sources sdk/src/backends/vk/shaders/opticalflow/ffx_opticalflow_<name>.glsl.
OF_PASSES = ["prepare_luma_pass", "compute_luminance_pyramid_pass", "generate_scd_histogram_pass",
             "compute_scd_divergence_pass", "compute_optical_flow_advanced_pass_v5", "filter_optical_flow_pass_v5",
             "scale_optical_flow_advanced_pass_v5"]
OF_DEFINES = ["-DFFX_GPU=1", "-DFFX_GLSL=1", "-DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT=0"]
KINDS = ["cbv", "srv_texture", "uav_texture", "srv_buffer", "uav_buffer", "sampler"]


def reflect(words):
    """Descriptor bindings as {kind: [(set, binding, count, name)]}, classified like the SDK's SPIRV-Reflect use."""
    if words[0] != 0x07230203:
        raise ValueError("not SPIR-V")
    names, deco, member_nonwritable, types, consts, variables, used = {}, {}, {}, {}, {}, [], set()
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xffff
        a = words[i + 1:i + count]
        if count == 0:
            raise ValueError("bad SPIR-V instruction")
        if op == 5:  # OpName
            names[a[0]] = struct.pack(f"<{len(a) - 1}I", *a[1:]).split(b"\0")[0].decode()
        elif op == 71:  # OpDecorate
            deco.setdefault(a[0], {})[a[1]] = a[2] if len(a) > 2 else True
        elif op == 72 and a[2] == 24:  # OpMemberDecorate NonWritable
            member_nonwritable.setdefault(a[0], set()).add(a[1])
        elif op in (25, 26, 27, 28, 29, 30, 32, 5341):
            types[a[0]] = (op, a[1:])
        elif op == 43:  # OpConstant
            consts[a[1]] = a[2]
        elif op == 59:  # OpVariable
            variables.append((a[1], a[0], a[2]))
        elif op not in (5, 6, 15, 71, 72):  # any other operand use counts as static use (names/decorations/entry point do not)
            used.update(a)
        i += count
    out = {k: [] for k in KINDS}
    for var, ptr, storage in variables:
        d = deco.get(var, {})
        if 33 not in d or var not in used:  # unused declarations (e.g. rw_output_autocomposition) are not bound
            continue
        pointee = types[ptr][1][1]
        n = 1
        while types[pointee][0] in (28, 29):
            if types[pointee][0] == 29:
                raise ValueError("runtime descriptor arrays are unsupported")
            n *= consts[types[pointee][1][1]]
            pointee = types[pointee][1][0]
        op, operands = types[pointee]
        name = names.get(var) or names.get(pointee, "")
        if storage == 0:  # UniformConstant
            if op == 25 and operands[1] != 5:
                kind = "srv_texture" if operands[5] == 1 else "uav_texture"
            elif op == 26:
                kind = "sampler"
            else:
                raise ValueError(f"unsupported resource type for {name}")
        elif storage in (2, 12) and op == 30:
            sdeco = deco.get(pointee, {})
            if storage == 2 and 2 in sdeco:
                kind = "cbv"
            else:
                readonly = 24 in d or len(member_nonwritable.get(pointee, ())) == len(operands)
                kind = "srv_buffer" if readonly else "uav_buffer"
        else:
            raise ValueError(f"unsupported storage class for {name}")
        out[kind].append((d.get(34, 0), d[33], n, name))
    for k in out:
        out[k].sort()
    return out


def compile_pass(glslc, sdk, effect, name, defines, out):
    gpu = os.path.join(sdk, "sdk/include/FidelityFX/gpu")
    src = os.path.join(sdk, f"sdk/src/backends/vk/shaders/{effect}/ffx_{effect}_{name}.glsl")
    subprocess.run([glslc, "-fshader-stage=comp", "--target-env=vulkan1.2", *defines,
                    "-I", gpu, "-I", os.path.join(gpu, effect), src, "-o", out], check=True)
    with open(out, "rb") as f:
        data = f.read()
    return list(struct.unpack(f"<{len(data) // 4}I", data))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--glslc", required=True)
    p.add_argument("--sdk", required=True)
    p.add_argument("--output", required=True)
    args = p.parse_args()
    jobs = [("fsr3upscaler", f"{PASSES[i]}_pass", BASE + [f"-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT={h}",
             f"-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={s}"]) for i in range(len(PASSES)) for s in (0, 1) for h in (0, 1)]
    jobs += [("frameinterpolation", name, FI_DEFINES) for name in FI_PASSES]
    jobs += [("opticalflow", name, OF_DEFINES) for name in OF_PASSES]
    with tempfile.TemporaryDirectory() as tmp, ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        spirv = list(pool.map(lambda n: compile_pass(args.glslc, args.sdk, *jobs[n], os.path.join(tmp, f"{n}.spv")),
                              range(len(jobs))))
    unique, blob_of = {}, []
    lines = ["// Generated by tools/ffx_fsr3_blobs.py from third_party/FidelityFX-SDK (v1.1.4, FSR 3.1.4). Do not edit.",
             "#pragma once", "#include <cstdint>", "namespace ikv::ffx_blobs {",
             "struct Reflected {std::uint32_t count;const char** names;const std::uint32_t* bindings;"
             "const std::uint32_t* counts;const std::uint32_t* sets;};",
             "struct Blob {const std::uint32_t* words;std::uint32_t bytes;Reflected " + ",".join(KINDS) + ";};"]
    for words in spirv:
        key = hashlib.sha256(struct.pack(f"<{len(words)}I", *words)).hexdigest()[:16]
        if key not in unique:
            r = reflect(words)
            unique[key] = r
            lines.append(f"inline const std::uint32_t spirv_{key}[]={{" + ",".join(map(str, words)) + "};")
            for kind, entries in r.items():
                if not entries:
                    continue
                lines.append(f"inline const char* names_{key}_{kind}[]={{" + ",".join(f'"{e[3]}"' for e in entries) + "};")
                for field, idx in (("bindings", 1), ("counts", 2), ("sets", 0)):
                    lines.append(f"inline const std::uint32_t {field}_{key}_{kind}[]={{" +
                                 ",".join(str(e[idx]) for e in entries) + "};")
        r = unique[key]
        refl = ",".join(f"{{{len(r[k])},names_{key}_{k},bindings_{key}_{k},counts_{key}_{k},sets_{key}_{k}}}"
                        if r[k] else "{0,nullptr,nullptr,nullptr,nullptr}" for k in KINDS)
        blob_of.append(f"{{spirv_{key},{len(words) * 4},{refl}}}")
    lines.append(f"// [pass][hdr | sharpen<<1]")
    lines.append(f"inline const Blob blobs[{len(PASSES)}][4]={{")
    for i in range(len(PASSES)):
        lines.append("{" + ",".join(blob_of[i * 4:i * 4 + 4]) + "},")
    lines.append("};")
    upscaler = len(PASSES) * 4
    lines.append(f"// [FfxFrameInterpolationPass]")
    lines.append(f"inline const Blob fi_blobs[{len(FI_PASSES)}]={{" + ",".join(blob_of[upscaler:upscaler + len(FI_PASSES)]) + "};")
    lines.append(f"// [FfxOpticalflowPass]")
    lines.append(f"inline const Blob of_blobs[{len(OF_PASSES)}]={{" + ",".join(blob_of[upscaler + len(FI_PASSES):]) + "};")
    lines += ["}"]
    tmp_out = args.output + ".tmp"
    with open(tmp_out, "w") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp_out, args.output)


if __name__ == "__main__":
    sys.exit(main())
