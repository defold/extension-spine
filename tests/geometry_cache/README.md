# Geometry cache regression checks

Run from the repository root with Python 3 and a GCC-compatible C++11 compiler:

```sh
python tests/geometry_cache/run.py
python tests/geometry_cache/run.py --cxx /path/to/g++
```

The runner compiles the current cache helpers, render-object setup, `RenderBatch`,
and `RenderListDispatch` directly from `comp_spine_model.cpp`. It uses deterministic
test doubles for the skeleton generator, array storage and engine/GPU calls. It
does not copy or reimplement the cache algorithm. Generated code and executables
live in a temporary directory. `--source /path/to/comp_spine_model.cpp` can check a
saved revision against the same assertions.

Coverage:

- Modes 0/1/2, identical same-frame passes and invalidation across epochs.
- Shorter/longer prefixes, reordered components, changed revisions, empty passes
  and components producing no geometry.
- Mixed Alpha/Add/Inherit batches and render-object overflow storage.
- 65,535 / 65,536 / 65,537 vertex cases and transitions back to 16-bit indices.
- Corrupted direct 32-bit indices, retained packed 16-bit bytes and cached vertices.
- Corrupted in-bounds render ranges, materials, textures, constants and blend factors.
- Repairing current-pass uploads/objects and disabling subsequent cache reuse.

The fault-injection cases assert that corrupted indices and render-object state
are detected and repaired before the current pass reaches the GPU upload or draw.
The other cases protect cache reuse, invalidation and fallback behavior.

These tests validate CPU control flow and upload bytes, not actual Spine geometry,
GPU output, WebGL limits, context recovery or platform packaging. The expected
render object is built from independent generator ranges and batch inputs, using
the shared render-state initializer; a defect common to that initializer and both
paths still requires an engine/visual test.

## Platform verification

Keep `spine.geometry_cache` at its default `0` until the target configurations have
been tested. Use the project's required Defold version (currently at least 1.14.1)
and its matching SDK/Bob. Do not lower the project's minimum version to make a test
build pass.

For each target, build the project with modes 0, 1 and 2 and compare the same
scene/camera sequence. Exercise repeated draws of the same Spine predicate within
one render-script update, reordered/shortened visible lists, empty draws, mixed
blend modes and geometry around the index-width boundary. Mode 2 must report
`mismatches: 0`, `failed: false`, and positive `hits`/`complete_pass_reuse` in a
repeated-draw scene. Only mode 1 is suitable for measuring the optimized runtime;
mode 2 intentionally regenerates geometry for comparison.

| Target | Build | Runtime/visual verification |
| --- | --- | --- |
| Web | `wasm-web` | HTTP-served bundle in desktop and mobile browsers; check actual WebGL version |
| Android | `arm64-android`, plus `armv7-android` if shipped | Device runs with the shipped graphics backend; pause/resume |
| iOS | `arm64-ios` | Signed device build with the shipped graphics backend; pause/resume |

For WebGL 1 / GLES 2 targets, verify 32-bit index support separately. The existing
renderer selects 32-bit indices above 65,536 vertices without checking backend
capabilities or splitting the geometry. This patch does not change that behavior;
turning off the cache does not fix an unsupported index format.

### Validation scope

The regression suite covers CPU control flow and uploaded bytes using test
doubles. Complete platform bundles, real Spine/GPU rendering, device lifecycle
behavior and performance comparisons require the target checks above. Passing
this suite alone does not establish Web, Android or iOS runtime compatibility.
