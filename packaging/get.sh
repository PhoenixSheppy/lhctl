#!/usr/bin/env bash
# One-line installer / updater for the Steam Frame (Desktop Mode -> Konsole):
#
#   curl -fsSL https://raw.githubusercontent.com/PhoenixSheppy/lhctl/main/packaging/get.sh | bash
#
# Options (after `bash -s --`):
#   --unsandboxed   run the service outside the Flatpak sandbox (if `probe` can't reach SteamVR)
#   --tag TAG       install a specific release instead of the latest (e.g. v0.1.0-build.3)
#
# Downloads the newest release's lhctl.flatpak, checks its SHA-256, installs it for this user,
# and enables a systemd user service so the "Base Stations" tab shows up whenever SteamVR runs.
# Run it again to update. Uninstall: systemctl --user disable --now lhctl;
#   rm ~/.config/systemd/user/lhctl.service; flatpak uninstall --user com.phoenixnet_labs.LighthouseCtl
set -euo pipefail

REPO="PhoenixSheppy/lhctl"
APP="com.phoenixnet_labs.LighthouseCtl"
unsandboxed=false
tag=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --unsandboxed) unsandboxed=true; shift ;;
    --tag) tag="${2:?--tag needs a value}"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

[[ "$(uname -m)" == aarch64 ]] || { echo "this bundle is arm64, for the Steam Frame" >&2; exit 1; }

if [[ -n "$tag" ]]; then
  base="https://github.com/$REPO/releases/download/$tag"
else
  base="https://github.com/$REPO/releases/latest/download"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
echo "==> downloading ${tag:-latest release} from $REPO"
curl -fL --progress-bar -o "$tmp/lhctl.flatpak" "$base/lhctl.flatpak"
curl -fsSL -o "$tmp/lhctl.flatpak.sha256" "$base/lhctl.flatpak.sha256"
(cd "$tmp" && sha256sum -c lhctl.flatpak.sha256) || { echo "checksum mismatch, not installing" >&2; exit 1; }

echo "==> installing (first run also fetches the freedesktop 25.08 runtime)"
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user -y --noninteractive flathub org.freedesktop.Platform//25.08
flatpak install --user -y --noninteractive --reinstall --bundle "$tmp/lhctl.flatpak"

exec_start="/usr/bin/flatpak run $APP run"
if $unsandboxed; then
  exec_start="$HOME/.local/share/flatpak/app/$APP/current/active/files/bin/lhctl run"
fi

echo "==> enabling the background service"
mkdir -p ~/.config/systemd/user
cat > ~/.config/systemd/user/lhctl.service <<EOF
[Unit]
Description=Lighthouse Control (SteamVR base station dashboard tab)

[Service]
ExecStart=$exec_start
Restart=on-failure
RestartSec=5
TimeoutStopSec=40

[Install]
WantedBy=default.target
EOF

# Over SSH (or any shell without a login session) systemctl --user can't find the user bus.
# Point it at the usual place; if the user manager still isn't reachable, enable the unit by
# hand (the same symlink `systemctl enable` makes) so it starts with the next session.
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}"
if systemctl --user show-environment >/dev/null 2>&1; then
  systemctl --user daemon-reload
  systemctl --user enable lhctl.service >/dev/null
  systemctl --user restart lhctl.service
  echo "   service running"
else
  mkdir -p ~/.config/systemd/user/default.target.wants
  ln -sf ../lhctl.service ~/.config/systemd/user/default.target.wants/lhctl.service
  echo "   couldn't reach your systemd user session (SSH, or nobody logged in on the headset)."
  echo "   The service is enabled and starts with your next session. To start it now, open"
  echo "   'Lighthouse Control' from the app menu on the headset."
fi

echo "==> probe (the SteamVR lines only pass while SteamVR is running)"
if $unsandboxed; then
  "${exec_start% run}" probe || true
else
  flatpak run "$APP" probe || true
fi

echo
echo "Done: $(flatpak run "$APP" --version 2>/dev/null || echo installed)."
echo "Open the SteamVR dashboard and look for the 'Base Stations' tab."
echo "Logs: journalctl --user -u lhctl -f"
