# ms912x — Linux DRM Driver for MacroSilicon USB Display Adapters

Out-of-tree Linux DRM kernel module for MacroSilicon 345f:9132 USB display
adapters, packaged for NixOS.

## Background

This driver was written to make the **Neewer X11 Teleprompter** work on
Linux. (X11 is the product name, not the display protocol — the driver
works on Wayland.) The device connects over USB and presents itself as a
345f:9132 (MacroSilicon) display adapter.

Two existing options both had problems:

- **MacroSilicon's official driver** — unusably laggy.
- **Other open-source implementations** — do not work at all with this
  device. The 345f:9132 uses a different partial painting protocol from
  what those drivers implement, so frames are never displayed correctly.

This driver implements the correct partial painting protocol for the
345f:9132 and is unrelated to either of the above. It is designed to work
on Wayland on machines where the other displays are driven by a GPU —
the common case where existing drivers that rely on GPU render offload
are not an option.

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
