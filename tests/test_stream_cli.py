#!/usr/bin/env python3
# Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
# SPDX-License-Identifier: Apache-2.0
"""Exercise stream CLI contracts without opening audio hardware."""

import pathlib
import struct
import subprocess
import sys
import tempfile
import wave


def main():
    executable = sys.argv[1]

    def run(*args, data=b"", success=True):
        result = subprocess.run(
            [executable, *args], input=data, capture_output=True, timeout=10, check=False
        )
        output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
        assert (result.returncode == 0) == success, (args, result.returncode, output)
        return output

    for flag in ("-h", "--help"):
        output = run(flag)
        assert "--stdin" in output and "--input" in output and "--list" in output
        assert "--ack-wav" in output and "--no-ack" in output and "--output" in output
        assert "--aec" in output and "--aec-bypass" in output
        assert "--holdoff-ms" in output

    for args in (
        ("--stdin", "-i", "0"),
        ("--stdin", "-t", "1"),
        ("--stdin", "-l"),
        ("-i", "nan"),
        ("-i", "-2"),
        ("-o", "nan"),
        ("-o", "-2"),
        ("-t", "-1"),
        ("-c", "0"),
        ("--holdoff-ms", "-1"),
        ("--holdoff-ms", "nan"),
        ("--holdoff-ms",),
        ("--model-dir",),
        ("--ack-wav",),
        ("--no-ack", "--ack-wav", "response.wav"),
        ("--no-ack", "--ack", "true"),
        ("--no-ack", "-o", "0"),
        ("--ack", "true", "--ack-wav", "response.wav"),
        ("--ack", "true", "-o", "0"),
        ("--stdin", "-o", "0"),
        ("--stdin", "--aec"),
        ("--aec", "--aec-bypass"),
        ("--aec", "--ack", "true"),
        ("--aec-delay-ms", "50"),
        ("--aec", "--aec-delay-ms", "501"),
        ("--aec", "--aec-delay-ms", "-1"),
        ("--aec", "--aec-record", "recording"),
        ("--aec", "--playback-channels", "0"),
        ("--unknown",),
    ):
        run(*args, success=False)

    with tempfile.TemporaryDirectory(prefix="kws-stream-cli-") as directory:
        root = pathlib.Path(directory)
        # Small valid model with a constant class-1 posterior, matching the C++ fixture topology.
        dims = (400, 6, 5, 3, 3, 2, 4, 4, 3)
        count = 800 + 6 * 400 + 6 + 5 * 6 + 5 + 4 * (3 * 5 + 3 * 3 + 3 * 2 + 5 * 3 + 5)
        count += 4 * 5 + 4 + 3 * 4 + 3
        weights = [0.0] * count
        weights[400:800] = [1.0] * 400
        weights[-3:] = [0.0, 4.0, -4.0]
        (root / "cfsmn.bin").write_bytes(
            b"KWSF" + struct.pack("<9i", *dims) + struct.pack(f"<{count}f", *weights)
        )
        (root / "keywords.txt").write_text("小进小进 1\n", encoding="utf-8")
        common = ("--stdin", "--model-dir", str(root), "--no-beam")
        output = run(*common, data=b"\0\0" * 16000)
        assert "reading 16000 Hz S16_LE PCM from stdin" in output and "[WAKE]" in output
        assert "[ACK]" not in output and "wake response:" not in output
        output = run(*common, "--holdoff-ms", "0", data=b"\0\0" * (16000 * 6))
        assert "wake cooldown: 0 ms" in output and output.count("[WAKE]") == 1
        output = run(*common, "--no-ack", data=b"\0\0" * 16000)
        assert "[WAKE]" in output and "[ACK]" not in output
        output = run(*common, "--ack", "printf ACK_COMMAND_OK", data=b"\0\0" * 16000)
        assert "[WAKE]" in output and "ACK_COMMAND_OK" in output
        run(*common, "--ack-wav", str(root / "missing.wav"), success=False)
        if len(sys.argv) > 2:
            wav_path = root / "long.wav"
            with wave.open(str(wav_path), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(16000)
                wav.writeframes(b"\0\0" * (16000 * 6))
            file_result = subprocess.run([sys.argv[2], "--model-dir", str(root), "--wav", str(wav_path),
                                          "--no-beam", "--quiet"], capture_output=True, text=True, timeout=10)
            assert file_result.returncode == 0, file_result.stdout + file_result.stderr
            assert "1 wake(s)" in file_result.stdout and "queue" not in file_result.stderr.lower()
        assert "incomplete interleaved PCM frame" in run(*common, data=b"\0", success=False)
        run(*common, "-c", "4", data=b"\0\0", success=False)
    print("kws stream CLI: OK")


if __name__ == "__main__":
    main()
