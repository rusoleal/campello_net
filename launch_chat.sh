#!/bin/bash
# Launch script for the campello_net chat example.
#
# Usage:
#   ./launch_chat.sh              Quick demo: starts server + one client
#   ./launch_chat.sh server       Start server only
#   ./launch_chat.sh client       Start client only (prompts for username)
#   ./launch_chat.sh client NAME  Start client with given username

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BIN="${BUILD_DIR}/bin/chat_example"
PORT="${CHAT_PORT:-7777}"
HOST="${CHAT_HOST:-127.0.0.1}"

# Build if binary is missing
if [[ ! -x "$BIN" ]]; then
    echo "[launch_chat] chat_example not found, building..."
    cmake -B "$BUILD_DIR" -DCAMPELLO_NET_BUILD_EXAMPLES=ON -DCAMPELLO_NET_BUILD_TESTS=OFF >/dev/null 2>&1
    cmake --build "$BUILD_DIR" --target chat_example --parallel >/dev/null 2>&1
    echo "[launch_chat] Build complete."
fi

MODE="${1:-demo}"

CLEANUP_DONE=0
cleanup() {
    if [[ "$CLEANUP_DONE" -eq 1 ]]; then
        return
    fi
    CLEANUP_DONE=1
    echo ""
    echo "[launch_chat] Shutting down..."
    jobs -p | xargs kill 2>/dev/null || true
    wait 2>/dev/null || true
    exit 0
}
trap cleanup INT TERM EXIT

launch_server() {
    echo "[launch_chat] Starting server on port $PORT"
    "$BIN" --server "$PORT" &
}

launch_client() {
    local username="${1:-}"
    if [[ -z "$username" ]]; then
        read -rp "[launch_chat] Enter username: " username
    fi
    if [[ -z "$username" ]]; then
        username="user$$"
    fi
    echo "[launch_chat] Starting client '$username' connecting to $HOST:$PORT"
    "$BIN" --client "$HOST" "$PORT" "$username"
}

case "$MODE" in
    server|s)
        launch_server
        wait
        ;;
    client|c)
        launch_client "${2:-}"
        ;;
    demo|d|"")
        launch_server
        sleep 1
        echo ""
        echo "═══════════════════════════════════════════════════"
        echo "  campello_net chat demo"
        echo "  Server: $HOST:$PORT"
        echo "  Type messages and press Enter."
        echo "  Type /quit to exit."
        echo "═══════════════════════════════════════════════════"
        echo ""
        launch_client "${2:-demo_user}"
        ;;
    *)
        echo "Usage: $0 [server|client [NAME]|demo [NAME]]"
        echo ""
        echo "Modes:"
        echo "  server       Start the chat server only"
        echo "  client NAME  Start a chat client (prompts if no name given)"
        echo "  demo NAME    Start server + client for a quick demo (default)"
        exit 1
        ;;
esac
