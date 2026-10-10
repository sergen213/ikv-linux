#!/usr/bin/env python3
"""Audit original audio metadata without decoding, playing or modifying assets."""
import argparse
from collections import Counter
import json
from decimal import Decimal, InvalidOperation, ROUND_CEILING
from pathlib import Path
import sys
import subprocess


LOOP_LIMIT_SECONDS = 300
FX_LIMIT_SECONDS = 10
FFPROBE_TIMEOUT_SECONDS = 20


def duration_summary(files):
    return {
        "file_count": len(files),
        "total_duration_seconds": sum(f["duration_seconds"] for f in files),
        "longest": max(files, key=lambda f: f["duration_seconds"], default=None),
        "exceeding_loop_300s": {
            "count": sum(f["duration_seconds"] > LOOP_LIMIT_SECONDS for f in files),
            "paths": [f["path"] for f in files
                      if f["duration_seconds"] > LOOP_LIMIT_SECONDS]},
        "exceeding_fx_10s": {
            "count": sum(f["duration_seconds"] > FX_LIMIT_SECONDS for f in files),
            "paths": [f["path"] for f in files
                      if f["duration_seconds"] > FX_LIMIT_SECONDS]},
    }


def probe_ogg(root, file):
    source = (root / file["path"]).resolve()
    if not source.is_relative_to(root):
        raise ValueError("inventory path escapes installation root")
    if not source.is_file():
        raise ValueError("inventory path is not a regular installation file")
    encoded_bytes = source.stat().st_size
    try:
        probe = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries",
             "stream=codec_name,sample_rate,channels:format=duration",
             "-of", "json", str(source)],
            capture_output=True, text=True, timeout=FFPROBE_TIMEOUT_SECONDS,
            check=False)
    except subprocess.TimeoutExpired as error:
        raise ValueError(f"ffprobe exceeded {FFPROBE_TIMEOUT_SECONDS}s timeout") from error
    if probe.returncode:
        raise ValueError(f"ffprobe exit {probe.returncode}: {probe.stderr.strip()}")
    try:
        metadata = json.loads(probe.stdout)
        streams = metadata["streams"]
        if len(streams) != 1:
            raise ValueError("expected exactly one audio stream")
        stream = streams[0]
        codec = stream["codec_name"]
        rate = int(stream["sample_rate"])
        channels = int(stream["channels"])
        duration_text = metadata["format"]["duration"]
        duration = Decimal(duration_text)
        if not isinstance(codec, str) or not codec or rate <= 0 or channels <= 0:
            raise ValueError("missing or invalid codec/rate/channels")
        if not duration.is_finite() or duration < 0:
            raise ValueError("duration must be finite and nonnegative")
    except (KeyError, TypeError, ValueError, InvalidOperation) as error:
        raise ValueError(f"invalid ffprobe JSON metadata: {error}") from error
    frames = int((duration * 48000).to_integral_value(rounding=ROUND_CEILING))
    return {"path": file["path"], "inventory_bytes": file["bytes"],
            "encoded_bytes": encoded_bytes,
            "codec_name": codec, "sample_rate": rate, "channels": channels,
            "duration_seconds": float(duration), "ffprobe_duration_text": duration_text,
            "estimated_decoded_48000_stereo_s16le_bytes": frames * 4}


