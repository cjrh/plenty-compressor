"""Exercise the native LV2 interface; this is not real-time certification."""
import ctypes as c
import math
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import threading
import subprocess
import sys
import textwrap
import unittest


class Descriptor(c.Structure):
    _fields_ = [("uri", c.c_char_p), ("instantiate", c.c_void_p),
                ("connect", c.c_void_p), ("activate", c.c_void_p),
                ("run", c.c_void_p), ("deactivate", c.c_void_p),
                ("cleanup", c.c_void_p), ("extension", c.c_void_p)]


LIB = c.CDLL(str(Path("build/compressor.lv2/plugin.so").resolve()))
LIB.lv2_descriptor.argtypes = [c.c_uint32]
LIB.lv2_descriptor.restype = c.POINTER(Descriptor)
DESCRIPTOR = LIB.lv2_descriptor(0)
assert DESCRIPTOR
INSTANTIATE = c.CFUNCTYPE(c.c_void_p, c.POINTER(Descriptor), c.c_double,
                        c.c_char_p, c.c_void_p)(DESCRIPTOR.contents.instantiate)
CONNECT = c.CFUNCTYPE(None, c.c_void_p, c.c_uint32, c.c_void_p)(DESCRIPTOR.contents.connect)
ACTIVATE = c.CFUNCTYPE(None, c.c_void_p)(DESCRIPTOR.contents.activate)
RUN = c.CFUNCTYPE(None, c.c_void_p, c.c_uint32)(DESCRIPTOR.contents.run)
CLEANUP = c.CFUNCTYPE(None, c.c_void_p)(DESCRIPTOR.contents.cleanup)
# LV2 requires a non-null, null-terminated feature array even with no features.
FEATURES = (c.c_void_p * 1)(None)


def instantiate(rate=48000):
    return INSTANTIATE(DESCRIPTOR, rate, b"", FEATURES)


