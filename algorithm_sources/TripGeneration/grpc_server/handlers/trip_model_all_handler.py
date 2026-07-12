"""出行生成 + 出行分布串联处理器。"""
import json
import sys
import os

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from trip_generation.trip_generation import fs_run_trip_generation
from trip_distribution.trip_distribution import fs_run_trip_distribution
from .db_conf import load_db_conf, build_runtime_db_kw, build_logical_table_name, ensure_required_tables_exist


def run(project_id: int, user_id: int, case_id: int, param1: str, param2: str, on_progress):
    """串联执行出行生成与出行分布，返回 (code, message, data_json)。"""
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
        hist_od_table  = None
        ensure_required_tables_exist(runtime_db_kw, [taz_table_name])

        cfg = {
            "db_conn_str": runtime_db_kw,
            "algorithm_params": extra.get("algorithm_params", {}),
        }

        # ── Step 1: Trip Generation (0~50%)
        def pg(it, pct):
            on_progress(it, pct * 0.5)

        r1_json = fs_run_trip_generation(
            on_progress=pg, cfg=cfg, taz_table_name=taz_table_name,
        )
        r1 = json.loads(r1_json)
        if r1.get("status") != 1:
            return -99, r1.get("message", "generation failed"), r1_json

        pa_table = r1.get("data", {}).get("tables", {}).get("trip_generation_table", "")

        # ── Step 2: Trip Distribution (50~100%)
        def pd_(it, pct):
            on_progress(it + 10, 50.0 + pct * 0.5)

        r2_json = fs_run_trip_distribution(
            on_progress=pd_,
            cfg=cfg,
            taz_table_name=taz_table_name,
            trip_generation_table_name=pa_table,
            hist_od_table_name=hist_od_table,
        )
        r2 = json.loads(r2_json)
        code = 1 if r2.get("status") == 1 else -99

        combined = {
            "status": code,
            "message": r2.get("message", ""),
            "generation": r1,
            "distribution": r2,
        }
        return code, r2.get("message", ""), json.dumps(combined, ensure_ascii=False)

    except FileNotFoundError as e:
        return -2, str(e), "{}"
    except Exception as e:
        return -99, f"trip_model_all_handler error: {e.__class__.__name__}: {e}", "{}"
