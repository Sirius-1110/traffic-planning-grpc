#!/bin/bash
# Usage: grpc_stream.sh '{"project_id":110,"user_id":1,"case_id":0,"async":1}'
set -euo pipefail
PAYLOAD="${1:-{\"project_id\":110,\"user_id\":1,\"case_id\":0,\"async\":1}}"
MAX_TIME="${GRPC_MAX_TIME:-900}"
FMT="/home/giss/opt_algorithms/tna_total_service/helpers/grpc_stream_fmt.py"
export LANG=zh_CN.UTF-8
export LC_ALL=zh_CN.UTF-8
grpcurl -plaintext -max-time "$MAX_TIME" \
  -import-path /home/giss/opt_algorithms/proto \
  -proto tna_service.proto \
  -d "$PAYLOAD" \
  localhost:50051 func.FuncService/tool_od_estimation_local_stream 2>&1 \
  | python3 -u "$FMT" | tee /tmp/grpc_stream_latest.log