def ogg_metadata(ogg, root):
    files, errors = [], []
    for file in ogg:
        try:
            files.append(probe_ogg(root, file))
        except (OSError, ValueError) as error:
            errors.append({"path": file["path"], "error": str(error)})
    summary = duration_summary(files)
    summary.update({
        "codec_counts": dict(sorted(Counter(f["codec_name"] for f in files).items())),
        "sample_rate_counts": dict(sorted(Counter(
            str(f["sample_rate"]) for f in files).items())),
        "channel_counts": dict(sorted(Counter(str(f["channels"]) for f in files).items())),
        "total_estimated_decoded_48000_stereo_s16le_bytes": sum(
            f["estimated_decoded_48000_stereo_s16le_bytes"] for f in files),
        "largest_encoded_ogg": max(files, key=lambda f: f["encoded_bytes"], default=None),
        "total_encoded_bytes": sum(f["encoded_bytes"] for f in files),
        "exceeding_encoded_32MiB": {
            "count": sum(f["encoded_bytes"] > 32 * 1024 * 1024 for f in files),
            "paths": [f["path"] for f in files
                      if f["encoded_bytes"] > 32 * 1024 * 1024]},
        "exceeding_estimated_decoded_64MiB": {
            "count": sum(f["estimated_decoded_48000_stereo_s16le_bytes"] >
                         64 * 1024 * 1024 for f in files),
            "paths": [f["path"] for f in files
                      if f["estimated_decoded_48000_stereo_s16le_bytes"] >
                      64 * 1024 * 1024]},
    })
    return {
        "metadata_source": "actual read-only ffprobe of installation files; no decoding or playback",
        "encoded_bytes_source": "actual installation file stat; inventory_bytes are saved lengths, neither rehashes",
        "installation_root": root.as_posix(),
        "ffprobe_arguments": ["-v", "error", "-show_entries",
                             "stream=codec_name,sample_rate,channels:format=duration",
                             "-of", "json"],
        "timeout_seconds_per_file": FFPROBE_TIMEOUT_SECONDS,
        "inventory_file_count": len(ogg),
        "probed_file_count": len(files),
        "error_count": len(errors),
        "errors": errors,
        "decoded_byte_estimate": {
            "formula": "ceil(ffprobe format duration * 48000) * 4",
            "encoding": "PCM_S16LE", "sample_rate": 48000, "channels": 2,
            "status": "estimate from reported container duration, not measured decoded length"},
        "summary": summary,
        "longestOgg": summary["longest"],
        "files": files,
    }

