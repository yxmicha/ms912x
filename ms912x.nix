{ lib
, stdenv
, kernel
,
}:
stdenv.mkDerivation {
  pname = "ms912x";
  version = "0.1.0";

  src = ./src;

  nativeBuildInputs = kernel.moduleBuildDependencies;

  makeFlags = [
    "KVER=${kernel.modDirVersion}"
    "KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
  ];

  installPhase = ''
    runHook preInstall
    install -Dm644 drm/usbdisp_drm.ko "$out/lib/modules/${kernel.modDirVersion}/kernel/drivers/gpu/drm/usbdisp_drm.ko"
    install -Dm644 drm/usbdisp_usb.ko "$out/lib/modules/${kernel.modDirVersion}/kernel/drivers/gpu/drm/usbdisp_usb.ko"
    runHook postInstall
  '';

  meta = {
    description = "Linux DRM driver for MacroSilicon 345f:9132 USB display adapters";
    license = lib.licenses.gpl2Only;
    platforms = lib.platforms.linux;
  };
}
