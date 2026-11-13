#!/usr/bin/env bash
# What tools/build.sh --test needs beyond packaging/linux/ci-deps.sh, on the same Ubuntu 20.04 system: Node.js 22 (the
# development lobby uses node:sqlite) and Python 3.9 with cryptography, pillow and numpy in CHECKOUT/.venv. Run as root.
#
#   packaging/linux/test-deps.sh CHECKOUT
set -euo pipefail
NODE_VERSION=22.23.5
NODE_SHA256=cdeb8670a0c81f51b4dc4ac61919f9b1ad917b304bf8221eb939570663d240f8

die() { echo "ERROR: $*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }
checkout=$(cd -- "${1:?usage: packaging/linux/test-deps.sh CHECKOUT}" && pwd)
[[ $(id -u) == 0 ]] || die 'run as root'
. /etc/os-release
[[ $ID == ubuntu && $VERSION_ID == 20.04 ]] || die "this is $PRETTY_NAME, not Ubuntu 20.04"
export DEBIAN_FRONTEND=noninteractive

step "Node.js $NODE_VERSION"
if [[ $(/opt/node/bin/node --version 2>/dev/null) != "v$NODE_VERSION" ]]; then
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    curl -fsSL --retry 3 -o "$work/node.tar.xz" \
        "https://nodejs.org/dist/v$NODE_VERSION/node-v$NODE_VERSION-linux-x64.tar.xz"
    echo "$NODE_SHA256  $work/node.tar.xz" | sha256sum -c --status - || die 'Node.js does not match its pinned SHA-256'
    rm -rf /opt/node
    mkdir -p /opt/node
    tar -xJf "$work/node.tar.xz" -C /opt/node --strip-components 1
fi
ln -sf /opt/node/bin/node /usr/local/bin/node
node --version

step 'Python 3.9 (Ubuntu 20.04 archive) and the test packages'
apt-get install -y -qq --no-install-recommends python3.9 python3.9-venv >/dev/null
[[ -x $checkout/.venv/bin/python ]] || python3.9 -m venv "$checkout/.venv"
"$checkout/.venv/bin/python" -m pip install --quiet --disable-pip-version-check --upgrade pip
"$checkout/.venv/bin/python" -m pip install --quiet --disable-pip-version-check cryptography pillow numpy
"$checkout/.venv/bin/python" -I -c 'import sys, cryptography, PIL, numpy; print(sys.version.split()[0], cryptography.__version__, PIL.__version__, numpy.__version__)'
