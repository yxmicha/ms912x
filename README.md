# ms912x — Linux DRM Driver for MacroSilicon USB Display Adapters

Out-of-tree Linux DRM kernel module for MacroSilicon 345f:9132 USB display
adapters, packaged for NixOS.

## Background

This driver was written to make the **Neewer X11 Teleprompter** work on
Linux. (X11 is the product name, not the display protocol — the driver
works on Wayland.) The device connects over USB and presents itself as a
345f:9132 (MacroSilicon) display adapter.

Two existing options both had problems:

- **MacroSilicon's official driver** — unusably laggy. Sends a full
  framebuffer over USB on every update, saturating bandwidth.
- **Other open-source implementations** — do not work at all with this
  device. The 345f:9132 uses a different partial painting protocol from
  what those drivers implement, so frames are never displayed correctly.

This driver is derived from MacroSilicon's official source and reworked
to fix the performance problems. It keeps the correct USB protocol for the
345f:9132 while replacing the full-frame transfer path with damage-tracked
partial updates and double-buffered async USB submission. It is designed
to work on Wayland on machines where the other displays are driven by a
GPU — the common case where existing drivers that rely on GPU render
offload are not an option.

## Performance

The MacroSilicon official driver sends a full framebuffer over USB on every
update — at 1920×1080 with YUV422 encoding that is ~4 MB per frame, saturating
USB bandwidth and causing the lag that makes it unusable. This driver avoids
that in several ways:

**Damage tracking.** The DRM atomic commit path uses
`drm_atomic_helper_damage_merged` to compute the dirty rectangle reported by
the compositor. Only the changed region is encoded and sent over USB. For
typical desktop use — cursor movement, a single window updating — this reduces
transfer size by an order of magnitude or more.

**Double-buffered async USB submission.** Two transfer buffers are kept
in-flight. While one buffer is being transmitted by the USB host controller,
the driver encodes the next frame into the second buffer. The encoding and USB
transfer overlap, eliminating the stall-per-frame of the upstream design.

**Direct shadow plane mapping.** Source pixels are read directly from the
shadow plane's pre-mapped framebuffer. There is no intermediate copy and no
`dma_buf_begin/end_cpu_access` round-trip.

**Scatter-gather URBs.** Transfers use scatter-gather rather than a
contiguous vmalloc buffer, removing one large allocation and avoiding
fragmentation pressure under sustained use.

## Device

| Field       | Value              |
|-------------|--------------------|
| Vendor      | MacroSilicon       |
| USB ID      | `345f:9132`        |
| Driver      | `usbdisp_drm`      |
| Interface   | USB 3.0            |
| Output      | HDMI (up to 1080p) |

## Kernel support

Requires **Linux 4.19 or later**. Tested against the kernels provided by
`nixos-unstable` (currently 6.x). The `flake.lock` pins the exact nixpkgs
revision used to build, so the kernel version is fully reproducible.

## Usage

### NixOS flake (recommended)

```nix
# flake.nix
inputs.ms912x.url = "github:yxmicha/ms912x";

# configuration.nix (or equivalent)
{ inputs, ... }:
{
  imports = [ inputs.ms912x.nixosModules.default ];
  hardware.ms912x.enable = true;
}
```

The module adds `usbdisp_drm.ko` and `usbdisp_usb.ko` to
`boot.extraModulePackages` and loads them automatically on boot.

### Manual load

```sh
nix build .#default
sudo insmod result/lib/modules/$(uname -r)/kernel/drivers/gpu/drm/usbdisp_drm.ko
sudo insmod result/lib/modules/$(uname -r)/kernel/drivers/gpu/drm/usbdisp_usb.ko
```

## Development

Enter the dev shell to get `checkpatch.pl`, `shellcheck`, `nixpkgs-fmt`,
and `statix` together with lefthook hooks:

```sh
nix develop
```

## License

GPL-2.0-only.
