#!/usr/bin/env bash
# Removes the service and the Flatpak. Keeps ~/.config/lhctl unless you pass --purge.
set -uo pipefail
systemctl --user disable --now lhctl.service 2>/dev/null
rm -f ~/.config/systemd/user/lhctl.service
systemctl --user daemon-reload
flatpak uninstall --user -y com.phoenixnet_labs.LighthouseCtl
[[ "${1:-}" == "--purge" ]] && rm -rf ~/.config/lhctl
echo "Removed."
