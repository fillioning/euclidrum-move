#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$SCRIPT_DIR"
MODULE_ID="euclidrum"
COMPONENT_TYPE="midi_fx"
MOVE_HOST="${MOVE_HOST:-root@move.local}"
DEST="/data/UserData/schwung/modules/${COMPONENT_TYPE}/${MODULE_ID}"

echo "=== Installing Euclidrum on Move ==="

if ! ssh "$MOVE_HOST" "echo ok" 2>/dev/null; then
    echo "Error: Cannot reach $MOVE_HOST"
    echo "Enable SSH: http://move.local/development/ssh"
    exit 1
fi

echo "Removing previous installation..."
ssh "$MOVE_HOST" "rm -rf $DEST"

echo "Copying module..."
scp -r "$REPO_ROOT/dist/$MODULE_ID" "$MOVE_HOST:$DEST"

echo "Setting ownership..."
ssh "$MOVE_HOST" "chown -R ableton:users $DEST"

echo ""
echo "=== Installed ==="
echo "Restart Move or run: ssh $MOVE_HOST 'systemctl restart schwung'"
