{
  lib,
  stdenv,
  kernel,
  source ? ../.,
  enableProfiling ? true,
}:

let
  root = source;
  rootPrefix = "${toString root}/";
  sourceEntries = [ "Makefile" "dkms.conf" "kernel" "proto" ];
in
stdenv.mkDerivation {
  pname = "thunderbolt-ibverbs";
  version = import ./version.nix;

  src = lib.cleanSourceWith {
    src = root;
    filter = path: type:
      let
        rel = lib.removePrefix rootPrefix (toString path);
      in
        builtins.any (entry: rel == entry || lib.hasPrefix "${entry}/" rel) sourceEntries;
  };

  nativeBuildInputs = kernel.moduleBuildDependencies;

  buildPhase = ''
    runHook preBuild
    make \
      TBV_PROFILING=${if enableProfiling then "1" else "0"} \
      KVER=${kernel.modDirVersion} \
      KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build \
      modules
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -D -m 0644 kernel/thunderbolt_ibverbs.ko \
      "$out/lib/modules/${kernel.modDirVersion}/extra/thunderbolt_ibverbs.ko"
    runHook postInstall
  '';

  dontPatchELF = true;
  dontStrip = true;
  dontFixup = true;

  meta = {
    description = "Thunderbolt/USB4 host-to-host RDMA verbs kernel module";
    license = lib.licenses.gpl2Only;
    maintainers = with lib.maintainers; [ georgewhewell ];
    platforms = [ "x86_64-linux" ];
  };
}
