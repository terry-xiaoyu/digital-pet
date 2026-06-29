#!/usr/bin/env python3
# Copyright 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
#
# SPDX-License-Identifier: Apache-2.0
"""
SpacemitAudio demo CLI.
"""

import argparse
import importlib
import math
import os
import sys
import time
import wave
from array import array

spacemit_audio = None
AudioCapture = None
AudioPlayer = None


def load_audio_module():
    """Load spacemit_audio lazily so --help works before the extension is installed."""
    global spacemit_audio, AudioCapture, AudioPlayer
    if spacemit_audio is not None:
        return

    try:
        audio_module = importlib.import_module("spacemit_audio")
    except ImportError as exc:
        raise RuntimeError(
            f"无法导入 spacemit_audio: {exc}\n"
            "请确认当前 Python 环境已安装 spacemit_audio，且没有同名源码目录遮蔽安装包"
        ) from exc

    spacemit_audio = audio_module
    AudioCapture = audio_module.AudioCapture
    AudioPlayer = audio_module.AudioPlayer


def positive_int(value: str) -> int:
    """Parse a positive integer for argparse."""
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"{value!r} 不是有效整数") from exc
    if parsed <= 0:
        raise argparse.ArgumentTypeError("必须大于 0")
    return parsed


def list_devices() -> bool:
    """列出音频设备"""
    load_audio_module()

    print("=== 输入设备 ===")
    for idx, name in AudioCapture.list_devices():
        print(f"  [{idx}] {name}")

    print("\n=== 输出设备 ===")
    for idx, name in AudioPlayer.list_devices():
        print(f"  [{idx}] {name}")

    return True


def write_wav(filename: str, data: bytes, sample_rate: int = 16000, channels: int = 1):
    """写入 16-bit PCM WAV 文件"""
    import struct

    with open(filename, "wb") as f:
        data_size = len(data)
        byte_rate = sample_rate * channels * 2
        block_align = channels * 2

        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + data_size))
        f.write(b"WAVE")
        f.write(b"fmt ")
        f.write(struct.pack("<I", 16))  # fmt chunk size
        f.write(struct.pack("<H", 1))   # PCM
        f.write(struct.pack("<H", channels))
        f.write(struct.pack("<I", sample_rate))
        f.write(struct.pack("<I", byte_rate))
        f.write(struct.pack("<H", block_align))
        f.write(struct.pack("<H", 16))  # bits per sample
        f.write(b"data")
        f.write(struct.pack("<I", data_size))
        f.write(data)


def read_wav_pcm16(filename: str):
    """读取 16-bit PCM WAV 文件"""
    if not os.path.exists(filename):
        raise RuntimeError(f"文件不存在: {filename}")

    try:
        with wave.open(filename, "rb") as wav:
            channels = wav.getnchannels()
            sample_rate = wav.getframerate()
            sample_width = wav.getsampwidth()
            compression = wav.getcomptype()
            audio_data = wav.readframes(wav.getnframes())
    except (OSError, wave.Error) as exc:
        raise RuntimeError(f"无法读取 WAV 文件: {exc}") from exc

    if compression != "NONE" or sample_width != 2:
        raise RuntimeError("仅支持 16-bit PCM WAV")
    if sample_rate <= 0 or channels <= 0:
        raise RuntimeError("WAV 参数无效")

    return audio_data, sample_rate, channels


def resample_pcm16(data: bytes, src_rate: int, dst_rate: int, channels: int) -> bytes:
    """Resample interleaved PCM16 audio with linear interpolation."""
    if src_rate == dst_rate or not data:
        return data

    samples = array("h")
    samples.frombytes(data)
    if sys.byteorder != "little":
        samples.byteswap()

    frames = len(samples) // channels
    if frames < 2:
        return data

    ratio = dst_rate / src_rate
    out_frames = int(math.ceil(frames * ratio))
    output = array("h", [0]) * (out_frames * channels)

    for frame in range(out_frames):
        src_pos = frame / ratio
        src_idx = int(src_pos)
        frac = src_pos - src_idx

        if src_idx >= frames - 1:
            src_idx = frames - 1
            frac = 0.0

        for ch in range(channels):
            s0 = samples[src_idx * channels + ch]
            if src_idx + 1 < frames:
                s1 = samples[(src_idx + 1) * channels + ch]
            else:
                s1 = s0
            value = int(round(s0 * (1.0 - frac) + s1 * frac))
            output[frame * channels + ch] = max(-32768, min(32767, value))

    if sys.byteorder != "little":
        output.byteswap()
    return output.tobytes()


