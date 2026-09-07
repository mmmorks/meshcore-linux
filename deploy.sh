#!/usr/bin/env bash
# deploy.sh - build the Linux firmware and install it on a MeshCore node.
#
# Usage:
#   ./deploy.sh                      # build + deploy to $MESHCORE_HOST (default: pimesh)
#   ./deploy.sh othernode            # deploy to another ssh host
#   SKIP_BUILD=1 ./deploy.sh         # reuse the existing build artifact
#
# Copies the binary over (plus meshcorectl, when the tree ships it), installs
# them, restarts the service, and then insists the service actually came back
# up.
#
# What it deliberately does NOT do:
#
#   * Install udev rules. This is the one piece of setup left to the README,
#     and the reason is worth recording. An earlier version of this script
#     installed variants/linux/99-meshcore.rules when it was absent, which
#     looked harmless and was not: udev sorts rule files by name, 99-meshcore
#     sorts after Raspberry Pi OS's own 99-com.rules, and GROUP=/MODE=
#     overwrite rather than merge -- so it silently reassigned /dev/spidev0.0
#     and /dev/gpiochip* from spi/gpio to the meshcore group, locking out
#     meshtasticd and the login user on a shared node. The udevadm trigger it
#     ran then also destroyed the /dev/gpiochip4 compatibility symlink.
#
#     The setup below takes the opposite approach, which is what a working node
#     turned out to be doing anyway: join the spi/gpio/dialout groups that the
#     distro's own rules already grant, rather than claim the devices. Adding a
#     member to a group takes nothing away from anyone.
#
#   * Touch /etc/meshcored/meshcored.ini, or replace an existing unit file.
#     The config is node-specific and holds the admin password, and the unit
#     may carry local drop-ins that assume the installed version.
#
#   * Install or configure gpsd and chrony. Whether a node feeds its clock from
#     a GNSS receiver is a property of that node, which this script does not
#     read, and the chrony refclock needs a per-board offset that has to be
#     measured on the node rather than guessed. Doing it half-automatically
#     would be worse than not at all: a wrong offset makes chrony classify the
#     GPS as a falseticker and silently ignore it.
#
# The setup steps it DOES run are all idempotent: on an already-provisioned node
# they print nothing and change nothing.
#
# The previous binary is kept as /usr/bin/meshcored.prev, so a bad deploy is
# recoverable with:
#   ssh <host> 'sudo mv /usr/bin/meshcored.prev /usr/bin/meshcored && sudo systemctl restart meshcored'
set -euo pipefail

HOST="${1:-${MESHCORE_HOST:-pimesh}}"
ENV_NAME="${ENV_NAME:-linux_repeater}"
FIRMWARE_VERSION="${FIRMWARE_VERSION:-dev}"
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SSH_OPTS=(-o ConnectTimeout=10)
say()  { printf '>> %s\n' "$*"; }
die()  { printf 'deploy.sh: %s\n' "$*" >&2; exit 1; }
rsh()  { ssh "${SSH_OPTS[@]}" "$HOST" "$@"; }

# --- 1. the one thing that has to be known before building -------------------
#
# libgpiod's two major versions are not ABI compatible, and EventGPIOPin picks
# its code path at compile time, so the target's Debian release decides what to
# build: bookworm ships libgpiod 1.x, trixie ships 2.x.
#
# This is the only thing asked of the node in advance, because it is the only
# one that changes what gets built. Everything else this script could check --
# whether a config exists, whether the unit is installed, whether sudo works,
# whether the arch even matches -- is something where attempting it and reading
# the failure is a better signal than a prediction, so those are left to the
# install step's own conditionals and to the verification at the end.

say "asking $HOST which Debian release it runs"
codename="$(rsh '. /etc/os-release 2>/dev/null && echo "${VERSION_CODENAME:-unknown}"')" \
  || die "cannot reach $HOST over ssh"

