#!/usr/bin/env bash
# Launch the frontend with full logging from both the Rust frontend and
# the Vulkan layer going to the same file, tee'd to your terminal.
#
# Always performs pre-flight cleanup (stale processes + socket) and an
# after-exit cleanup (kill the frontend we launched, remove our socket),
# so repeated runs don't leave garbage behind.
#
# Usage:
#   ./scripts/run.sh               # info level everywhere
#   ./scripts/run.sh debug         # layer + rust at debug
#   ./scripts/run.sh trace         # everything loud

set -eu

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$REPO/frontend/target/debug/lumen"
LOG="${LUMEN_LOG:-/tmp/lumen.log}"
SOCKET="${XDG_RUNTIME_DIR:-/tmp}/lumen.sock"

# The implicit-layer manifest at ~/.local/share/vulkan/implicit_layer.d/
# points at this absolute path. Builds land in layer/builddir; we copy
# from there to here so freshly-built shaders/code actually load.
LAYER_SRC="$REPO/layer/builddir/src/liblumen.so"
LAYER_DST="$HOME/.local/lib/liblumen.so"
MANIFEST_DIR="$HOME/.local/share/vulkan/implicit_layer.d"
MANIFEST_DST="$MANIFEST_DIR/lumen.json"

case "${1:-info}" in
  trace) LAYER_LEVEL=trace ; RUST_LEVEL=trace ;;
  debug) LAYER_LEVEL=debug ; RUST_LEVEL=debug ;;
  info)  LAYER_LEVEL=info  ; RUST_LEVEL=info  ;;
  warn)  LAYER_LEVEL=warn  ; RUST_LEVEL=warn  ;;
  *)     LAYER_LEVEL=info  ; RUST_LEVEL=info  ;;
esac

preflight_cleanup() {
  # Kill any frontend left over from a previous run (signal-safe — if none,
  # pkill returns 1 which is fine). Use `-x lumen` to match on exact process
  # name (the binary's argv[0] basename); a looser substring match on "lumen"
  # would hit random unrelated processes that happen to have the word
  # somewhere in their cmdline.
  pkill -TERM -x lumen 2>/dev/null || true
  # Give it up to 2s to exit gracefully, then force-kill any strays.
  for _ in 1 2 3 4; do
    pgrep -x lumen >/dev/null 2>&1 || break
    sleep 0.5
  done
  pkill -KILL -x lumen 2>/dev/null || true
  # Remove the socket; the layer IPC won't attempt to bind if a stale file
  # was left from an ungraceful exit.
  rm -f "$SOCKET"
  # Remove legacy layer .so + manifest from the pre-Lumen naming so the
  # Vulkan loader doesn't register both and load the absent old library.
  rm -f "$HOME/.local/lib/libgamefiltersflatpak.so" \
        "$HOME/.local/share/vulkan/implicit_layer.d/game-filters-flatpak.json" \
        "$HOME/.local/share/vulkan/implicit_layer.d/nvidia-filters-flatpak.json" \
        "$HOME/.local/lib/libnvidiafiltersflatpak.so"
}

postflight_cleanup() {
  # Kill the frontend we started (if still alive).
  if [[ -n "${FRONT_PID:-}" ]]; then
    kill -TERM "$FRONT_PID" 2>/dev/null || true
    sleep 0.5
    kill -KILL "$FRONT_PID" 2>/dev/null || true
  fi
  # Kill the tail we started.
  if [[ -n "${TAIL_PID:-}" ]]; then
    kill "$TAIL_PID" 2>/dev/null || true
  fi
  # Remove the socket (frontend's Drop impl already does this on clean exit;
  # this covers crashes and SIGKILLs).
  rm -f "$SOCKET"
  echo ""
  echo "[run.sh] cleaned up: killed frontend, removed $SOCKET"
}

preflight_cleanup

# Build the layer (in distrobox where the dev headers live), copy the .so
# to where the implicit-layer manifest expects it, then build the frontend.
# Without this step you would silently test against whatever .so was last
# manually copied to ~/.local/lib — usually stale.
echo "[run.sh] building layer in distrobox…"
distrobox enter fedora-dev -- meson compile -C "$REPO/layer/builddir"
mkdir -p "$(dirname "$LAYER_DST")"
# Atomic replace. A plain `cp` truncates the destination file; any
# currently-running Vulkan process has the .so mmap'd MAP_SHARED and
# will SIGBUS the next time it touches a page of the truncated .text.
# `install` writes to a temp file and rename(2)'s it over — old inode
# stays alive for running processes, new processes pick up the new one.
install -m 755 "$LAYER_SRC" "$LAYER_DST"
echo "[run.sh] installed $(stat -c '%y  %s bytes' "$LAYER_DST") → $LAYER_DST"

# Write the implicit-layer manifest with an absolute library_path. The
# in-tree template under layer/config/ ships a bare filename for Flatpak
# installs (where the .so lands in standard lib dirs); for dev installs
# under ~/.local/lib/ the Vulkan loader has no search path convention
# that finds it, so we hardcode the absolute path here. Idempotent — we
# just overwrite on every launch so stale paths can't drift.
mkdir -p "$MANIFEST_DIR"
cat > "$MANIFEST_DST" <<EOF
{
  "file_format_version" : "1.0.0",
  "layer" : {
    "name": "VK_LAYER_LUMEN_post_processing",
    "type": "GLOBAL",
    "library_path": "$LAYER_DST",
    "api_version": "1.4.341",
    "implementation_version": "1",
    "description": "Lumen post-processing layer (dev install)",
    "functions": {
      "vkGetInstanceProcAddr": "lumen_GetInstanceProcAddr",
      "vkGetDeviceProcAddr":   "lumen_GetDeviceProcAddr"
    },
    "disable_environment": {
      "LUMEN_DISABLE": "1"
    }
  }
}
EOF
echo "[run.sh] wrote dev manifest → $MANIFEST_DST"

echo "[run.sh] building frontend in distrobox…"
distrobox enter fedora-dev -- cargo build --manifest-path "$REPO/frontend/Cargo.toml"

: > "$LOG"

echo "frontend binary: $BIN"
echo "log file:        $LOG"
echo "socket:          $SOCKET"
echo "layer level:     $LAYER_LEVEL"
echo "rust level:      $RUST_LEVEL"
echo ""
echo "If a KDE portal dialog appears asking to allow Lumen to"
echo "register a global shortcut — click ALLOW. If you dismiss it, the"
echo "hotkey won't work this session."
echo ""
echo "To capture a Steam game's layer output into the same log file, add"
echo "this to the game's Launch Options in Steam (one-time setup):"
echo "  LUMEN_LOG_FILE=$LOG LUMEN_LOG_LEVEL=$LAYER_LEVEL %command%"
echo "---"

export LUMEN_LOG_FILE="$LOG"
export LUMEN_LOG_LEVEL="$LAYER_LEVEL"
export RUST_LOG="$RUST_LEVEL"
export RUST_BACKTRACE=1

# tail -F the log in the background.
tail -n +1 -F "$LOG" &
TAIL_PID=$!

# EXIT trap fires regardless of how the script exits (Ctrl+C, kill, normal),
# so cleanup always runs.
trap postflight_cleanup EXIT INT TERM

# Start the frontend in the background so we have its PID for cleanup.
"$BIN" >> "$LOG" 2>&1 &
FRONT_PID=$!

wait "$FRONT_PID"
