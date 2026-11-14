let
  lines = builtins.filter builtins.isString (builtins.split "\n" (builtins.readFile ../dkms.conf));
  hits = builtins.filter (m: m != null) (map (builtins.match ''PACKAGE_VERSION="([^"]+)"'') lines);
in
builtins.head (builtins.head hits)
