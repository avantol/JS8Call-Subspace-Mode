# Heap Allocation: `cd[NN*NSS]` Signal Extract Buffer

## What

Moved two `Cx cd[NN*NSS]` arrays from stack to heap in
[JS8_Mode/ft2_modem.cpp](../JS8_Mode/ft2_modem.cpp).

| Function | Location |
|----------|----------|
| `decodeFT2` | inner loop body, per sync candidate |
| `triggeredDecodeFT2` | inner loop body, per sync hit |

## Size

`NN=103`, `NSS=32` → `NN*NSS = 3296` elements × `sizeof(Cx)` (8 bytes) = **26.4 KB per allocation**

## Rationale

No reason to put this on the stack:

- It is a pure working buffer — allocated, filled, consumed, discarded each loop iteration
- The `= {}` zero-initialisation on the stack version cost cycles on every iteration;
  heap allocation via `make_unique` zero-initialises once and has no additional overhead
  vs the stack version
- Reduces per-call stack frame by 26KB in functions that already have significant
  stack pressure from other allocations (see `stack-overflow-fix.md`)
- Consistent with the approach taken for `dd[NMAX]`, `cd2[NP]`, and `cb[NP]`

## Change

```cpp
// Before
Cx cd[NN*NSS] = {};

// After
auto cd_buf = std::make_unique<Cx[]>(NN*NSS);
Cx *cd = cd_buf.get();
```

Zero-initialisation is handled by `make_unique` (value-initialises the array).

## Files Changed

- [JS8_Mode/ft2_modem.cpp](../JS8_Mode/ft2_modem.cpp) — two instances

## Platform Note

Applies unconditionally on all platforms. No functional change.
