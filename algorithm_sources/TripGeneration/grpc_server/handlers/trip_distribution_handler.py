"""出行分布业务处理器。"""
import json
import sys
import os

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from trip_distribution.trip_distribution import fs_run_trip_distribution
from .db_conf import load_db_conf, build_runtime_db_kw, build_logical_table_name, ensure_required_tables_exist


def run(project_id: int, user_id: int, case_id: int, param1: str, param2: str, on_progress):
    """执行出行分布，返回 (code, message, data_json)。"""
    try:
        db_kw = load_db_conf(param1)
        runtime_db_kw, schema, prefix = build_runtime_db_kw(db_kw, project_id, user_id, case_id)

        extra = {}
        if param2 and param2.strip():
            try:
                extra = json.loads(param2)
            except Exception:
                pass

        taz_table_name = build_logical_table_name(schema, prefix, "other_taz_socioeconomic")
        pa_table_name  = build_logical_table_name(schema, prefix, "other_trip_production_attraction")
        hist_od_table  = None
        ensure_required_tables_exist(runtime_db_kw, [taz_table_name, pa_table_name])

        cfg = {
            "db_conn_str": runtime_db_kw,
            "algorithm_params": extra.get("algorithm_params", {}),
        }

        result_json = fs_run_trip_distribution(
            on_progress=on_progress,
            cfg=cfg,
            taz_table_name=taz_table_name,
            trip_generation_table_name=pa_table_name,
            hist_od_table_name=hist_od_table,
        )
        ret = json.loads(result_json)
        code = 1 if ret.get("status") == 1 else -99
        return code, ret.get("message", ""), result_json

    except FileNotFoundError as e:
        return -2, str(e), "{}"
    except Exception as e:
        return -99, f"trip_distribution_handler error: {e.__class__.__name__}: {e}", "{}"
