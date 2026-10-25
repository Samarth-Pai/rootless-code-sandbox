#!/usr/bin/env bash
set -e

echo "=== Building Rootless Code Sandbox ==="
gcc -Wall -Wextra -O2 daemon.c -o daemon

echo "=== Terminating existing daemons ==="
pkill -f "^./daemon" 2>/dev/null || true

echo "=== Launching C Fork-Server Daemon (/tmp/sandbox.sock) ==="
./daemon &
DAEMON_PID=$!

trap "echo 'Shutting down daemon...'; kill $DAEMON_PID 2>/dev/null || true" EXIT INT TERM

sleep 0.4
echo "=== Starting FastAPI Assessment Server on http://0.0.0.0:8000 ==="
python3 app.py
