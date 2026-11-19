#!/usr/bin/env bash
# Deploy a git ref as a DKMS module to both lab hosts (fuzzy and misty).
#
# Usage:
#   tools/lab/deploy.sh VERSION [GIT_REF]
# Example:
#   tools/lab/deploy.sh 0.4.0.dk9 HEAD
set -euo pipefail

VER=${1:?usage: tools/lab/deploy.sh VERSION [GIT_REF]}
REF=${2:-HEAD}
HOST0=${TBV_HOST0:-fuzzy}
HOST1=${TBV_HOST1:-misty}

ROOT=$(git rev-parse --show-toplevel)
STAGE=$(mktemp -d "/tmp/tbv-deploy-XXXXXX")
trap 'rm -rf "$STAGE"' EXIT

DIR="$STAGE/thunderbolt-ibverbs-$VER"
TGZ="$STAGE/thunderbolt-ibverbs-$VER.tar.gz"
mkdir -p "$DIR"
git -C "$ROOT" archive "$REF" | tar -x -C "$DIR"
perl -pi -e "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$VER\"/" "$DIR/dkms.conf"
tar -czf "$TGZ" -C "$STAGE" "thunderbolt-ibverbs-$VER"

deploy_host() {
	local h=$1
	scp -q "$TGZ" "$h:/tmp/thunderbolt-ibverbs-$VER.tar.gz"
	ssh "$h" "set -euo pipefail
		kver=\$(uname -r)
		sudo dkms remove -m thunderbolt-ibverbs -v '$VER' --all >/dev/null 2>&1 || true
		sudo rm -rf '/usr/src/thunderbolt-ibverbs-$VER'
		sudo tar -xzf '/tmp/thunderbolt-ibverbs-$VER.tar.gz' -C /usr/src
		rm -f '/tmp/thunderbolt-ibverbs-$VER.tar.gz'
		sudo dkms add -m thunderbolt-ibverbs -v '$VER'
		sudo dkms build -m thunderbolt-ibverbs -v '$VER' -k \"\$kver\"
		sudo dkms install -m thunderbolt-ibverbs -v '$VER' -k \"\$kver\" --force
		echo \"$h: \$(modinfo -F version thunderbolt_ibverbs)\"
	"
}

deploy_host "$HOST0" &
p0=$!
deploy_host "$HOST1" &
p1=$!
wait "$p0"
wait "$p1"
