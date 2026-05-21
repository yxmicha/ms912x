#!/usr/bin/env bash
set -euo pipefail

make clean
make
sudo insmod ./drm/usbdisp_drm.ko
sudo insmod ./drm/usbdisp_usb.ko
lsmod | grep usbdisp
