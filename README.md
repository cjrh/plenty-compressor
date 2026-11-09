# Plenty Compressor

An **experimental, not host-ready** stereo LV2 compressor. The peak detector and gain curve are written in Plenty; a small C adapter implements the LV2 ABI, host buffers, and parameter conversion. No third-party libraries are used. The generic host UI supplies four controls: threshold, ratio, attack, and release. There is no custom GUI.

## Important limitations

**Do not install this in a DAW or use it on a real-time audio thread yet.** Plenty 0.8.21's generated `compressor.h` requires *all calls* to the library to be serialized on the same caller thread. LV2 permits multiple plugin instances and hosts may run them on different threads. The adapter does not enforce Plenty's stronger contract, so ordinary hosts can violate it. A mutex alone would not meet the same-thread requirement and would not be appropriate in the audio callback. A usable plugin needs a supported Plenty threading contract compatible with audio hosts, or a different integration design.

Real-time safety has not been established: the Plenty runtime and `std.math.pow` have not been audited for blocking, allocation, worst-case timing, or denormals. A scalar foreign call and power calculation per sample is also likely expensive. The test host exercises only a single thread, not a DAW. The placeholder plugin URI should be replaced with an owned, stable URI before distribution.

## Build and test

Requires Linux, Plenty 0.8.21, a C compiler, Python 3 (for the smoke test), and [just](https://just.systems/). No LV2 SDK is required; the few core ABI declarations are in `lv2/plugin.c`.

```sh
just check
just test
```

`just build` creates `build/compressor.lv2/` with the adapter, generated Plenty shared library, and LV2 metadata. `just clean` removes build artifacts. The test loads the LV2 descriptor and runs the effect on one thread; it verifies linked-channel gain, the steady-state curve, 1:1 ratio, and silence. It does not certify LV2 compatibility or real-time safety.

## Processing

The maximum absolute level of the two input channels feeds a one-pole envelope follower with separate attack and release time constants. Above threshold, its linear gain is `(envelope / threshold) ** (1 / ratio - 1)`; the same gain is applied to both channels. Parameter changes are sampled once per processing block. There is no makeup gain, knee, lookahead, sidechain, or parameter smoothing. Input NaNs and infinities are replaced with silence; control values are clamped to their declared ranges.

## References

- [LV2 programming guide](https://lv2plug.in/book/): descriptor, ports, and host lifecycle.
- [LV2 core threading classes](https://drobilla.net/files/lv2_docs/ns/lv2core.html): instance-level host threading rules.
- [nice-plug](https://codeberg.org/RustAudio/nice-plug): a higher-level Rust plugin framework; its GUI-less examples informed the initial scope.
- Plenty's generated `build/compressor.lv2/compressor.h` is the authoritative contract for the compiled library. The local Plenty source documentation is `../plenty/book/src/design/facilities/04-interop/exports.md`.
