#!/usr/bin/env python3
"""
Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
SPDX-License-Identifier: Apache-2.0

KWS Python 示例：回放一个 16 kHz wav，打印唤醒事件。
4 通道文件（SPV 复合设备录音）默认走 3 麦波束。

    python kws_file_demo.py --wav play-3m.wav
"""
import argparse
import time
import wave

import numpy as np
import spacemit_kws


def read_wav(path):
    with wave.open(path) as w:
        if w.getframerate() != 16000:
            raise SystemExit(f"{path}: {w.getframerate()} Hz, the model needs 16000")
        if w.getsampwidth() != 2 or w.getcomptype() != "NONE":
            raise SystemExit(f"{path}: expected PCM16 WAV")
        channels = w.getnchannels()
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
        if pcm.size != w.getnframes() * channels:
            raise SystemExit(f"{path}: truncated PCM data")
    return pcm.astype(np.float32) / 32768.0, channels


class Printer(spacemit_kws.KwsCallback):
    def __init__(self, quiet):
        super().__init__()
        self.quiet = quiet
        self.wakes = 0

    def on_wake_word(self, keyword, score, timestamp_ms):
        self.wakes += 1
        print(f"[WAKE] {keyword}  score {score:.3f}  (t={timestamp_ms / 1000:.1f}s)", flush=True)

    def on_event(self, result):
        if not self.quiet and not result.is_wake_word and result.score > 0.02:
            print(f"       score {result.score:.3f}  (t={result.timestamp_ms / 1000:.1f}s)")

    def on_error(self, message):
        print("error:", message)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wav", required=True)
    ap.add_argument("--model-dir", default="")
    ap.add_argument("--keyword", default="")
    ap.add_argument("--thr", type=float, default=0.3)
    ap.add_argument("--no-beam", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    audio, channels = read_wav(args.wav)
    beam = channels >= 4 and not args.no_beam

    config = spacemit_kws.KwsConfig.preset("xiaojin-4mic" if beam else "xiaojin")
    config.num_channels = channels
    config.use_beamforming = beam
    config.threshold = args.thr
    if args.model_dir:
        config.model_dir = args.model_dir
    if args.keyword:
        config = config.with_keyword(args.keyword)

    engine = spacemit_kws.KwsEngine(config)
    if not engine.initialized:
        raise SystemExit(f"KWS engine not initialized: {engine.last_error}")

    callback = Printer(args.quiet)
    engine.set_callback(callback)
    if not engine.start():
        raise SystemExit(engine.last_error)
    print(f"engine  : {engine.engine_name}, keywords: {' '.join(engine.get_keywords())}, "
          f"thr {config.threshold:.2f}, lookahead {engine.lookahead_ms} ms, "
          f"beam {'on' if beam else 'off'}")

    hop = 160 * channels
    for i in range(0, len(audio), hop):
        # Offline feeding may wait; never wait in a microphone callback.
        while engine.streaming and engine.get_stream_stats().queued_blocks == 128:
            time.sleep(0.001)
        if engine.send_audio_frame(audio[i:i + hop]) != spacemit_kws.KwsAudioStatus.ACCEPTED:
            raise SystemExit(engine.last_error)
        if not engine.streaming:
            raise SystemExit(engine.last_error)
    engine.stop()
    if engine.last_error:
        raise SystemExit(engine.last_error)

    seconds = len(audio) / channels / 16000
    print(f"done    | {callback.wakes} wake(s) over {seconds:.1f}s audio")


if __name__ == "__main__":
    main()
