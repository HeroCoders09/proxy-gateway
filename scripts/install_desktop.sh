#!/usr/bin/env bash
# Installs / removes the "Proxy Gateway" desktop app launcher for the current user.
#   bash scripts/install_desktop.sh install|uninstall
# Builds the gateway first if build/gateway is missing (install only).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
ICONS="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/scalable/apps"
BIN="${XDG_BIN_HOME:-$HOME/.local/bin}"

case "${1:-install}" in
install)
    if [[ ! -x "$ROOT/build/gateway" ]]; then
        echo "[Install] Building gateway..."
        mkdir -p "$ROOT/build" && (cd "$ROOT/build" && cmake .. >/dev/null && make -j"$(nproc)" gateway proxy_node target_server >/dev/null)
    fi
    command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 1; }
    mkdir -p "$APPS" "$ICONS" "$BIN"
    cp "$ROOT/app/proxy-gateway.svg" "$ICONS/proxy-gateway.svg"
    cat > "$BIN/proxy-gateway-ui" <<EOF
#!/usr/bin/env bash
exec python3 "$ROOT/app/gateway_ui.py" --bin "$ROOT/build/gateway" "\$@"
EOF
    chmod 755 "$BIN/proxy-gateway-ui"
    cat > "$APPS/proxy-gateway.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Proxy Gateway
Comment=Start, configure and monitor the intelligent proxy gateway
Exec=$BIN/proxy-gateway-ui
Icon=proxy-gateway
Terminal=false
Categories=Network;System;
StartupWMClass=proxy-gateway
EOF
    command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" 2>/dev/null || true
    echo "[Install] Done. Launch 'Proxy Gateway' from your app menu, or run: $BIN/proxy-gateway-ui"
    ;;
uninstall)
    rm -f "$APPS/proxy-gateway.desktop" "$ICONS/proxy-gateway.svg" "$BIN/proxy-gateway-ui"
    echo "[Install] Removed launcher. Config/logs kept in ~/.config/proxy-gateway and ~/.local/state/proxy-gateway."
    ;;
*) echo "usage: $0 install|uninstall" >&2; exit 2 ;;
esac
