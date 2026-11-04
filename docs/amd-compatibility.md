# AMD Vulkan compatibility

The renderer uses the installed Vulkan driver. CUDA is only relevant to the
separate, optional Python texture upscaler.

## Vertex fetch fix

On AMD Radeon(TM) Graphics, driver 32.0.32015.7029, an interleaved vertex
buffer could lose its final attribute when that attribute had a nonzero binding
offset. For the affected fullscreen triangle, UVs at byte 24 of a 32-byte stride
were returned as zero for the final vertex. The resulting downsample stretched
one row of the correctly rendered scene into vertical columns. Text also lost
parts of its final glyphs.

Vertex uploads now reserve and zero at least one extra stride (minimum 16 bytes),
without reading beyond the guest buffer. Cached buffers grow if the same guest
range is subsequently used with a larger stride. Streaming uploads use the same
padding. The existing write tracking and GPU lifetime rules remain in use.

The `render_geometry` regression draws three vertices from a 96-byte interleaved
buffer, with UVs at offset 24 and stride 32, then checks every output pixel. It
failed on the tested AMD GPU before the stride padding and passes afterward.
It also exercises a cached buffer whose required padding grows.

## Other portability changes

- GPU selection checks required features before ranking adapters. `RBREW_GPU`
  accepts a Vulkan adapter index or a case-insensitive device-name substring.
- Warm-up skips attachment formats unsupported by the current GPU, including
  D24 depth records created on another adapter.
- Unexported vertex varyings receive the Latte pixel-input defaults. Vertex and
  fragment interfaces therefore agree, including rectangle geometry expansion.
  The translation compatibility revision is bumped; old cache files are retained
  but cannot supply the changed translations.

## Raster state and overlay fixes

Rasterizer-discard pipelines declare and update only the dynamic state they use.
Switching between discard and normal rendering resets command-state tracking so
depth and stencil values are emitted again. A GPU regression checks these transitions.

The overlay attachment barrier now includes color-attachment read access for its
load operation. The presentation regression includes ordinary overlay frames with
synchronization validation enabled.

## Verification and limits

On 3 November, the combined local build against upstream `dde4e71` passed all
69 native tests on GPU 0 with synchronization validation, plus both recompiler
tests. That build includes a fork-only offline bot test which is not part of
this change. The PR provides 68 native cases including GPU selection. The
vertex-boundary regression covers both tracked and untracked cache entries.

The broader run exposed an integer histogram descriptor mismatch and unsupported
depth blits. The weather composite now binds a correctly typed histogram even
when tone matching is disabled, and scaled depth arrays use a shader fallback
when depth blits are unavailable. The depth regression checks an explicit
array slice as well as scaled image edges.

Local verification on 2 November 2026 uses GPU 0, AMD Radeon(TM) Graphics,
Vulkan 1.4.359, Windows driver 32.0.32015.7029. Authored GPU tests cover vertex
fetch, shader translation, descriptor and pipeline handling, buffer transfers,
allocation recovery, and command recording. Run them with `RBREW_BUILD_TESTS=ON`.
All 17 targeted cases pass with synchronization validation enabled. The normal
executable is rebuilt with tests and diagnostic logging disabled, then launched
on GPU 0 for a separate smoke check. The previous executable is retained as
`Game/Splatoon-before-amd-fix.exe`.

The loading screen, title screen, and plaza are checked separately using the
owned game. These checks do not certify every effect, game mode, or driver.
The RX 9060 XT and NVIDIA regression hardware were not available in this session.
The follow-up live run reports no validation errors in the observed scenes.
One warning remains for a shader color output without a bound attachment; that
output is unused. This does not certify validation coverage for every game mode.

Gameplay builds compile out diagnostic logging. For investigation, use a separate
build with `RBREW_DIAGNOSTIC_RUNTIME=ON`; Vulkan validation additionally needs the
Khronos validation layer. Captures contain private game data and must stay local.
