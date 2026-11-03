#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE=(docker compose -f "$ROOT/testbed/docker-compose.yml")
RUN="$ROOT/testbed/.run"
SECRET_FILE="$RUN/docker-client-password"
DEFAULT_CERT_DIR="$ROOT/testbed/tls/certs"

case "${1:-}" in
  start)
    mkdir -p "$RUN"
    if [ -z "${PROXY_GATEWAY_CLIENT_PASSWORD:-}" ]; then
      if [ ! -s "$SECRET_FILE" ]; then
        (umask 077; python3 -c 'import secrets; print(secrets.token_urlsafe(32))' >"$SECRET_FILE")
      fi
      chmod 600 "$SECRET_FILE"
      IFS= read -r PROXY_GATEWAY_CLIENT_PASSWORD <"$SECRET_FILE"
      echo "Using local client credentials (username: pricelens); password stored under testbed/.run/"
    fi
    if [ "${#PROXY_GATEWAY_CLIENT_PASSWORD}" -lt 16 ]; then
      echo "PROXY_GATEWAY_CLIENT_PASSWORD must contain at least 16 characters" >&2
      exit 2
    fi
    export PROXY_GATEWAY_CLIENT_PASSWORD
    export TLS_BIND="${TLS_BIND:-127.0.0.1}"
    export TLS_CERT_DIR="${TLS_CERT_DIR:-$DEFAULT_CERT_DIR}"
    if [[ "$TLS_CERT_DIR" != /* ]]; then TLS_CERT_DIR="$ROOT/$TLS_CERT_DIR"; export TLS_CERT_DIR; fi
    mkdir -p "$TLS_CERT_DIR"
    if [ ! -r "$TLS_CERT_DIR/fullchain.pem" ] || [ ! -r "$TLS_CERT_DIR/privkey.pem" ]; then
      if [ "$TLS_CERT_DIR" != "$DEFAULT_CERT_DIR" ]; then
        echo "TLS_CERT_DIR must contain readable fullchain.pem and privkey.pem" >&2
        exit 2
      fi
      if ! command -v openssl >/dev/null 2>&1; then
        echo "openssl is required to create a local self-signed demo certificate" >&2
        exit 2
      fi
      openssl req -x509 -newkey rsa:3072 -sha256 -days 30 -nodes \
        -keyout "$TLS_CERT_DIR/privkey.pem" -out "$TLS_CERT_DIR/fullchain.pem" \
        -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
      chmod 600 "$TLS_CERT_DIR/privkey.pem"
      chmod 644 "$TLS_CERT_DIR/fullchain.pem"
      echo "Created a self-signed localhost certificate for the demo only."
    fi
    "${COMPOSE[@]}" --profile tls up -d --build
    echo "TLS proxy ready at https://localhost:8443 (admin dashboard remains at http://127.0.0.1:9000/ui)"
    ;;
  stop)
    "${COMPOSE[@]}" --profile tls down
    rm -f "$SECRET_FILE"
    ;;
  *)
    echo "usage: scripts/docker-demo-tls.sh start|stop" >&2
    exit 2
    ;;
esac
