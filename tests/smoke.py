"""Single-threaded LV2 ABI smoke test, not a DAW or realtime certification."""
import ctypes as c
import math
from pathlib import Path

lib = c.CDLL(str(Path('build/compressor.lv2/plugin.so').resolve()))
class Descriptor(c.Structure):
    _fields_ = [('uri', c.c_char_p), ('instantiate', c.c_void_p), ('connect', c.c_void_p),
                ('activate', c.c_void_p), ('run', c.c_void_p), ('deactivate', c.c_void_p),
                ('cleanup', c.c_void_p), ('extension', c.c_void_p)]
lib.lv2_descriptor.argtypes = [c.c_uint32]
lib.lv2_descriptor.restype = c.POINTER(Descriptor)
d = lib.lv2_descriptor(0)
assert d and not lib.lv2_descriptor(1)
instantiate = c.CFUNCTYPE(c.c_void_p, c.POINTER(Descriptor), c.c_double, c.c_char_p, c.c_void_p)(d.contents.instantiate)
connect = c.CFUNCTYPE(None, c.c_void_p, c.c_uint32, c.c_void_p)(d.contents.connect)
activate = c.CFUNCTYPE(None, c.c_void_p)(d.contents.activate)
run = c.CFUNCTYPE(None, c.c_void_p, c.c_uint32)(d.contents.run)
cleanup = c.CFUNCTYPE(None, c.c_void_p)(d.contents.cleanup)
h = instantiate(d, 48000, b'', None)
assert h
n = 48000
left = (c.c_float * n)(*([1.0] * n))
right = (c.c_float * n)(*([0.5] * n))
out_l, out_r = (c.c_float * n)(), (c.c_float * n)()
controls = [c.c_float(-20), c.c_float(4), c.c_float(1), c.c_float(100)]
for i, value in enumerate([left, right, out_l, out_r, *controls]):
    connect(h, i, c.cast(c.pointer(value), c.c_void_p))
activate(h)
run(h, n)
expected = math.pow(10, -15 / 20)
assert abs(out_l[-1] - expected) < 1e-5, (out_l[-1], expected)
assert abs(out_r[-1] - 0.5 * expected) < 1e-5
controls[1].value = 1
run(h, n)
assert abs(out_l[-1] - 1.0) < 1e-6
activate(h)
controls[1].value = 4
left[0] = 0.0
right[0] = 0.0
run(h, 1)
assert out_l[0] == out_r[0] == 0
cleanup(h)
print('single-threaded LV2 smoke test passed')
