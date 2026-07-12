"""出行生成业务处理器。"""
import json
import sys
import os

# 保证 repo 根目录在 sys.path
_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from trip_generation.trip_generation import fs_run_trip_generation
from .db_conf import load_db_conf, build_runtime_db_kw, build_logical_table_name, ensure_required_tables_exist


def run(project_id: int, user_id: int, case_id: int, param1: str, param2: str, on_progress):
    """执行出行生成，返回 (code, message, data_json)。"""
    try:
        db_kw = load_db_conf(param1)
        runtime_db_kw, schema, prefix = build_runtime_db_kw(db_kw, project_id, user_id, case_id)

        # param2 仅读取 algorithm_params；表名/连接串字段（如 taz_table_name、db_conn_str）一律静默忽略，
        # 表名始终由 project_id/user_id/case_id 派生，连接串始终由 param1 或 db.conf 决定。
        extra = {}
        if param2 and param2.strip():
            try:
                extra = json.loads(param2)
            except Exception:
                pass

        taz_table_name = build_logical_table_name(schema, prefix, "other_taz_socioeconomic")
        ensure_required_tables_exist(runtime_db_kw, [taz_table_name])

        cfg = {
            "db_conn_str": runtime_db_kw,
            "algorithm_params": extra.get("algorithm_params", {}),
        }

        result_json = fs_run_trip_generation(
            on_progress=on_progress,
            cfg=cfg,
            taz_table_name=taz_table_name,
        )
        ret = json.loads(result_json)
        code = 1 if ret.get("status") == 1 else -99
        return code, ret.get("message", ""), result_json

    except FileNotFoundError as e:
        return -2, str(e), "{}"
    except Exception as e:
        return -99, f"trip_generation_handler error: {e.__class__.__name__}: {e}", "{}"
