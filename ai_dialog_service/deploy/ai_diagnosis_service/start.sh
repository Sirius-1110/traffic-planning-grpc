#!/bin/bash
# TNA AI 诊断 HTTP 服务启动脚本（服务器）
set -euo pipefail

APP_ROOT="${TNA_AI_APP_ROOT:-/home/giss/opt_algorithms/tna_ai_diagnosis}"
cd "$APP_ROOT"

export PYTHONPATH="$APP_ROOT:${PYTHONPATH:-}"
export TNA_DB_CONF="${TNA_DB_CONF:-/home/giss/opt_algorithms/db.conf}"
export TNA_AI_CONFIG_JSON="${TNA_AI_CONFIG_JSON:-$APP_ROOT/secrets/ai.local.json}"
export TNA_AI_REPORT_DIR="${TNA_AI_REPORT_DIR:-$APP_ROOT/reports}"
export TNA_AI_SERVICE_HOST="${TNA_AI_SERVICE_HOST:-127.0.0.1}"
export TNA_AI_SERVICE_PORT="${TNA_AI_SERVICE_PORT:-18080}"

VENV="$APP_ROOT/venv"
if [ -x "$VENV/bin/python" ]; then
  PY="$VENV/bin/python"
else
  PY="${TNA_AI_PYTHON:-python3}"
fi

mkdir -p "$TNA_AI_REPORT_DIR" "$APP_ROOT/logs"

echo "[tna-ai-diagnosis] host=$TNA_AI_SERVICE_HOST port=$TNA_AI_SERVICE_PORT"
exec "$PY" -m ai_diagnosis >> "$APP_ROOT/logs/service.log" 2>&1
