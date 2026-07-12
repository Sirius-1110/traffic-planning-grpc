"""现状交通 AI 诊断报告：读 PG → 图表数据 + 叙事章节 → 图文并茂 HTML。

工作区：tna_unified_grpc_service（见 docs/server-workspace.md）
部署：MCP（Cursor）/ CLI / 后续可挂 NestJS 或 gRPC bridge，不依赖 grpc_server 编译。
"""

from .service import build_status_diagnosis_report, fetch_report_payload

__all__ = ["build_status_diagnosis_report", "fetch_report_payload"]
