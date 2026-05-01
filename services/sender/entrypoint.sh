#!/usr/bin/env bash
set -euo pipefail

port="${SENDER_PORT:-9001}"

# Keep a long-running listener for service health until benchmark binaries are wired in.
while true; do
  nc -lk -p "$port" -q 1 >/dev/null 2>&1 || true
done