def convert_channels_pcm16(data: bytes, src_channels: int, dst_channels: int) -> bytes:
    """Convert interleaved PCM16 audio between channel counts."""
    if src_channels == dst_channels or not data:
        return data

    samples = array("h")
    samples.frombytes(data)
    if sys.byteorder != "little":
        samples.byteswap()

    frames = len(samples) // src_channels
    output = array("h", [0]) * (frames * dst_channels)

    for frame in range(frames):
        src_base = frame * src_channels
        dst_base = frame * dst_channels

        if dst_channels == 1:
            total = 0
            for ch in range(src_channels):
                total += samples[src_base + ch]
            output[dst_base] = int(round(total / src_channels))
            continue

        if src_channels == 1:
            value = samples[src_base]
            for ch in range(dst_channels):
                output[dst_base + ch] = value
            continue

        for ch in range(dst_channels):
            src_ch = ch if ch < src_channels else src_channels - 1
            output[dst_base + ch] = samples[src_base + src_ch]

    if sys.byteorder != "little":
        output.byteswap()
    return output.tobytes()


def record_audio(
    output_file: str,
    duration: int = 3,
    sample_rate: int = 16000,
    channels: int = 1,
    device: int = -1,
) -> bool:
    """录音并保存到 WAV 文件"""
    load_audio_module()

    total_bytes = 0
    chunks = []

    def on_audio(data: bytes):
        nonlocal total_bytes
        total_bytes += len(data)
        chunks.append(data)
        print(f"\r录音中... {total_bytes} bytes", end="", flush=True)

    spacemit_audio.init(
        sample_rate=sample_rate,
        channels=channels,
        chunk_size=3200,
        capture_device=device,
        player_device=-1,
    )

    print(f"录音 {duration}s -> {output_file}")
    print(f"配置: {sample_rate}Hz, {channels}ch, device={device}")

    with AudioCapture(device) as cap:
        cap.set_callback(on_audio)
        if not cap.start(sample_rate=sample_rate, channels=channels):
            print("录音设备启动失败", file=sys.stderr)
            return False

        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))

        cap.stop()

    audio_data = b"".join(chunks)
    print(f"\n完成，共 {total_bytes} bytes, {len(chunks)} chunks")

    try:
        write_wav(output_file, audio_data, sample_rate, channels)
    except OSError as exc:
        print(f"保存 WAV 失败: {exc}", file=sys.stderr)
        return False

    print(f"已保存到: {output_file}")
    return True


def play_file(
    file_path: str,
    sample_rate: int = 48000,
    device: int = -1,
    channels: int = None,
) -> bool:
    """播放 WAV 文件"""
    print(f"播放 {file_path}")

    try:
        audio_data, wav_rate, wav_channels = read_wav_pcm16(file_path)
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return False

    target_channels = channels or wav_channels
    if wav_channels != target_channels:
        print(f"转换声道: {wav_channels}ch -> {target_channels}ch")
        audio_data = convert_channels_pcm16(audio_data, wav_channels, target_channels)

    if wav_rate != sample_rate:
        print(f"重采样: {wav_rate}Hz -> {sample_rate}Hz")
        audio_data = resample_pcm16(audio_data, wav_rate, sample_rate, target_channels)

    load_audio_module()

    spacemit_audio.init(
        sample_rate=sample_rate,
        channels=target_channels,
        chunk_size=3200,
        capture_device=-1,
        player_device=device,
    )

    def write_audio(play_data: bytes, play_channels: int) -> str:
        """Return ok, start_failed, or write_failed."""
        with AudioPlayer(device) as player:
            if not player.start(sample_rate=sample_rate, channels=play_channels):
                return "start_failed"

            chunk_bytes = 4096 * play_channels * 2
            for offset in range(0, len(play_data), chunk_bytes):
                if not player.write(play_data[offset:offset + chunk_bytes]):
                    return "write_failed"

        return "ok"

    status = write_audio(audio_data, target_channels)
    if status == "start_failed" and channels is None and target_channels != 1:
        print("按 WAV 声道打开播放设备失败，自动降到 1ch 重试", file=sys.stderr)
        audio_data = convert_channels_pcm16(audio_data, target_channels, 1)
        target_channels = 1
        spacemit_audio.init(
            sample_rate=sample_rate,
            channels=target_channels,
            chunk_size=3200,
            capture_device=-1,
            player_device=device,
        )
        status = write_audio(audio_data, target_channels)

    if status == "start_failed":
        print("播放设备启动失败", file=sys.stderr)
        return False
    if status == "write_failed":
        print("播放失败", file=sys.stderr)
        return False

    print("播放完成")
    return True


