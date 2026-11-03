#!/usr/bin/env bash
# Applies / reverts a sysctl profile for high-concurrency proxying.
#   ./scripts/kernel_tune.sh apply|revert
# Env: BACKUP_FILE (default /tmp/gateway_sysctl_backup.conf)
set -euo pipefail

BACKUP_FILE="${BACKUP_FILE:-/tmp/gateway_sysctl_backup.conf}"
KEYS=(net.core.somaxconn net.ipv4.tcp_tw_reuse net.ipv4.ip_local_port_range net.ipv4.tcp_fin_timeout net.core.netdev_max_backlog)

SUDO=""
if [[ "${EUID}" -ne 0 ]]; then
    if command -v sudo >/dev/null 2>&1; then SUDO="sudo"; else
        echo "[KernelTune] Must run as root (sudo not found)." >&2; exit 1
    fi
fi

norm() { tr -s '[:space:]' ' ' | sed 's/^ //; s/ $//'; }

apply_tuning() {
    if [[ -f "${BACKUP_FILE}" ]]; then
        echo "[KernelTune] ${BACKUP_FILE} already exists: tuning looks applied. Run '$0 revert' first." >&2
        exit 1
    fi
    echo "[KernelTune] Saving current values to ${BACKUP_FILE}..."
    : > "${BACKUP_FILE}"
    for k in "${KEYS[@]}"; do
        v="$(sysctl -n "$k" | norm)"
        printf '%s=%s\n' "$k" "$v" >> "${BACKUP_FILE}"
    done

    echo "[KernelTune] Applying high-concurrency profile..."
    ${SUDO} sysctl -w net.core.somaxconn=4096
    ${SUDO} sysctl -w net.ipv4.tcp_tw_reuse=1
    ${SUDO} sysctl -w net.ipv4.ip_local_port_range="10240 65535"
    ${SUDO} sysctl -w net.ipv4.tcp_fin_timeout=15
    ${SUDO} sysctl -w net.core.netdev_max_backlog=8192
    echo "[KernelTune] Tuning applied. Undo with: $0 revert"
}

revert_tuning() {
    if [[ ! -f "${BACKUP_FILE}" ]]; then
        echo "[KernelTune] No backup found at ${BACKUP_FILE}; nothing to revert (a reboot also restores defaults)." >&2
        exit 1
    fi
    echo "[KernelTune] Restoring values from ${BACKUP_FILE}..."
    while IFS='=' read -r key val; do
        [[ -z "${key}" ]] && continue
        ${SUDO} sysctl -w "${key}=${val}"
    done < "${BACKUP_FILE}"
    rm -f "${BACKUP_FILE}"
    echo "[KernelTune] Reverted to previous values."
}

case "${1:-}" in
    apply) apply_tuning ;;
    revert) revert_tuning ;;
    *) echo "Usage: $0 {apply|revert}"; exit 1 ;;
esac
