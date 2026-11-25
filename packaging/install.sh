#!/usr/bin/env bash
# Installs Lighthouse Control on the Steam Frame (Desktop Mode -> Konsole).
#
#   ./packaging/install.sh                 build from this source tree with flatpak-builder, then install
#   ./packaging/install.sh lhctl.flatpak   install a prebuilt bundle (e.g. from the GitHub Actions build)
#   add --unsandboxed                      service runs the Flatpak's binary directly on the host, outside
#                                          the sandbox (fallback if 'probe' can't reach SteamVR from inside)
#
# Then: probes SteamVR + Bluetooth access and enables a systemd user service so the
# "Base Stations" tab appears whenever SteamVR runs. Undo with packaging/uninstall.sh.
set -euo pipefail

APP=com.phoenixnet_labs.LighthouseCtl
here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
bundle=""
unsandboxed=false
for a in "$@"; do
  case "$a" in
    --unsandboxed) unsandboxed=true ;;
    -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) bundle="$a" ;;
  esac
done

[[ "$(uname -m)" == aarch64 ]] || echo "note: this is not an arm64 machine; building for $(uname -m)"

flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo

if [[ -n "$bundle" ]]; then
  echo "==> installing bundle $bundle"
  flatpak install --user -y --noninteractive flathub org.freedesktop.Platform//25.08
  flatpak install --user -y --noninteractive --bundle "$bundle"
else
  echo "==> building from source (first run downloads the 25.08 SDK, ~few hundred MB)"
  flatpak install --user -y --noninteractive flathub org.flatpak.Builder
  cd "$root"
  flatpak run org.flatpak.Builder --user --install --force-clean --install-deps-from=flathub \
    --state-dir="$root/.flatpak-builder" "$root/flatpak-build" "$here/$APP.yml"
fi

echo "==> probe (SteamVR must be running for the SteamVR lines to pass)"
flatpak run "$APP" probe || echo "   ^ something failed; see README 'Troubleshooting'"

echo "==> enabling the background service"
mkdir -p ~/.config/systemd/user
cp "$here/lhctl.service" ~/.config/systemd/user/lhctl.service
if $unsandboxed; then
  bin="$HOME/.local/share/flatpak/app/$APP/current/active/files/bin/lhctl"   # stable across updates
  echo "   unsandboxed: $bin"
  "$bin" probe || true
  sed -i "s|^ExecStart=.*|ExecStart=$bin run|" ~/.config/systemd/user/lhctl.service
fi
systemctl --user daemon-reload
systemctl --user enable --now lhctl.service
systemctl --user restart lhctl.service

echo
echo "Done. Open the SteamVR dashboard and look for the 'Base Stations' tab."
echo "Logs: journalctl --user -u lhctl -f"