def roundtrip_audio(
    output_file: str,
    duration: int = 2,
    sample_rate: int = 16000,
    channels: int = 1,
    input_device: int = -1,
    output_device: int = -1,
    play_rate: int = 48000,
    play_channels: int = None,
) -> bool:
    """先录音保存，再立即播放"""
    if not record_audio(output_file, duration, sample_rate, channels, input_device):
        return False
    return play_file(output_file, play_rate, output_device, play_channels)


def build_parser() -> argparse.ArgumentParser:
    prog = os.path.basename(sys.argv[0])
    examples = f"""Examples:
  python {prog} list
  python {prog} record OUT.wav --duration 5 --rate 16000 --channels 2 --device -1
  python {prog} play IN.wav --rate 48000 --channels 1 --device 1
  python {prog} roundtrip OUT.wav --duration 2 --rate 16000 --channels 1 --play-channels 1
"""
    parser = argparse.ArgumentParser(
        description=f"SpacemitAudio Demo\n\n{examples}",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    subparsers = parser.add_subparsers(dest="command", metavar="command")

    subparsers.add_parser("list", help="列出音频设备")

    record_parser = subparsers.add_parser(
        "record",
        help="录音到 WAV 文件",
        description=(
            "录音到 WAV 文件\n\n"
            "Example:\n"
            f"  python {prog} record OUT.wav --duration 5 --rate 16000 "
            f"--channels 2 --device -1"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    record_parser.add_argument("output", metavar="OUT.wav", help="输出 WAV 文件")
    record_parser.add_argument("--duration", "-t", type=positive_int, default=3,
                               help="录音时长，单位秒 (默认: 3)")
    record_parser.add_argument("--rate", "-s", type=positive_int, default=16000,
                               help="录音采样率 (默认: 16000)")
    record_parser.add_argument("--channels", "-c", type=positive_int, default=1,
                               help="录音声道数 (默认: 1)")
    record_parser.add_argument("--device", "-d", type=int, default=-1,
                               help="输入设备索引 (默认: -1 自动选择)")

    play_parser = subparsers.add_parser(
        "play",
        help="播放 WAV 文件",
        description=(
            "播放 WAV 文件\n\n"
            "Example:\n"
            f"  python {prog} play IN.wav --rate 48000 --channels 1 --device 1"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    play_parser.add_argument("input", metavar="IN.wav", help="输入 WAV 文件")
    play_parser.add_argument("--rate", "-s", type=positive_int, default=48000,
                             help="目标播放采样率 (默认: 48000)")
    play_parser.add_argument("--channels", "-c", type=positive_int, default=None,
                             help="目标播放声道数 (默认: 使用 WAV 声道数)")
    play_parser.add_argument("--device", "-d", type=int, default=-1,
                             help="输出设备索引 (默认: -1 自动选择)")

    roundtrip_parser = subparsers.add_parser(
        "roundtrip",
        help="录音后立即播放",
        description=(
            "录音后立即播放\n\n"
            "Example:\n"
            f"  python {prog} roundtrip OUT.wav --duration 2 --rate 16000 "
            f"--channels 1 --play-rate 48000 --play-channels 1"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    roundtrip_parser.add_argument("output", metavar="OUT.wav", help="录音输出 WAV 文件")
    roundtrip_parser.add_argument("--duration", "-t", type=positive_int, default=2,
                                  help="录音时长，单位秒 (默认: 2)")
    roundtrip_parser.add_argument("--rate", "-s", type=positive_int, default=16000,
                                  help="录音采样率 (默认: 16000)")
    roundtrip_parser.add_argument("--channels", "-c", type=positive_int, default=1,
                                  help="录音声道数 (默认: 1)")
    roundtrip_parser.add_argument("--input-device", type=int, default=-1,
                                  help="输入设备索引 (默认: -1 自动选择)")
    roundtrip_parser.add_argument("--output-device", type=int, default=-1,
                                  help="输出设备索引 (默认: -1 自动选择)")
    roundtrip_parser.add_argument("--play-rate", type=positive_int, default=48000,
                                  help="目标播放采样率 (默认: 48000)")
    roundtrip_parser.add_argument("--play-channels", type=positive_int, default=None,
                                  help="目标播放声道数 (默认: 使用录音文件声道数)")

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    try:
        if args.command == "list":
            ok = list_devices()
        elif args.command == "record":
            ok = record_audio(args.output, args.duration, args.rate, args.channels, args.device)
        elif args.command == "play":
            ok = play_file(args.input, args.rate, args.device, args.channels)
        elif args.command == "roundtrip":
            ok = roundtrip_audio(
                args.output,
                args.duration,
                args.rate,
                args.channels,
                args.input_device,
                args.output_device,
                args.play_rate,
                args.play_channels,
            )
        else:
            parser.print_help()
            return 1
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return 1

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
