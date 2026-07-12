"""数据库连接与表名前缀（与 server-workspace.md 一致）。"""
from __future__ import annotations

import json
import os
from typing import Any

import psycopg2
from psycopg2.extensions import connection as PgConnection

# Non-secret defaults. Credentials must come from configuration or environment.
_DEFAULT_PG = {
    "host": "8.137.55.118",
    "port": 5432,
    "dbname": "urban",
    "user": "urban",
    "password": "",
    "schema": "user_project",
}


def load_pg_config() -> dict[str, Any]:
    """优先级：TNA_REPORT_PG_JSON > TNA_DB_CONF 文件 > 内置默认。"""
    defaults = _DEFAULT_PG.copy()
    defaults["password"] = os.environ.get("TNA_DB_PASSWORD", "")
    env_json = os.environ.get("TNA_REPORT_PG_JSON", "").strip()
    if env_json:
        try:
            cfg = defaults.copy()
            cfg.update(json.loads(env_json))
            return cfg
        except json.JSONDecodeError:
            pass

    for path in (
        os.environ.get("TNA_DB_CONF", "").strip(),
        "/home/giss/opt_algorithms/db.conf",
        "/opt/algorithms/db.conf",
    ):
        if not path:
            continue
        try:
            cfg: dict[str, Any] = {}
            if path.lower().endswith(".json"):
                with open(path, encoding="utf-8") as f:
                    cfg = json.load(f)
            else:
                with open(path, encoding="utf-8") as f:
                    for line in f:
                        line = line.strip()
                        if "=" in line and not line.startswith("#"):
                            k, v = line.split("=", 1)
                            cfg[k.strip()] = v.strip()
            if cfg:
                merged = defaults.copy()
                merged.update(
                    {
                        "host": cfg.get("host", merged["host"]),
                        "port": int(cfg.get("port", merged["port"])),
                        "dbname": cfg.get("dbname", merged["dbname"]),
                        "user": cfg.get("user", merged["user"]),
                        "password": cfg.get("password", merged["password"]),
                    }
                )
                return merged
        except OSError:
            continue

    return defaults


def connect(cfg: dict[str, Any] | None = None) -> PgConnection:
    c = cfg or load_pg_config()
    dsn = c.get("connection_string", "")
    if dsn:
        conn = psycopg2.connect(dsn)
    else:
        conn = psycopg2.connect(
            host=c["host"],
            port=int(c["port"]),
            dbname=c["dbname"],
            user=c["user"],
            password=c["password"],
        )
    # 只读采集：避免某张表 SQL 失败后污染同一连接上的后续 table_exists
    conn.autocommit = True
    return conn


def build_prefix(project_id: int, user_id: int, case_id: int) -> str:
    """case_id 为方案 ID；0 也写入前缀 → project{p}_user{u}_case0_"""
    return f"project{project_id}_user{user_id}_case{case_id}_"


def table_name(prefix: str, suffix: str, schema: str = "user_project") -> str:
    return f'{schema}."{prefix}{suffix}"'


def query(conn: PgConnection, sql: str, params=None) -> tuple[list[str], list[tuple]]:
    try:
        with conn.cursor() as cur:
            cur.execute(sql, params)
            cols = [d[0] for d in cur.description] if cur.description else []
            rows = cur.fetchall()
        return cols, rows
    except Exception:
        if not conn.autocommit:
            conn.rollback()
        raise


def table_exists(conn: PgConnection, prefix: str, suffix: str, schema: str = "user_project") -> bool:
    tname = f"{prefix}{suffix}"
    with conn.cursor() as cur:
        cur.execute(
            "SELECT 1 FROM information_schema.tables "
            "WHERE table_schema=%s AND table_name=%s LIMIT 1",
            (schema, tname),
        )
        return cur.fetchone() is not None
