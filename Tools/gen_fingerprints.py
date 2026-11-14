#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_fingerprints.py —— 把任意音频语料压成 EWG1 多模板指纹包。

算法与运行期 C++（Source/Dsp/BandAnalyzer.h + FingerprintGate.h）逐行对齐，
也就是 everplus EverplusApo.dll 里的真实分析链：

    20 ms 帧（48 kHz 下 960 样本）
      → 6:1 盒式抽取到 8 kHz（160 点）
      → Tukey 窗 α=0.25
      → 补零到 256 点 FFT（31.25 Hz/bin）
      → 16 带盒式重叠积分（100·32^(b/16)，100–3200 Hz）
      → log10(max(1e-30))
      → 50 帧（1 秒）滑动均值
      → 减去 16 带均值（去电平）
      → L2 归一化
      → 写入模板包的一行

用法：
    # 三模板（推荐）
    python3 gen_fingerprints.py -o fingerprints.ewg \
        --template wind=wind10-reference.wav \
        --template rumble=npp-reference.wav \
        --corpus voice=./voice-corpus

    # 单个文件当一套模板
    python3 gen_fingerprints.py -o voice.ewg --template voice=sample.wav

    # 指定每套模板最多保留多少行（默认 2000，越多越慢、越易误命中）
    python3 gen_fingerprints.py ... --max-rows 1500
