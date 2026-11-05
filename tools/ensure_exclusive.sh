#!/bin/bash
# ensure_exclusive.sh <caller: strata|maya|maya-ds4|strata-ds4|maya-m-derisked|maya-l>
# Safely unloads all OTHER models to avoid VRAM / RAM collisions when switching models.
CALLER="$1"

API_KEY="${STRATA_API_KEY:-sk-strata-d6ce44efa3aeabdb06fb64742866d932}"

SERVICES=(
    "strata:8081:strata:strata-host"
    "maya:8082:maya:"
    "maya-ds4:8083:maya-ds4:"
    "strata-ds4:8084:strata-dsv41:"
    "maya-m-derisked:8085:maya:"
    "maya-l:8086:maya:"
)

unload_service() {
    local NAME="$1"
    local PORT="$2"
    local BINARY="$3"
    local ALT_BINARY="$4"

    # Always request unload via HTTP API first (safe, clean, flushes slots)
    curl -s -m 5 -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $API_KEY" -d '{}' "http://127.0.0.1:${PORT}/unload" > /dev/null 2>&1 || true

    for B in "$BINARY" "$ALT_BINARY"; do
        if [ -n "$B" ] && pgrep -x "$B" > /dev/null 2>&1; then
            sleep 1
            if pgrep -x "$B" > /dev/null 2>&1; then
                echo "[exclusive] Target $NAME engine ($B) still present; terminating binary..."
                pkill -9 -x "$B" 2>/dev/null || true
                sleep 2
            fi
        fi
    done
}

for SVC in "${SERVICES[@]}"; do
    IFS=":" read -r S_NAME S_PORT S_BIN S_ALT <<< "$SVC"
    # Match caller (allow alias like ds4 for maya-ds4)
    if [ "$CALLER" = "ds4" ] && [ "$S_NAME" = "maya-ds4" ]; then
        continue
    fi
    if [ "$S_NAME" != "$CALLER" ]; then
        unload_service "$S_NAME" "$S_PORT" "$S_BIN" "$S_ALT"
    fi
done

sleep 2

# HugePages management: Strata benefits from 2 MiB HugePages (host expert arena),
# while Maya / DS4 and other engines use standard pinned RAM / mmap and need all host RAM available.
STRATA_HUGEPAGES_TARGET="${STRATA_HUGEPAGES:-24000}"

set_hugepages() {
    local TARGET_PAGES="$1"
    local CURRENT_PAGES
    CURRENT_PAGES=$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo -1)

    if [ "$CURRENT_PAGES" = "$TARGET_PAGES" ]; then
        echo "[exclusive] HugePages already set to $TARGET_PAGES"
        return 0
    fi

    echo "[exclusive] Adjusting HugePages from $CURRENT_PAGES to $TARGET_PAGES..."
    if (echo "$TARGET_PAGES" > /proc/sys/vm/nr_hugepages) 2>/dev/null; then
        echo "[exclusive] Successfully set nr_hugepages to $TARGET_PAGES"
        return 0
    fi

    # Fallback to passwordless sudo if available
    if sudo -n tee /proc/sys/vm/nr_hugepages <<< "$TARGET_PAGES" >/dev/null 2>&1 || \
       sudo -n sysctl -w vm.nr_hugepages="$TARGET_PAGES" >/dev/null 2>&1; then
        echo "[exclusive] Successfully set nr_hugepages to $TARGET_PAGES (via sudo)"
        return 0
    fi

    echo "[exclusive] WARNING: Failed to set nr_hugepages to $TARGET_PAGES (permission denied)."
    echo "[exclusive] Run once: sudo chmod 666 /proc/sys/vm/nr_hugepages"
    return 1
}

case "$CALLER" in
    strata|strata-ds4)
        set_hugepages "$STRATA_HUGEPAGES_TARGET"
        ;;
    *)
        set_hugepages 0
        ;;
esac

echo "[exclusive] All other engines unloaded; ready for $CALLER."
