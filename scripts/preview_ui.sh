#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REQUESTED_PORT="${1:-8080}"

PORT="$(python3 - "${REQUESTED_PORT}" <<'PY'
import socket
import sys

start = int(sys.argv[1])

for port in range(start, start + 50):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(("127.0.0.1", port))
        except OSError:
            continue
        print(port)
        break
else:
    raise SystemExit("no_free_port")
PY
)"

if [[ "${PORT}" != "${REQUESTED_PORT}" ]]; then
  echo "[ui] 端口 ${REQUESTED_PORT} 已被占用，自动切换到 ${PORT}"
fi

echo "[ui] http://127.0.0.1:${PORT}/index.html"
cd "${ROOT_DIR}/ui"
exec python3 -m http.server "${PORT}" --bind 127.0.0.1