"""

from __future__ import annotations

import argparse
import glob
import os
import struct
import sys
from typing import Iterable

import numpy as np

BANDS = 16
FFT_SIZE = 256
FRAME_SECONDS = 0.02
CONTEXT_FRAMES = 50
TARGET_RATE = 48000
DECIM = 6
DOWN_LEN = 160


# ----------------------------------------------------------------------------- 音频读取
def read_wav(path: str) -> tuple[np.ndarray, int]:
    """读 WAV，返回 (单声道 float64, 采样率)。支持 16/24/32-bit PCM 与 float32。"""
    with open(path, "rb") as fh:
        data = fh.read()

    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise RuntimeError(f"{path}: 不是 WAV")

    pos = 12
    fmt = None
    while pos + 8 <= len(data):
        cid = data[pos : pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = pos + 8
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", data, body)
        elif cid == b"data":
            if fmt is None:
                raise RuntimeError(f"{path}: data 在 fmt 之前")
            _, ch, sr, _, block_align, bits = fmt
            raw = data[body : body + size]
            if bits == 16:
                a = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
            elif bits == 24:
                b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
                v = (
                    b[:, 0].astype(np.int32)
                    | (b[:, 1].astype(np.int32) << 8)
                    | (b[:, 2].astype(np.int32) << 16)
                )
                v = np.where(v & 0x800000, v - 0x1000000, v)
                a = v.astype(np.float64) / 8388608.0
            elif bits == 32 and fmt[0] == 3:
                a = np.frombuffer(raw, dtype="<f4").astype(np.float64)
            elif bits == 32:
                a = np.frombuffer(raw, dtype="<i4").astype(np.float64) / 2147483648.0
            else:
                raise RuntimeError(f"{path}: 不支持 {bits} bit")

            a = a.reshape(-1, ch)
            return a.mean(axis=1), sr
        pos = body + size + (size & 1)

    raise RuntimeError(f"{path}: 找不到 data 块")


def resample_linear(x: np.ndarray, src: int, dst: int = TARGET_RATE) -> np.ndarray:
    """线性重采样。目标频带只到 3.2 kHz，线性插值足够。"""
    if src == dst:
        return x
    n_out = int(round(len(x) * dst / src))
    if n_out <= 1:
        return x
    t = np.arange(n_out, dtype=np.float64) * (src / dst)
    return np.interp(t, np.arange(len(x), dtype=np.float64), x)


# ----------------------------------------------------------------------------- 分析链
def tukey(n: int = DOWN_LEN, alpha: float = 0.25) -> np.ndarray:
    """与 everplus 0x180045B60 完全一致的 Tukey 窗。"""
    w = np.ones(n, dtype=np.float64)
    denom = max(1, n - 1)
    for i in range(n):
        t = min(i, n - 1 - i) / denom
        w[i] = 1.0 if t >= 0.125 else 0.5 * (1.0 + np.cos(np.pi * (8.0 * t - 1.0)))
    return w


def band_matrix() -> np.ndarray:
    """16×129 的盒式重叠权重（bin 宽度 31.25 Hz）。"""
    bw = 8000.0 / FFT_SIZE
    m = np.zeros((BANDS, FFT_SIZE // 2 + 1), dtype=np.float64)
    for b in range(BANDS):
        lo = 100.0 * 32.0 ** (b / BANDS)
        hi = 100.0 * 32.0 ** ((b + 1) / BANDS)
        for k in range(FFT_SIZE // 2 + 1):
            bin_lo = k * bw
            bin_hi = bin_lo + bw
            overlap = min(bin_hi, hi) - max(bin_lo, lo)
            if overlap > 0:
                m[b, k] = overlap / bw
    return m


def features(x: np.ndarray) -> np.ndarray:
    """把单声道 48 kHz 波形压成 (n, 16) 的 log10 带功率。"""
    win = tukey()
    mat = band_matrix()
    n_frames = len(x) // 960
    out = np.zeros((n_frames, BANDS), dtype=np.float64)
    for i in range(n_frames):
        seg = x[i * 960 : (i + 1) * 960].reshape(DOWN_LEN, DECIM).mean(axis=1)
        spec = np.zeros(FFT_SIZE, dtype=np.float64)
        spec[:DOWN_LEN] = seg * win
        power = np.abs(np.fft.rfft(spec)) ** 2
        out[i] = mat @ power
    return np.log10(np.maximum(out, 1e-30))


def smooth(x: np.ndarray, k: int = CONTEXT_FRAMES) -> np.ndarray:
    """50 帧滑动均值（1 秒），与运行期的 bandSum/50 一致。"""
    c = np.cumsum(np.vstack([np.zeros((1, x.shape[1])), x]), axis=0)
    out = np.empty_like(x)
    for i in range(len(x)):
        lo = max(0, i - k + 1)
        out[i] = (c[i + 1] - c[lo]) / (i + 1 - lo)
    return out


def to_rows(x: np.ndarray, max_rows: int, hop: int = 5) -> np.ndarray:
    """波形 → 模板行：1 秒均值 → 去带均值 → L2 归一化 → 丢弃退化行。"""
    if len(x) < 960 * (CONTEXT_FRAMES + 1):
        return np.zeros((0, BANDS), dtype=np.float32)

    f = smooth(features(x))
    # 前 49 帧窗口未填满，跳过；之后每 hop 帧取一行
    f = f[CONTEXT_FRAMES - 1 :]
    rows = f[::hop]
    if len(rows) == 0:
        return np.zeros((0, BANDS), dtype=np.float32)

    rows = rows - rows.mean(axis=1, keepdims=True)
    norm = np.linalg.norm(rows, axis=1)
    med = float(np.median(norm)) if len(norm) else 0.0
    keep = norm > max(1e-6, 0.25 * med)
    rows = rows[keep] / norm[keep, None]

    if len(rows) > max_rows:
        idx = np.linspace(0, len(rows) - 1, max_rows).astype(int)
        rows = rows[idx]
    return rows.astype(np.float32)


# ----------------------------------------------------------------------------- 语料装载
def collect(path: str) -> list[str]:
    if os.path.isdir(path):
        files: list[str] = []
        for ext in ("*.wav", "*.WAV"):
            files.extend(glob.glob(os.path.join(path, ext)))
        return sorted(files)
    return [path]


def build_rows(paths: Iterable[str], max_rows: int) -> np.ndarray:
    chunks = []
    for p in paths:
        try:
            x, sr = read_wav(p)
        except Exception as exc:  # noqa: BLE001
            print(f"  跳过 {p}: {exc}", file=sys.stderr)
            continue
        x = resample_linear(x, sr)
        rows = to_rows(x, max_rows=max_rows)
        if len(rows) == 0:
            print(f"  跳过 {p}: 时长不足 1 秒", file=sys.stderr)
            continue
        chunks.append(rows)
        print(f"  {os.path.basename(p)}: {len(rows)} 行（{len(x)/TARGET_RATE:.1f}s）")

    if not chunks:
        return np.zeros((0, BANDS), dtype=np.float32)
    return np.vstack(chunks)


# ----------------------------------------------------------------------------- 写出
def write_ewg(path: str, templates: dict[str, np.ndarray]) -> None:
    with open(path, "wb") as fh:
        fh.write(b"EWG1")
        fh.write(struct.pack("<III", 1, BANDS, len(templates)))
        for label, rows in templates.items():
            lb = label.encode("utf-8")
            fh.write(struct.pack("<I", len(lb)))
            fh.write(lb)
            fh.write(struct.pack("<I", len(rows)))
            fh.write(rows.astype("<f4").tobytes())


def main() -> int:
    ap = argparse.ArgumentParser(description="生成 EWG1 多模板指纹包")
    ap.add_argument("--template", action="append", default=[], metavar="LABEL=PATH",
                    help="一套模板：标签=音频文件或目录，可重复")
    ap.add_argument("--corpus", action="append", default=[], metavar="LABEL=DIR",
                    help="同 --template，语义上表示整个语料目录")
    ap.add_argument("-o", "--out", required=True, help="输出 .ewg 路径")
    ap.add_argument("--max-rows", type=int, default=2000, help="每套模板最多保留行数")
    args = ap.parse_args()

    specs = args.template + args.corpus
    if not specs:
        ap.error("至少给一个 --template 或 --corpus")

    templates: dict[str, np.ndarray] = {}
    for spec in specs:
        if "=" not in spec:
            ap.error(f"格式应为 标签=路径，收到 {spec!r}")
        label, path = spec.split("=", 1)
        label = label.strip()
        if not label:
            ap.error(f"{spec!r} 标签为空")

        files = collect(path)
        if not files:
            print(f"警告：{label} 在 {path} 下没找到 wav", file=sys.stderr)
            continue

        print(f"[{label}] {len(files)} 个文件")
        rows = build_rows(files, args.max_rows)
        if len(rows):
            templates[label] = rows
            print(f"[{label}] 合计 {len(rows)} 行")

    if not templates:
        print("没有生成任何模板", file=sys.stderr)
        return 1

    write_ewg(args.out, templates)
    total = sum(len(v) for v in templates.values())
    print(f"\n写出 {args.out}: {len(templates)} 套模板 / {total} 行 / {os.path.getsize(args.out)/1024:.0f} KB")
    for label, rows in templates.items():
        print(f"  {label:8s} {len(rows):5d} 行")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
