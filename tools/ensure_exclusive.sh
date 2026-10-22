#!/bin/bash
# ensure_exclusive.sh <target: strata|maya>
# Safely unloads the peer model to avoid VRAM / RAM collisions when switching models.
TARGET="$1"

if [ "$TARGET" = "strata" ]; then
    PORT=8081
    SERVICE="strata.service"
    PROC_PATTERN="(Strata.*/strata|strata.*--pack)"
elif [ "$TARGET" = "maya" ]; then
    PORT=8082
    SERVICE="maya.service"
    PROC_PATTERN="(project-maya.*/(maya|strata)|strata.*--glm-pack)"
else
    echo "Unknown target: $TARGET"
    echo "Usage: $0 <strata|maya>"
    exit 1
fi

# Detect API Key from environment or local config if available
if [ -n "$STRATA_API_KEY" ]; then
    API_KEY="$STRATA_API_KEY"
else
    API_KEY="sk-strata-d6ce44efa3aeabdb06fb64742866d932"
fi

if pgrep -f "$PROC_PATTERN" > /dev/null; then
    echo "[exclusive] Target $TARGET is running; requesting unload on port $PORT..."
    curl -s -m 5 -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $API_KEY" -d '{}' "http://127.0.0.1:${PORT}/unload" > /dev/null 2>&1
    sleep 1
    if pgrep -f "$PROC_PATTERN" > /dev/null; then
        echo "[exclusive] Target $TARGET still running; restarting $SERVICE..."
        systemctl --user restart "$SERVICE" 2>/dev/null || true
        sleep 1
    fi
    if pgrep -f "$PROC_PATTERN" > /dev/null; then
        echo "[exclusive] Target $TARGET engine still present; terminating process..."
        pkill -9 -f "$PROC_PATTERN" 2>/dev/null || true
        sleep 1
    fi
    echo "[exclusive] Target $TARGET unloaded successfully."
else
    echo "[exclusive] Target $TARGET is already unloaded."
fi
