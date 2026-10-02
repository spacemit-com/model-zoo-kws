#!/usr/bin/env python3
"""Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
SPDX-License-Identifier: Apache-2.0

Run against the built extension: PYTHONPATH=build/python python tests/test_python_bindings.py
"""
import gc
import struct
import tempfile
import unittest
from pathlib import Path

import _spacemit_kws as kws
import numpy as np


class BindingTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="kws-python-")
        model_dir = Path(self.directory.name)
        weights = np.zeros(1214, dtype="<f4")
        weights[400:800] = 1
        weights[-1] = 4  # Always predict token 1.
        (model_dir / "cfsmn.bin").write_bytes(
            b"KWSF" + struct.pack("<9i", 400, 1, 1, 1, 1, 1, 1, 1, 2) + weights.tobytes()
        )
        (model_dir / "keywords.txt").write_text("test 1\n")
        self.config = kws.KwsConfig().with_model_dir(str(model_dir))

    def tearDown(self):
        self.directory.cleanup()

    def test_callback_lifetime_reentrancy_and_eos(self):
        engine = kws.KwsEngine(self.config)
        self.assertTrue(engine.initialized, engine.last_error)
        events = []

        class Handler(kws.KwsCallback):
            def on_open(self):
                events.append("open")

            def on_event(self, result):
                events.append("event")
                engine.set_threshold(0.5)
                engine.stop()

            def on_wake_word(self, keyword, score, timestamp_ms):
                events.append("wake")

            def on_complete(self):
                events.append("complete")

            def on_close(self):
                events.append("close")

        engine.set_callback(Handler())  # No other reference to the Python subclass.
        gc.collect()
        self.assertTrue(engine.start())
        engine.send_audio_frame(np.zeros(1600, dtype=np.float32))
        engine.stop()  # Wait for inference and dispatcher; releases the GIL.
        self.assertFalse(engine.streaming)
        self.assertEqual(events[0], "open")
        self.assertIn("wake", events)
        self.assertEqual(events[-2:], ["complete", "close"])
        self.assertEqual(events.count("close"), 1)
        result = engine.detect(np.zeros(720, dtype=np.float32))
        self.assertTrue(result.success and result.is_wake_word)  # Entire result is emitted at EOS.

    def test_destructor_releases_gil(self):
        events = []
        class Handler(kws.KwsCallback):
            def on_open(self):
                events.append("open")
        for _ in range(20):
            engine = kws.KwsEngine(self.config)
            engine.set_callback(Handler())
            self.assertTrue(engine.start())
            self.assertEqual(engine.send_audio_frame(np.zeros(160, dtype=np.float32)), kws.KwsAudioStatus.ACCEPTED)
            del engine
            gc.collect()

    def test_callback_exception_does_not_terminate_process(self):
        class Handler(kws.KwsCallback):
            def on_event(self, result):
                raise RuntimeError("injected callback error")
        engine = kws.KwsEngine(self.config)
        engine.set_callback(Handler())
        self.assertTrue(engine.start())
        engine.send_audio_frame(np.zeros(1600, dtype=np.float32))
        engine.stop()
        self.assertFalse(engine.streaming)
        self.assertIn("Callback threw", engine.last_error)

    def test_shapes_and_invalid_values(self):
        engine = kws.KwsEngine(self.config.with_channels(4))
        self.assertTrue(engine.initialized, engine.last_error)
        for audio in (np.zeros((4, 160)), np.zeros(641), np.zeros((2, 2, 4))):
            with self.assertRaises(ValueError):
                engine.detect(audio)
        self.assertTrue(engine.detect(np.zeros((720, 4))).success)
        self.assertFalse(engine.detect(np.full((720, 4), np.nan)).success)
        engine.set_threshold(float("nan"))
        self.assertAlmostEqual(engine.get_config().threshold, 0.3)
        self.assertTrue(engine.last_error)


if __name__ == "__main__":
    unittest.main()
