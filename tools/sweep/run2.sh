#!/bin/bash
# run2.sh SLOTS AI_EVERY REPS DURATION DRAIN
set -e
SLOTS=$1; AIE=$2; REPS=$3; DUR=$4; DRAIN=$5
BIN=/tmp/soak_s${SLOTS}_a${AIE}
gcc -std=c99 -Wall -Wextra -O2 -D_GNU_SOURCE -Iinclude \
    -DVENC_FRAME_QUEUE_SLOTS=${SLOTS}u -DVENC_CODEL_AI_EVERY=${AIE}u \
    tools/unix_pacing_soak.c src/venc_frame_queue.c src/venc_codel.c src/output_socket.c \
    -o $BIN 2>/dev/null
for r in $(seq 1 $REPS); do
  ./tests/unix_dgram_consumer_host --name soaksweep --duration $((DUR+3)) --drain-kbps $DRAIN >/dev/null 2>&1 &
  CPID=$!; sleep 0.4
  $BIN --name soaksweep --duration $DUR --kbps 15000 --fps 60 --throttle 1 2>/dev/null | tail -1
  wait $CPID 2>/dev/null || true; sleep 0.5
done
