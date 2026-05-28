#!/usr/bin/env bash
set -euo pipefail

PEERS=${1:-2}
DATAFILE=${2:-$(dirname "$0")/dados/file_a_10kb.bin}
BLOCK_SIZE=${3:-1024}
BASEPORT=5000
PEER_EXE=./p2p_peer
LOGDIR=$(dirname "$0")/logs
DATADIR=$(dirname "$DATAFILE")

mkdir -p "$LOGDIR" "$DATADIR"

if [ ! -x "$PEER_EXE" ]; then
  echo "Executable $PEER_EXE not found or not executable. Build the project first." >&2
  exit 1
fi

if [ "$PEERS" -ne 2 ] && [ "$PEERS" -ne 4 ]; then
  echo "Peers must be 2 or 4" >&2
  exit 1
fi

if [ ! -f "$DATAFILE" ]; then
  echo "Generating data file: $DATAFILE (size: 10KB)"
  dd if=/dev/urandom of="$DATAFILE" bs=1024 count=10 status=none
fi

META="$DATAFILE.meta"
SEEDER_PORT=$BASEPORT
LEECHER_PORTS=()
for i in $(seq 1 $((PEERS - 1))); do
  LEECHER_PORTS+=("$((BASEPORT + i))")
done

SEEDER_LOG="$LOGDIR/peer_0_seeder.log"
SEEDER_ARGS=(--listen "127.0.0.1:$SEEDER_PORT" --input "$DATAFILE" --block-size "$BLOCK_SIZE" --meta "$META")
for p in "${LEECHER_PORTS[@]}"; do
  SEEDER_ARGS+=(--peer "127.0.0.1:$p")
done

echo "Starting seeder on port $SEEDER_PORT -> log $SEEDER_LOG"
"$PEER_EXE" "${SEEDER_ARGS[@]}" >"$SEEDER_LOG" 2>&1 &
SEEDER_PID=$!

# give seeder a moment to create meta and listen
sleep 0.5

LEECHER_PIDS=()
INDEX=1
for p in "${LEECHER_PORTS[@]}"; do
  OUTFILE="$LOGDIR/peer_${INDEX}_download.bin"
  PEER_LOG="$LOGDIR/peer_${INDEX}.log"
  ARGS=(--listen "127.0.0.1:$p" --meta "$META" --output "$OUTFILE" --peer "127.0.0.1:$SEEDER_PORT" --block-size "$BLOCK_SIZE" --stop-on-complete)
  echo "Starting leecher #$INDEX on port $p -> log $PEER_LOG"
  "$PEER_EXE" "${ARGS[@]}" >"$PEER_LOG" 2>&1 &
  LEECHER_PIDS+=("$!")
  INDEX=$((INDEX + 1))
done

# wait leechers
for pid in "${LEECHER_PIDS[@]}"; do
  wait "$pid" || true
done

# stop seeder
if kill -0 "$SEEDER_PID" 2>/dev/null; then
  kill "$SEEDER_PID" || true
  wait "$SEEDER_PID" 2>/dev/null || true
fi

# verification
echo
echo "Verification:"
ORIG_SHA=$(sha256sum "$DATAFILE" | awk '{print $1}')
printf "Original: %s  %s\n" "$ORIG_SHA" "$DATAFILE"

PASS_COUNT=0
TOTAL=0
INDEX=1
for p in "${LEECHER_PORTS[@]}"; do
  OUTFILE="$LOGDIR/peer_${INDEX}_download.bin"
  if [ -f "$OUTFILE" ]; then
    DOWN_SHA=$(sha256sum "$OUTFILE" | awk '{print $1}')
    printf "Peer %d: %s  %s\n" "$INDEX" "$DOWN_SHA" "$OUTFILE"
    if [ "$DOWN_SHA" = "$ORIG_SHA" ]; then
      PASS_COUNT=$((PASS_COUNT + 1))
    fi
  else
    printf "Peer %d: MISSING %s\n" "$INDEX" "$OUTFILE"
  fi
  TOTAL=$((TOTAL + 1))
  INDEX=$((INDEX + 1))
done

echo
if [ "$PASS_COUNT" -eq "$TOTAL" ]; then
  echo "All downloads verified: $PASS_COUNT/$TOTAL"
  exit 0
else
  echo "Some downloads failed verification: $PASS_COUNT/$TOTAL"
  exit 2
fi
