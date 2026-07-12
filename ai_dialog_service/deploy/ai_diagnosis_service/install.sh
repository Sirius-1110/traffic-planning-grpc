#!/bin/bash
# 在服务器上首次安装依赖（在 deploy 脚本上传代码后执行一次）
set -euo pipefail

APP_ROOT="${1:-/home/giss/opt_algorithms/tna_ai_diagnosis}"
cd "$APP_ROOT"

python3 -m venv venv
./venv/bin/pip install -U pip
./venv/bin/pip install -r ai_diagnosis/requirements-server.txt

mkdir -p secrets reports logs
chmod +x deploy/ai_diagnosis_service/start.sh 2>/dev/null || chmod +x start.sh 2>/dev/null || true

echo "安装完成。请配置 $APP_ROOT/secrets/ai.local.json 后执行 start.sh"