def audio_metadata(inventory, root=None):
    raw = [f for f in inventory["files"]
           if f["path"].startswith("base/audio/") and f["path"].lower().endswith(".raw")]
    ogg = [f for f in inventory["files"]
           if f["path"].startswith("base/audio/") and f["path"].lower().endswith(".ogg")]
    grids = [f for f in inventory["files"] if f["path"].lower().endswith(".raw")
             and not f["path"].startswith("base/audio/")]
    rejected = [f for f in raw if not f["bytes"] or f["bytes"] % 2
                or f["bytes"] > 32 * 1024 * 1024]
    families = Counter(f["path"].split("/")[2] for f in raw)
    valid = [f for f in raw if f not in rejected]
    raw_durations = [
        {"path": f["path"], "inventory_bytes": f["bytes"],
         "duration_seconds": f["bytes"] / 88200,
         "complete_frames": f["bytes"] // 2,
         "trailing_bytes": f["bytes"] % 2,
         "frame_status": "incomplete_frame" if f["bytes"] % 2 else "complete_frames",
         "native_raw_accepted": f not in rejected}
        for f in raw]
    result = {
        "schema": "ikv-original-audio-format-1",
        "metadata_source": "saved read-only asset inventory; hashes not remeasured by this tool",
        "proof": {
            "unpacked_pe_sha256": "44ffa8389a9f6e4c35841103ad4f40a90513e4deb518ab5605b0a1ebfae0be2d",
            "image_base": "0x00400000",
            "selector": "0x004580f0 raw string0x005937b0 ->0x00457770 ->0x004534f0 vtable0x00593580 ->0x00453580",
            "format_stores": "0x004535db-0x00453600",
            "buffer_create": "0x00453753 IDirectSound::CreateSoundBuffer vtable+0x0c",
            "buffer_lock": "0x004537a3 IDirectSoundBuffer::Lock vtable+0x2c flags2 (entire buffer)",
            "file_size": "0x00452d90 fseek/fgetpos; call0x0045365a",
            "payload_read": "0x00452e60 fread(dst,1,count); call0x004537be",
            "buffer_unlock": "0x00453803 IDirectSoundBuffer::Unlock vtable+0x4c",
        },
        "raw_format": {"encoding": "PCM_S16LE", "wave_format_tag": 1,
                       "sample_rate": 44100, "channels": 1, "bits_per_sample": 16,
                       "block_align": 2, "average_bytes_per_second": 88200,
                       "header_bytes": 0, "compression": "none", "interleaving": "mono"},
        "native_policy": "Reject empty, odd-byte, over32MiB or non-audio-root RAW; do not pad/drop samples. This is not recovered original malformed-input behavior.",
        "counts": {"raw_total": len(raw) + len(grids), "audio_raw": len(raw),
                   "audio_ogg": len(ogg), "non_audio_raw": len(grids),
                   "native_raw_accepted": len(valid), "native_raw_rejected": len(rejected)},
        "raw_families": dict(sorted(families.items())),
        "raw_audio_bytes": sum(f["bytes"] for f in raw),
        "accepted_frames": sum(f["bytes"] // 2 for f in valid),
        "rejected_original_files": rejected,
        "non_audio_raw_files": [{"path": f["path"], "bytes": f["bytes"]} for f in grids],
        "raw_duration_audit": {
            "metadata_source": "saved inventory byte lengths and statically recovered PCM_S16LE mono44100 descriptor; no current RAW reads",
            "formula": "inventory bytes / 88200",
            "incomplete_frame_note": "Odd lengths have nominal byte-derived durations only, not playable complete-frame durations; native rejects them without padding or truncation.",
            "non_audio_grid_note": "The 24 non-audio RAW selection grids are excluded; no PCM interpretation.",
            "summary": duration_summary(raw_durations),
            "files": raw_durations},
        "native_duration_policy": {
            "loop_limit_seconds": LOOP_LIMIT_SECONDS,
            "fx_limit_seconds": FX_LIMIT_SECONDS,
            "decoded_format": "PCM_S16LE stereo48000",
            "pcm_cache_bytes": 64 * 1024 * 1024,
            "significance": "Independent native decoder/cache limits, not canonical original asset constraints. Both thresholds are compared to every audio resource; paths do not prove loop/FX assignment."},
        "examples": [dict(f, frames=f["bytes"] // 2,
                          seconds=(f["bytes"] // 2) / 44100)
                     for f in valid if f["path"] in {
                         "base/audio/ambient/hakirpropaganda.raw",
                         "base/audio/ambient/break_desk.raw", "base/audio/ui/kritik.raw",
                         "base/audio/skills/atescemberi.raw"}],
        "remaining_unknowns": [
            "Original malformed odd-byte DirectSound acceptance/failure behavior was not exercised.",
            "Original exact asset-selection/spatial sound gain/pitch semantics are outside this fixed-format recovery.",
            "Zone musicNN.raw/soundNN.raw select resources and must never be PCM-decoded."],
    }
    if root is not None:
        result["ogg_duration_audit"] = ogg_metadata(ogg, root.resolve())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inventory", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--root", type=Path,
                        help="installation root for read-only ffprobe Ogg metadata")
    args = parser.parse_args()
    if args.root and not args.root.is_dir():
        raise ValueError("installation root must be an existing directory")
    if args.output:
        destination = args.output.resolve()
        if destination == args.inventory.resolve():
            raise ValueError("output must not replace the input inventory")
        if args.root and destination.is_relative_to(args.root.resolve()):
            raise ValueError("output must not modify the original installation")
    result = audio_metadata(json.loads(args.inventory.read_text(encoding="utf-8")), args.root)
    result["inventory_path"] = args.inventory.as_posix()
    text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        # Audit output contains metadata only, never original audio payloads.
        args.output.write_text(text, encoding="utf-8")
        print(json.dumps(result["counts"], sort_keys=True))
        if args.root:
            audit = result["ogg_duration_audit"]
            print(json.dumps({key: audit[key] for key in
                              ("probed_file_count", "error_count", "longestOgg")},
                             ensure_ascii=False, sort_keys=True))
    else:
        sys.stdout.write(text)
    if args.root and result["ogg_duration_audit"]["error_count"]:
        raise ValueError("Ogg metadata audit contains ffprobe errors; see output details")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        sys.exit(str(error))