case "$codename" in
  bookworm) BASE_IMAGE=debian:bookworm; BUILD_DIR=.pio/build ;;
  trixie)   BASE_IMAGE=debian:trixie;   BUILD_DIR=.pio/build-trixie ;;
  *) die "unrecognised target release '$codename'.
  Pick a base by hand and deploy the result:
    BASE_IMAGE=debian:trixie ./build-docker.sh $ENV_NAME && SKIP_BUILD=1 $0 $HOST" ;;
esac

BIN="$REPO_DIR/$BUILD_DIR/$ENV_NAME/meshcored"

# --- 2. build ----------------------------------------------------------------

if [ "${SKIP_BUILD:-0}" = 1 ]; then
  [ -f "$BIN" ] || die "SKIP_BUILD=1 but $BIN does not exist"
  say "skipping build, using $BIN"
else
  say "building $ENV_NAME for $codename ($BASE_IMAGE)"
  BASE_IMAGE="$BASE_IMAGE" FIRMWARE_VERSION="$FIRMWARE_VERSION" \
    "$REPO_DIR/build-docker.sh" "$ENV_NAME"
  [ -f "$BIN" ] || die "build reported success but $BIN is missing"
fi

# --- 3. ship ------------------------------------------------------------------
#
# STAGE is created remotely with mktemp -d rather than a name built from this
# process's own PID: "/tmp/meshcore-deploy.$$" is predictable and, under the
# default umask, world-writable, so on a shared node another local user could
# pre-create it or swap the binary in the window between the scp below and the
# sudo install that reads from it. mktemp -d picks an unguessable name owned
# by (and, by default, readable/writable only by) the connecting user.

# The CLI client is optional: it is shipped and installed alongside the daemon
# when the tree has it, and a tree without it deploys the daemon alone. (The
# ${arr[@]+...} form keeps `set -u` happy on bash 3.2 when the list is empty --
# macOS still ships that bash.)
TOOLS=()
for t in meshcorectl; do
  [ -f "$REPO_DIR/variants/linux/$t" ] && TOOLS+=("$REPO_DIR/variants/linux/$t")
done

STAGE="$(rsh 'mktemp -d')" || die "cannot create a staging directory on $HOST"
say "copying to $HOST:$STAGE"
scp "${SSH_OPTS[@]}" -q \
  "$BIN" \
  ${TOOLS[@]+"${TOOLS[@]}"} \
  "$REPO_DIR/variants/linux/meshcored.service" \
  "$HOST:$STAGE/"

# --- 4. set up and install ----------------------------------------------------
#
# Idempotent setup, then the binaries and a restart.
#
# The setup runs first so that a sudo which cannot work without a terminal fails
# on `groupadd` -- harmless, and before anything has been stopped. On an
# already-provisioned node the setup is a no-op, so the first thing that can
# fail is the stop, which falls into its `|| true` and lets the `install` after
# it abort the script -- leaving the node running the build it already had.
#
# A node that is not provisioned yet fails at the verification step with its own
# log, which is a better diagnosis than anything this script could guess at.
#
# A failure *after* the stop (below) is different: the node is left stopped,
# not merely "not yet updated", and with nothing said about why. The ERR trap
# in the remote script covers exactly that gap -- it fires on any command that
# would otherwise end the script silently under `set -e`, names the line that
# failed, says plainly that meshcored may now be down, and removes the stage
# dir so a retry does not accumulate them.

say "installing"
rsh "STAGE='$STAGE' bash -s" <<'REMOTE' || die "remote install failed -- see the diagnosis above; meshcored may be left stopped on $HOST"
set -euo pipefail

trap 'ec=$?; echo "   !! install step failed (exit $ec) at line $LINENO -- meshcored may be stopped; removing $STAGE" >&2; rm -rf "$STAGE"' ERR

# --- setup, all idempotent ---------------------------------------------------
# Modelled on a node that works. Each step is a no-op once it has been done, so
# this is silent on every deploy after the first.

getent group meshcore >/dev/null || {
  echo "   creating meshcore group"
  sudo groupadd -r meshcore
}

