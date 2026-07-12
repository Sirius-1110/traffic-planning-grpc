"""读取 server-workspace 登记的全部项目表：存在性、行数、关键汇总（可选样例行）。"""
from __future__ import annotations

from typing import Any

from . import db
from .table_registry import TABLE_SPECS, suffixes_for_scope


def _safe(v, dec: int = 2):
    try:
        return round(float(v), dec) if v is not None else None
    except (TypeError, ValueError):
        return None


def list_tables_in_db(conn, prefix: str, schema: str = "user_project") -> list[dict[str, Any]]:
    """从 information_schema 列出该前缀下所有物理表（含未在登记表中的表）。"""
    pattern = f"{prefix}%"
    _, rows = db.query(
        conn,
        """
        SELECT table_name
        FROM information_schema.tables
        WHERE table_schema = %s AND table_name LIKE %s
        ORDER BY table_name
        """,
        (schema, pattern),
    )
    out = []
    for (name,) in rows:
        suffix = name[len(prefix) :] if name.startswith(prefix) else name
        out.append({"table_name": name, "suffix": suffix})
    return out


def _count_rows(conn, tbl: str) -> int:
    try:
        _, rows = db.query(conn, f"SELECT COUNT(*)::bigint FROM {tbl}")
        return int(rows[0][0]) if rows else 0
    except Exception:
        return -1


def _summarize_suffix(conn, prefix: str, suffix: str) -> dict[str, Any]:
    """按表类型返回关键汇总（无则仅行数）。"""
    tbl = db.table_name(prefix, suffix)
    summary: dict[str, Any] = {}

    if suffix == "other_od":
        try:
            _, rows = db.query(conn, f"SELECT COUNT(*)::bigint FROM {tbl}")
            summary = {"od_pairs": int(rows[0][0]) if rows else 0}
        except Exception:
            pass
    elif suffix == "od_estimation_results":
        try:
            _, rows = db.query(
                conn,
                f"""
                SELECT ROUND(SUM(initial_demand)::numeric,0),
                       ROUND(SUM(estimated_demand)::numeric,0)
                FROM {tbl}
                """,
            )
            if rows and rows[0][0] is not None:
                d0, d1 = float(rows[0][0] or 0), float(rows[0][1] or 0)
                summary = {
                    "baseline_demand": d0,
                    "max_demand": d1,
                    "utilization": round(d0 / d1, 4) if d1 > 0 else None,
                }
        except Exception:
            pass
    elif suffix == "other_taz_socioeconomic":
        try:
            _, rows = db.query(
                conn,
                f"""
                SELECT COUNT(*)::bigint,
                       COALESCE(SUM(pop),0)::bigint,
                       ROUND(SUM(COALESCE(area,0))::numeric,2)
                FROM {tbl}
                """,
            )
            if rows:
                summary = {
                    "taz_count": int(rows[0][0] or 0),
                    "total_pop": int(rows[0][1] or 0),
                    "total_area_km2": _safe(rows[0][2], 2),
                }
        except Exception:
            pass
    elif suffix == "greedy_link_flow_results":
        try:
            _, rows = db.query(
                conn,
                f"""
                SELECT COUNT(*)::bigint,
                       ROUND(AVG(flow)::numeric,1),
                       ROUND(MAX(flow)::numeric,1)
                FROM {tbl} WHERE flow IS NOT NULL
                """,
            )
            if rows:
                summary = {
                    "links": int(rows[0][0] or 0),
                    "avg_flow": _safe(rows[0][1], 1),
                    "max_flow": _safe(rows[0][2], 1),
                }
        except Exception:
            pass
    elif suffix in ("pt_summary_result", "pt_iter_result", "greedy_summary_statistics", "slow_greedy_summary_statistics"):
        try:
            with conn.cursor() as cur:
                cur.execute(f"SELECT * FROM {tbl} LIMIT 1")
                cols = [d[0] for d in cur.description] if cur.description else []
                row = cur.fetchone()
            if row:
                summary = {cols[i]: row[i] for i in range(min(len(cols), 15))}
        except Exception:
            pass
    elif suffix in ("road_community", "road_point"):
        try:
            _, rows = db.query(conn, f"SELECT COUNT(*)::bigint FROM {tbl}")
            summary = {"count": int(rows[0][0]) if rows else 0}
        except Exception:
            pass

    return summary


def _sample_rows(conn, tbl: str, limit: int, max_cols: int = 12) -> list[dict[str, Any]]:
    if limit <= 0:
        return []
    try:
        with conn.cursor() as cur:
            cur.execute(f"SELECT * FROM {tbl} LIMIT {int(limit)}")
            cols = [d[0] for d in cur.description][:max_cols] if cur.description else []
            samples = []
            for row in cur.fetchall():
                samples.append({cols[i]: row[i] for i in range(len(cols))})
            return samples
    except Exception:
        return []


def collect_workspace_tables(
    conn,
    prefix: str,
    logs: list[str],
    scope: str = "all",
    *,
    include_samples: bool = False,
    sample_rows: int = 3,
) -> dict[str, Any]:
    """
    读取 server-workspace 登记表 + 库中同前缀的其它表。
    返回 { registered: {...}, discovered: [...], missing: [...] }
    """
    specs = suffixes_for_scope(scope)
    registered: dict[str, Any] = {}
    missing: list[str] = []

    for spec in specs:
        suffix = spec["suffix"]
        exists = db.table_exists(conn, prefix, suffix)
        entry: dict[str, Any] = {
            "suffix": suffix,
            "label": spec["label"],
            "category": spec["category"],
            "exists": exists,
            "full_name": f"{prefix}{suffix}",
        }
        if exists:
            tbl = db.table_name(prefix, suffix)
            entry["row_count"] = _count_rows(conn, tbl)
            entry["summary"] = _summarize_suffix(conn, prefix, suffix)
            if include_samples and sample_rows > 0 and entry["row_count"] > 0:
                entry["sample_rows"] = _sample_rows(conn, tbl, sample_rows)
            logs.append(f"table[{suffix}]: rows={entry['row_count']}")
        else:
            missing.append(suffix)
            entry["row_count"] = 0
        registered[suffix] = entry

    # 库中存在但未登记的表
    db_tables = list_tables_in_db(conn, prefix)
    extra = []
    for t in db_tables:
        if t["suffix"] in registered:
            continue
        suffix = t["suffix"]
        tbl = db.table_name(prefix, suffix)
        extra.append(
            {
                "suffix": suffix,
                "table_name": t["table_name"],
                "row_count": _count_rows(conn, tbl),
                "label": "（库中发现，未在登记表）",
            }
        )

    logs.append(
        f"tables: registered={len(registered)} exists={sum(1 for v in registered.values() if v['exists'])} "
        f"extra_in_db={len(extra)} missing={len(missing)}"
    )

    return {
        "ok": True,
        "prefix": prefix,
        "scope": scope,
        "registered": registered,
        "missing_suffixes": missing,
        "extra_tables": extra,
        "all_in_db": db_tables,
    }
