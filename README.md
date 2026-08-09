# android_hardware_nvidia_hwcomposer

A hardware composer for NVIDIA Tegra K1 (T124), speaking the HWC2 API to
SurfaceFlinger and driving the display controller through the `tegra_dc_ext`
ioctl interface.

## Why

The board ships a proprietary HWC1 module, and the platform provides an adapter
that lets an HWC1 module serve the HWC2 API. On this hardware that adapter does
not hold up.

The failure is in fence ownership. The NVIDIA module takes the acquire fence out
of the layer structure and zeroes the field, and closes fences internally when it
recycles framebuffer layers — neither of which the adapter tracks. It ends up
either closing a descriptor that was already taken, or leaking the ones it never
learned about, at a few descriptors per composed frame. Under a steady frame rate
the process walks into its descriptor limit and SurfaceFlinger dies with it.

Papering over that means fixing the adapter for one vendor's quirks. Speaking
HWC2 natively means the fence discipline is ours and correct by construction,
which is what this is.

Target device: Xiaomi Mi Pad 1st generation, codename `mocha`.

## Approach

This is not a fork. The codebase is our own, and selected files are adapted from
[drm-hwcomposer](https://gitlab.freedesktop.org/drm-hwcomposer/drm-hwcomposer) —
the HWC2 entry points, layer bookkeeping, fence discipline and backend registry,
the parts that are display-hardware agnostic and have years of production use
behind them. Everything DRM/KMS-specific is left out: the R24.1 kernel this
device runs has no DRM/KMS driver for the display controller, and no PRIME path.

Adapted files keep their original copyright headers, and the notes in the
design documents record what came from where.

Backend-facing interfaces sit between the composer core and the display:
a pipeline, a compositor, a vsync source and an immutable frame plan. The
implementation behind them issues `TEGRA_DC_EXT_FLIP3` and reads display events
from the kernel's event mask.

## Design documents

Analysis and plans live in the kernel repository, under
`docs/graphics/hwc/` in [SmokeR24.1-kernel](https://github.com/arttttt/SmokeR24.1-kernel):
anatomy of the existing HWC1 module, the HWC1/HWC2 API and fence-semantics
comparison, the display controller ioctl reference, the composition engines, and
the implementation plan itself.

## License

Apache License 2.0 — see [LICENSE](LICENSE).

Apache-2.0 rather than GPL-2.0 because the files adapted from drm-hwcomposer
carry it, and Apache-2.0 cannot be relicensed into GPL-2.0.
