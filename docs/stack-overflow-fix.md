# Stack Overflow Fix: FT2 Decoder Thread Pool Crash

## Problem

JS8Call crashed on macOS with `EXC_BAD_ACCESS (SIGBUS)` / `KERN_PROTECTION_FAILURE` in
thread pool threads shortly after the decoder started processing audio. The faulting address
was always in the thread's stack guard page.

Crashed thread (from crash report):

```
0  ___chkstk_darwin + 55
1  JS8::DecodeFT2::decodeL2(...) + 372
2  (unnamed — triggeredDecodeFT2 call chain)
3  (unnamed — thread pool dispatch)
```

`rax` at the time of the fault was `0x89478` = **562,296 bytes** — the size of the stack
frame that `decodeL2` was trying to allocate. Thread pool threads have a default stack size
of 520KB, so the allocation failed immediately.

## Root Cause

`triggeredDecodeFT2` in [JS8_Mode/ft2_modem.cpp](../JS8_Mode/ft2_modem.cpp) declared three
large arrays on the stack:

| Variable | Type | Size |
|----------|------|------|
| `dd` | `float[NMAX]` | NMAX=90,000 → **360 KB** |
| `cd2` (Phase 1 loop) | `Cx[NP]` | NP=10,000 × 8 bytes → **80 KB** |
| `cd2` (Phase 2 loop) | `Cx[NP]` | **80 KB** |
| `cb` (Phase 2 loop) | `Cx[NP]` | **80 KB** |

**Total: ~600 KB** — exceeding the 520 KB thread stack.

The compiler also inlined `ft2_triggered_decode_c` → `triggeredDecodeFT2` into `decodeL2`,
merging all their stack frames. The combined frame size reached 562 KB, which is what the
crash report recorded.

The same problem existed in `decodeFT2` (the non-triggered path): `float dd[NMAX]` (360 KB)
and `Cx cd2[NP]` / `Cx cb[NP]` (80 KB each) were also stack-allocated there.

## Fix

All large stack arrays in both functions were moved to heap allocation using
`std::make_unique`. The changes are **unconditional** — they apply on all platforms (macOS,
Linux, Windows). The stack overflow is a real bug on any platform where thread pool threads
have a stack smaller than ~600 KB, which is the default on all major OS platforms.

### `ft2_modem.cpp` — `triggeredDecodeFT2`

```cpp
// Before
float dd[NMAX];

// After
auto dd_buf = std::make_unique<float[]>(NMAX);
float *dd = dd_buf.get();
```

Same pattern applied to the three `Cx[NP]` arrays inside the loop bodies.

`__attribute__((noinline))` added to `triggeredDecodeFT2` to prevent the compiler from
merging its frame with callers.

### `ft2_modem.cpp` — `decodeFT2`

Same heap-allocation fix applied to `float dd[NMAX]`, `Cx cd2[NP]`, and `Cx cb[NP]`.

### `ft2_impl.cpp` — `ft2_triggered_decode_c`

`__attribute__((noinline))` added to the `extern "C"` wrapper to prevent inlining into
`decodeL2`.

## Files Changed

- [JS8_Mode/ft2_modem.cpp](../JS8_Mode/ft2_modem.cpp)
- [JS8_Mode/ft2_impl.cpp](../JS8_Mode/ft2_impl.cpp)

## Verified

Tested on macOS 15.7.4, Intel Mac mini (Macmini8,1), built with AppleClang 17 and
Qt 6.11.0 (Homebrew). Application starts, audio initialises, and the decoder runs without
crashing.
