{ lib
, stdenv
, fetchFromGitHub
, autoreconfHook
, pkg-config
, rdma-core-usb4 ? null
, pciutils ? null
}:

let
  perftest-src = fetchFromGitHub {
    owner = "linux-rdma";
    repo = "perftest";
    rev = "26.04.17";
    hash = "sha256-oNvzQubmslZ4JUNww/wvWd54JDsDLamCDlorHWlNtaY=";
  };
in
# Linux build against our rdma-core-usb4 provider.
stdenv.mkDerivation {
  pname = "perftest";
  version = "26.04.17";
  src = perftest-src;

  nativeBuildInputs = [ autoreconfHook pkg-config ];
  buildInputs = [ rdma-core-usb4 pciutils ];

  postPatch = ''
    # Force HAVE_MLX5DV=no even though rdma-core ships mlx5dv.h. The
    # perftest negotiate struct grows by 8 bytes when MLX5DV is on, which
    # changes the on-wire protocol against existing builds without it.
    # Our usb4_rdma devices aren't Mellanox anyway.
    substituteInPlace configure.ac \
      --replace-fail '[HAVE_MLX5DV=yes], [HAVE_MLX5DV=no])' '[HAVE_MLX5DV=no], [HAVE_MLX5DV=no])'
  '';

  configureFlags = [
    "--disable-cudart"
    "--disable-rocm"
    "--disable-neuron"
    "--disable-cq_ex"
  ];

  meta = with lib; {
    description = "OFED InfiniBand performance tests (linked against rdma-core-usb4)";
    homepage = "https://github.com/linux-rdma/perftest";
    license = licenses.bsd2;
    maintainers = with maintainers; [ georgewhewell ];
    platforms = platforms.linux;
  };
}
