#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root_dir"

build_dir="${PTT_BUILD_DIR:-build-wsl}"

cmake -S . -B "$build_dir"
cmake --build "$build_dir" -j

mkdir -p logs
: > logs/server.log
: > logs/client_001.log
: > logs/client_002.log
: > logs/client_003.log

"./$build_dir/ptt_server" --run-seconds 8 > logs/server.log 2>&1 &
server_pid=$!
sleep 0.3

"./$build_dir/ptt_client" \
  --server 127.0.0.1 \
  --id client_002 --group group_01 --sip-port 15072 --rtp-port 20002 \
  --run-seconds 6 --auto-talk-delay-ms 1500 --auto-talk-ms 500 --simulate-opus \
  > logs/client_002.log 2>&1 &
client_2_pid=$!

"./$build_dir/ptt_client" \
  --server 127.0.0.1 \
  --id client_003 --group group_01 --sip-port 15073 --rtp-port 20003 \
  --run-seconds 6 --simulate-opus > logs/client_003.log 2>&1 &
client_3_pid=$!

"./$build_dir/ptt_client" \
  --server 127.0.0.1 \
  --id client_001 --group group_01 --sip-port 15071 --rtp-port 20001 \
  --run-seconds 6 --auto-talk-delay-ms 1000 --auto-talk-ms 2000 --simulate-opus \
  > logs/client_001.log 2>&1 &
client_1_pid=$!

wait "$client_1_pid" "$client_2_pid" "$client_3_pid" "$server_pid"

echo "=== server ==="
cat logs/server.log
echo "=== client_001 (speaker) ==="
cat logs/client_001.log
echo "=== client_002 (listener) ==="
cat logs/client_002.log
echo "=== client_003 (listener) ==="
cat logs/client_003.log

