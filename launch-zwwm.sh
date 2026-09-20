#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
portal_root="${XDG_DATA_HOME:-$HOME/.local/share}/xdg-desktop-portal"
portal_dir="$portal_root/portals"

mkdir -p "$portal_dir"
install -m 0644 "$root/xdg-desktop-portal-zwwm/data/zwwm.portal" "$portal_dir/zwwm.portal"
install -m 0644 "$root/xdg-desktop-portal-zwwm/data/zwwm-gtk.portal" "$portal_dir/zwwm-gtk.portal"
install -m 0644 "$root/xdg-desktop-portal-zwwm/data/zwwm-portals.conf" "$portal_root/zwwm-portals.conf"

systemctl --user set-environment \
  XDG_CURRENT_DESKTOP=zwwm \
  XDG_SESSION_DESKTOP=zwwm \
  NIX_XDG_DESKTOP_PORTAL_DIR="$portal_dir"

if [[ -z "${ZWWM_CONFIG:-}" ]]; then
  user_config="${XDG_CONFIG_HOME:-$HOME/.config}/zwwm/config.zw"
  if [[ -f "$user_config" ]]; then
    ZWWM_CONFIG="$user_config"
  else
    ZWWM_CONFIG="$root/zwwm/data/config.zw"
  fi
fi

exec env -u WAYLAND_DISPLAY -u WAYLAND_DEBUG \
  XDG_CURRENT_DESKTOP=zwwm \
  XDG_SESSION_DESKTOP=zwwm \
  XDG_SESSION_TYPE=wayland \
  ZWWM_CONFIG="$ZWWM_CONFIG" \
  nix develop "$root" --no-write-lock-file --command \
  "$root/build/zwwm/zwwm" \
  --portal-helper "$root/build/xdg-desktop-portal-zwwm/xdg-desktop-portal-zwwm"
