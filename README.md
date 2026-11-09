# Plenty Compressor

An experimental stereo LV2 compressor written in Plenty. The plugin descriptor,
callbacks, instance lifecycle, port connections, parameter conversion and DSP are
all Plenty code. A trusted `.plentyi` module declares C memory operations and the
system allocator; no handwritten C adapter is compiled or shipped.

The generic host UI supplies threshold, ratio, attack and release controls.
There is no custom GUI. Real-time execution has not been audited or certified,
and the metadata does not advertise `lv2:hardRTCapable`.

## Build and test

Requires Linux on a supported 64-bit Plenty target, a compiler built from the
Plenty `main` at `d16e1a68` or later, the native linker toolchain, Python 3,
and [just](https://just.systems/). Released Plenty 0.8.22 does not include these
interface extensions. No LV2 SDK is required.

```sh
# Build Plenty in the sibling checkout first.
cargo build --locked --manifest-path ../plenty/Cargo.toml --bin plenty
PLENTY=../plenty/target/debug/plenty just check
PLENTY=../plenty/target/debug/plenty just test
```

These paths assume sibling `plenty` and `plenty-compressor` checkouts. Set
`PLENTY` to the compiler path if your checkouts are elsewhere. `PLENTY` defaults
to `plenty` on PATH. `just build` produces
`build/compressor.lv2/plugin.so` and LV2 metadata. `just clean` removes build
artifacts. Tests load the actual descriptor and invoke its callback addresses,
including concurrent independent instances and ordered thread handoff. They are
an ABI and signal regression harness, not a DAW compatibility certification.

## Interface design

`src/lib.plenty` implements the plugin. `src/abi.plentyi` is the trusted boundary:

- Opaque pointer exports and exact symbol naming expose `lv2_descriptor`.
- Static storage holds the descriptor and its URI. Callback pointers refer to
  generated C calling-convention adapters, not Plenty's internal function ABI.
- `@thread_safe` exports check reachable Plenty effects. The host serializes
  access to each instance and synchronizes thread handoff; independent instances
  may run concurrently.
- Raw scalar loads and stores permit pointer aliases. They do not construct
  ordinary shared or exclusive Plenty borrows, and do not validate host memory.
- `calloc`/`free` own each instance. Port buffers remain owned by the host.

The descriptor is initialized with Linux GNU `pthread_once`, so discovery can
move between threads and repeat safely. The internal initializer callback must
only be invoked through that once control. Its address stays stable, and its
optional deactivate and extension-data callbacks are null. Callback code and
static data must remain loaded for their entire use; this prototype does not
qualify runtime library unloading.

C-layout classes and typed function-pointer fields are not implemented by this
prototype. The trusted module and descriptor construction use explicit offsets
for the 64-bit C ABI: eight pointer fields in the descriptor, and eight port
pointers followed by two doubles in each instance. This layout must be reviewed
before using another target. The raw primitives are an experimental compiler
interface, not a general safe pointer API.

## Aliasing

Both input samples are read before either output sample is written. Exact
in-place buffers, crossed stereo buffers and shared input buffers therefore
work without copies. Controls are read before audio output is written. If the
two output ports share storage, the right-channel store is last; distinct
channel results cannot both be represented in one buffer.

Partially overlapping buffers use forward, sample-by-sample memory semantics:
a write can change a later input sample. They are memory accesses with defined
ordering, not an out-of-place snapshot of the original input. No hidden copy is
made. The host must provide valid, aligned storage for every sample and must not
access it concurrently in conflict with processing.

## Processing

The maximum absolute level of the input channels feeds a one-pole envelope
follower with separate attack and release time constants. Above threshold, the
linear gain is `(envelope / threshold) ** (1 / ratio - 1)`; both channels receive
the same gain. Parameters are sampled once per block. Input NaNs and infinities
become silence; nonfinite controls use defaults, and controls are clamped to
their declared ranges. Invalid sample rates reject instantiation.

There is no makeup gain, knee, lookahead, sidechain, parameter smoothing or
performance guarantee. Math errors use neutral fallback coefficients/gain.
The placeholder plugin URI must be replaced with an owned stable URI before
distribution.

## References

- [LV2 core API](https://lv2plug.in/c/html/group__lv2core.html): descriptor,
  callback signatures, port lifetimes and aliasing.
- [LV2 threading classes](https://lv2plug.in/ns/lv2core): discovery and
  instance-level threading contracts.
- [LV2 programming guide](https://lv2plug.in/book/): lifecycle and metadata.
- [nice-plug](https://codeberg.org/RustAudio/nice-plug): its GUI-less examples
  informed the original prototype's scope.