id -u meshcore >/dev/null 2>&1 || {
  echo "   creating meshcore user"
  sudo useradd -r -g meshcore -s /sbin/nologin meshcore
}

# Device access by joining the groups the distro's own udev rules already grant,
# never by claiming the devices. spi and gpio are the radio; dialout is a serial
# GPS. Skipped silently where a distro does not define the group -- there is
# nothing to join, and inventing one would put us back to fighting over
# ownership. A group that exists but grants nothing shows up at the verify step
# as the daemon's own "cannot claim GPIO" error.
#
# dialout is joined unconditionally even though a gpsd:// GPS does not need it
# (gpsd holds the device; the daemon only opens a localhost socket). Which of
# the two a node uses is in its meshcored.ini, which this script deliberately
# does not read -- and being in a group grants nothing on a node with no serial
# GPS, so the harmless case is the one to default to.
#
# Note i2c is deliberately absent: the working node does not put meshcore in it,
# and this should grant what is observed to be needed, not what might be.
for g in spi gpio dialout; do
  getent group "$g" >/dev/null || continue
  id -nG meshcore 2>/dev/null | grep -qw "$g" && continue
  echo "   adding meshcore to group $g"
  sudo usermod -aG "$g" meshcore
done

# Only when absent, and never overwritten: a deployed node may have local edits,
# or a drop-in that assumes the installed version.
unit_installed=no
if [ ! -f /etc/systemd/system/meshcored.service ]; then
  echo "   installing systemd unit"
  sudo install -m 644 "$STAGE/meshcored.service" /etc/systemd/system/
  sudo systemctl daemon-reload
  unit_installed=yes
fi

# --- swap the binaries -------------------------------------------------------

# Stop before replacing the binary: overwriting the file under a running process
# is allowed on Linux but leaves the old code running, so the deploy would look
# done while the node still ran the previous build.
sudo systemctl stop meshcored 2>/dev/null || true

# Spelled out rather than `[ -f ... ] && cp`, which is a false test on a first
# install -- and under `set -e` a compound command returning non-zero would end
# the script right here.
if [ -f /usr/bin/meshcored ]; then
  sudo cp -a /usr/bin/meshcored /usr/bin/meshcored.prev
fi

sudo install -m 755 "$STAGE/meshcored" /usr/bin/meshcored

# The CLI client, if it was shipped (see the scp above).
for t in meshcorectl; do
  [ -f "$STAGE/$t" ] || continue
  sudo install -m 755 "$STAGE/$t" "/usr/bin/$t"
done

# `start`, not `enable --now`: whether an existing node comes up at boot is its
# own decision, and a deploy has no business revising it. The exception is a
# node that had no unit at all until a moment ago -- there is no prior choice to
# respect there, and leaving a freshly provisioned node that does not survive a
# reboot would be a trap.
if [ "$unit_installed" = yes ]; then
  sudo systemctl enable meshcored
fi
sudo systemctl start meshcored
rm -rf "$STAGE"
REMOTE

# --- 5. verify ----------------------------------------------------------------
#
# The real check, and the reason step 1 does not need to predict much. The
# firmware validates its config at startup and exits on an invalid value, and
# Restart=on-failure means systemd will keep retrying, so "started" is not the
# same as "running". Whatever went wrong -- no config, a bad pin, a binary for
# the wrong architecture -- surfaces here as the node's own words.

say "verifying"
sleep 5
if rsh 'systemctl is-active --quiet meshcored'; then
  say "meshcored is active on $HOST"
  rsh 'systemctl --no-pager --lines=8 status meshcored' || true
else
  printf '\n'
  say "FAILED: meshcored is not active on $HOST -- its log follows"
  rsh 'journalctl -u meshcored --no-pager --lines=40' || true
  die "deploy did not come up.
  If this was a first deploy, the likeliest cause is a missing or invalid
  /etc/meshcored/meshcored.ini -- this script ships code, not configuration.
  To roll back:
    ssh $HOST 'sudo mv /usr/bin/meshcored.prev /usr/bin/meshcored && sudo systemctl restart meshcored'"
fi
