# Plenty Compressor

An experimental stereo LV2 and CLAP compressor written in Plenty. The plugin descriptor,
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

## CLAP build and test

CLAP currently requires **Linux GNU x86_64**, `libatomic` development/linker
files, a C compiler for tests, and the Plenty integration branch
`codex/clap-abi-integration` (local Plenty PR #1167, commit `eaf3827b`).
The ordinary released compiler and the earlier LV2 compiler are insufficient.
Set `PLENTY` to a compiler built from that integration branch:

```sh
PLENTY=/path/to/integration/target/debug/plenty just check
PLENTY=/path/to/integration/target/debug/plenty just test-all
```

`just build-clap` produces `build/plenty-compressor.clap`. Copy it into your
host's CLAP search directory, or add this build directory to `CLAP_PATH`.
There is one stereo float32 input/output bus and four automatable global
parameters. Hosts supply the parameter UI. Float64 audio, per-note parameters,
modulation and a custom GUI are not implemented. The experimental ID
`org.plenty.compressor` and placeholder project URL need stable ownership before
distribution. VST3 is not implemented.

`src/clap.plenty` implements the CLAP lifecycle, audio, parameters and state;
`src/clap_abi.plentyi` declares trusted native layouts and host callback
signatures. `src/dsp.plenty` shares the gain calculation with LV2. Initialized
C data exports provide `clap_entry`, the descriptor and extension tables with
callback relocations. No C adapter is built into either plugin. The C code in
`tests/clap_host.c` is a test host using pinned upstream CLAP 1.2.10 headers.

Control values use aligned, lock-free 64-bit atomic loads/stores through the
system `libatomic`; creation fails if the allocation cannot support them.
Processing owns the envelope, performs no explicit allocations or locking,
and handles sample-timed core parameter events. This is still an experimental
plugin without audited real-time guarantees or DAW compatibility certification.
Invalid/nonfinite parameter events are ignored or clamped; note-targeted events
are ignored. Zero-frame calls apply control events without touching audio.
Parameter text is locale-independent decimal rounded to three fractional digits;
parsing accepts signed decimal numbers without units or exponent notation.

State is 40 bytes: little-endian `PCLP`, version 1 as a 32-bit integer, then four
IEEE754 doubles in parameter order. Partial stream transfers are supported.
Failed reads or invalid values leave controls unchanged; successful loads
notify the host through `clap.params.rescan(VALUES)`. Controls are independent
atomics: concurrent automation may interleave with a save or successful load,
so a whole-preset transactional snapshot is not promised. Loading never resets
the audio-owned envelope.

The CLAP build passes `--unloadable-library`. Before unloading, the host must
stop and quiesce callbacks, destroy all instances, call entry `deinit`, and
discard library pointers. Host threads may remain alive. The plugin creates
no threads or deferred callbacks of its own. Native tests check teardown and
physical unmapping; this qualification applies to the supported target and
runtime subset, not arbitrary foreign dependencies.

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
static data must remain loaded for their entire use; the LV2 build does not
qualify runtime library unloading. The CLAP build uses the unloadable runtime.

C-layout classes and typed function-pointer fields are not used by this
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
the same gain. LV2 parameters are sampled once per block; CLAP automation is applied at its
sample offset, preserving the order of events at the same offset. Input NaNs and infinities
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
