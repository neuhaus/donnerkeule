{ lib
, stdenv
, pkg-config
, rdma-core-usb4 ? null
, python3
, source ? ../userspace/bench
}:

let
  # ibv_trace is an LD_PRELOAD tracer built as .so.
  cPrograms = [
    "rc_write_poll"
    "rc_write_verify"
    "u4_pingpong"
    "uc_oneway"
    "uc_write_verify"
    "rc_qpn_churn"
    "rc_write_imm_verify"
    "rdma_gid_probe"
  ];

  scripts = [
    "tbv_perftest_runner.py"
    "tbv_rdma_sweep.py"
    "tbv_uc_stress.py"
    "tbv_vllm_smoke.sh"
  ];
in
stdenv.mkDerivation {
  pname = "thunderbolt-ibverbs-bench-tools";
  version = import ./version.nix;

  src = lib.cleanSourceWith {
    src = source;
    filter = path: _type:
      let
        rel = baseNameOf (toString path);
      in
        lib.hasSuffix ".c" rel
        || lib.hasSuffix ".h" rel
        || lib.hasSuffix ".py" rel
        || lib.hasSuffix ".sh" rel;
  };

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ rdma-core-usb4 python3 ];

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    for name in ${lib.concatStringsSep " " cPrograms}; do
      $CC -O2 -Wall -Wextra -std=gnu11 \
        -I${rdma-core-usb4.dev}/include \
        "$name.c" \
        -L${rdma-core-usb4}/lib -libverbs \
        -Wl,-rpath,${rdma-core-usb4}/lib \
        -o "$name"
    done
    $CC -O2 -Wall -Wextra -fPIC -shared \
      -I${rdma-core-usb4.dev}/include \
      ibv_trace.c \
      -ldl -lpthread \
      -L${rdma-core-usb4}/lib -libverbs \
      -Wl,-rpath,${rdma-core-usb4}/lib \
      -o libibv_trace.so
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    install -m 0755 ${lib.concatStringsSep " " cPrograms} $out/bin/
    install -m 0755 ${lib.concatStringsSep " " scripts} $out/bin/
    install -m 0644 tbv_bench.py $out/bin/
    mkdir -p $out/lib
    install -m 0644 libibv_trace.so $out/lib/
    runHook postInstall
  '';

  postFixup = ''
    patchShebangs $out/bin
  '';

  meta = with lib; {
    description = "Thunderbolt/USB4 RDMA bench programs (libibverbs C tools + perftest helper scripts)";
    license = licenses.mit;
    maintainers = with maintainers; [ georgewhewell ];
    platforms = platforms.linux;
  };
}