class Instance:
    def __init__(self, size=4096, rate=48000):
        self.handle = instantiate(rate)
        assert self.handle
        self.size = size
        self.left = (c.c_float * size)(*([1.0] * size))
        self.right = (c.c_float * size)(*([0.5] * size))
        self.out_left = (c.c_float * size)()
        self.out_right = (c.c_float * size)()
        self.controls = [c.c_float(-20), c.c_float(4), c.c_float(1), c.c_float(100)]
        self.ports = [self.left, self.right, self.out_left, self.out_right, *self.controls]
        for port, value in enumerate(self.ports):
            self.connect(port, value)
        ACTIVATE(self.handle)

    def connect(self, port, value):
        CONNECT(self.handle, port, c.cast(c.pointer(value), c.c_void_p))

    def run(self, count=None):
        RUN(self.handle, self.size if count is None else count)

    def close(self):
        if self.handle:
            CLEANUP(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class PluginTests(unittest.TestCase):
    def test_descriptor(self):
        self.assertEqual(DESCRIPTOR.contents.uri, b"https://example.org/plenty-compressor")
        self.assertFalse(LIB.lv2_descriptor(1))
        self.assertFalse(LIB.lv2_descriptor(0xFFFFFFFF))
        self.assertEqual(c.addressof(DESCRIPTOR.contents), c.addressof(LIB.lv2_descriptor(0).contents))

    def test_concurrent_first_discovery(self):
        # A fresh process ensures the once control has not already run.
        probe = textwrap.dedent("""\
            import ctypes as c
            from concurrent.futures import ThreadPoolExecutor
            import threading
            import sys
            lib = c.CDLL(sys.argv[1])
            lib.lv2_descriptor.argtypes = [c.c_uint32]
            lib.lv2_descriptor.restype = c.c_void_p
            barrier = threading.Barrier(8)
            def discover(_):
                barrier.wait(timeout=30)
                pointer = lib.lv2_descriptor(0)
                assert pointer
                assert c.c_char_p.from_address(pointer).value == b'https://example.org/plenty-compressor'
                return pointer
            with ThreadPoolExecutor(max_workers=8) as workers:
                addresses = list(workers.map(discover, range(8)))
            assert len(set(addresses)) == 1
            """)
        subprocess.run([sys.executable, "-c", probe, str(Path(LIB._name))], check=True, timeout=60)

    def test_invalid_rate(self):
        for rate in [0, -1, math.inf, -math.inf, math.nan]:
            self.assertFalse(instantiate(rate))

    def test_curve_and_linked_channels(self):
        with Instance() as plugin:
            plugin.run()
            expected = math.pow(10, -15 / 20)
            self.assertAlmostEqual(plugin.out_left[-1], expected, places=5)
            self.assertAlmostEqual(plugin.out_right[-1], 0.5 * expected, places=5)

    def test_unity_ratio(self):
        with Instance() as plugin:
            plugin.controls[1].value = 1
            plugin.run()
            self.assertEqual(list(plugin.out_left), list(plugin.left))
            self.assertEqual(list(plugin.out_right), list(plugin.right))

    def test_in_place(self):
        with Instance() as reference, Instance() as plugin:
            reference.run()
            plugin.connect(2, plugin.left)
            plugin.connect(3, plugin.right)
            plugin.run()
            self.assertEqual(list(plugin.left), list(reference.out_left))
            self.assertEqual(list(plugin.right), list(reference.out_right))

    def test_cross_channel_aliases(self):
        # Both inputs must be read before either output is written.
        with Instance() as reference, Instance() as plugin:
            reference.run()
            plugin.connect(2, plugin.right)
            plugin.connect(3, plugin.left)
            plugin.run()
            self.assertEqual(list(plugin.right), list(reference.out_left))
            self.assertEqual(list(plugin.left), list(reference.out_right))

    def test_all_audio_ports_alias(self):
        with Instance() as reference, Instance() as plugin:
            reference.connect(1, reference.left)
            reference.run()
            for port in [1, 2, 3]:
                plugin.connect(port, plugin.left)
            plugin.run()
            self.assertEqual(list(plugin.left), list(reference.out_left))

    def test_partial_overlap_uses_forward_memory_order(self):
        with Instance(size=32) as plugin:
            plugin.controls[1].value = 1
            storage = (c.c_float * 33)(*range(2, 35))
            source = (c.c_float * 32).from_buffer(storage)
            destination = (c.c_float * 32).from_buffer(storage, c.sizeof(c.c_float))
            plugin.connect(0, source)
            plugin.connect(2, destination)
            plugin.run()
            # Each store overwrites the input for the following sample.
            self.assertEqual(list(storage), [2.0] * 33)

    def test_nonfinite_controls_use_defaults(self):
        with Instance() as reference, Instance() as plugin:
            for control, default in zip(reference.controls, [-18, 4, 10, 100]):
                control.value = default
            for control, invalid in zip(plugin.controls, [math.nan, math.inf, -math.inf, math.nan]):
                control.value = invalid
            reference.run()
            plugin.run()
            self.assertEqual(list(plugin.out_left), list(reference.out_left))
            self.assertEqual(list(plugin.out_right), list(reference.out_right))

    def test_zero_and_variable_blocks(self):
        with Instance() as plugin:
            plugin.controls[1].value = 1
            for count in [0, 1, 17, 127, 2048, 4096]:
                for i in range(plugin.size):
                    plugin.out_left[i] = -9
                plugin.run(count)
                self.assertEqual(list(plugin.out_left[:count]), [1.0] * count)
                self.assertEqual(list(plugin.out_left[count:]), [-9.0] * (plugin.size - count))

    def test_reconnect(self):
        with Instance() as plugin:
            plugin.run()
            replacement = (c.c_float * plugin.size)()
            plugin.connect(0, replacement)
            plugin.run()
            self.assertEqual(list(plugin.out_left), [0.0] * plugin.size)

    def test_nonfinite_samples(self):
        with Instance() as plugin:
            plugin.controls[1].value = 1
            for i, value in enumerate([math.nan, math.inf, -math.inf]):
                plugin.left[i] = value
                plugin.right[i] = value
            plugin.run(3)
            self.assertEqual(list(plugin.out_left[:3]), [0.0] * 3)
            self.assertEqual(list(plugin.out_right[:3]), [0.0] * 3)

    def test_activation_resets_envelope(self):
        with Instance() as plugin:
            plugin.run()
            ACTIVATE(plugin.handle)
            plugin.left[0] = 0
            plugin.right[0] = 0
            plugin.run(1)
            self.assertEqual(plugin.out_left[0], 0)
            self.assertEqual(plugin.out_right[0], 0)

    def test_thread_handoff(self):
        # Creation, processing and destruction use different caller threads;
        # waiting on each future provides an ordered, nonconcurrent handoff.
        with ThreadPoolExecutor(max_workers=1) as processor, ThreadPoolExecutor(max_workers=1) as disposer:
            plugin = Instance()
            try:
                processor.submit(plugin.run).result()
                self.assertAlmostEqual(plugin.out_left[-1], math.pow(10, -15 / 20), places=5)
            finally:
                disposer.submit(plugin.close).result()

    def test_concurrent_instances(self):
        barrier = threading.Barrier(4)

        def process(_):
            with Instance() as plugin:
                barrier.wait(timeout=30)
                for _ in range(8):
                    plugin.run()
                return plugin.out_left[-1], plugin.out_right[-1]

        with ThreadPoolExecutor(max_workers=4) as workers:
            results = list(workers.map(process, range(4)))
        for left, right in results:
            self.assertAlmostEqual(left, math.pow(10, -15 / 20), places=5)
            self.assertAlmostEqual(right, left * 0.5, places=5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
