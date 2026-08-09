# android_hardware_nvidia_hwcomposer

A hardware composer for NVIDIA Tegra K1 (T124), speaking the HWC2 API to
SurfaceFlinger and driving the display controller through the `tegra_dc_ext`
ioctl interface.

Target device: Xiaomi Mi Pad 1st generation, codename `mocha`.

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

## Architecture

Interfaces sit between the composer core and the display: a pipeline, a
compositor, a vsync source, and an immutable per-frame plan. Behind them the
Tegra implementation issues `TEGRA_DC_EXT_FLIP3` and reads display events from
the kernel's event mask. Nothing DRM/KMS is involved — the R24.1 kernel this
device runs has no DRM driver for the display controller and no PRIME path.

Parts are adapted from
[drm-hwcomposer](https://gitlab.freedesktop.org/drm-hwcomposer/drm-hwcomposer);
those files keep their original copyright headers.

## Design documents

Analysis and plans live under `docs/graphics/hwc/` in
[SmokeR24.1-kernel](https://github.com/arttttt/SmokeR24.1-kernel): anatomy of the
existing HWC1 module, the HWC1/HWC2 API and fence-semantics comparison, the
display controller ioctl reference, the composition engines, and the
implementation plan.

## License

Apache License 2.0 — see [LICENSE](LICENSE).
