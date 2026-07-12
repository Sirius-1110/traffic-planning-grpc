"""读取 db.conf 并构建 psycopg2 连接参数。

查找顺序：
  1. 环境变量 TNA_DB_CONF 指向的路径
  2. /opt/algorithms/db.conf（服务器共享位置）
  3. 脚本所在目录的上上级（算法根目录）下的 db.conf
"""
import os

import psycopg2

_DEFAULT_PATHS = [
    os.environ.get("TNA_DB_CONF", ""),
    "/opt/algorithms/db.conf",
    os.path.join(os.path.dirname(__file__), "..", "..", "db.conf"),
]


def _parse_conf(path: str) -> dict:
    kv = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" in line:
                k, v = line.split("=", 1)
                kv[k.strip()] = v.strip()
    return kv


def load_db_conf(param1: str = "") -> dict:
    """返回 psycopg2.connect 可用的关键字参数字典。

    param1 非空时优先解析为 libpq 连接串或 key=value 行；
    否则按默认路径搜索 db.conf。
    """
    if param1 and param1.strip():
        # 简单 key=value 多行或单行格式
        kv = {}
        for part in param1.replace(";", "\n").splitlines():
            part = part.strip()
            if "=" in part and not part.startswith("#"):
                k, v = part.split("=", 1)
                kv[k.strip()] = v.strip()
        if kv:
            return _conf_to_psycopg2(kv)
        # 回退：当作 DSN 串直接传给 psycopg2
        return {"dsn": param1}

    for path in _DEFAULT_PATHS:
        if path and os.path.isfile(path):
            return _conf_to_psycopg2(_parse_conf(path))

    raise FileNotFoundError(
        "未找到 db.conf，请设置环境变量 TNA_DB_CONF 或在 /opt/algorithms/db.conf 提供配置。"
    )


def _conf_to_psycopg2(kv: dict) -> dict:
    mapping = {
        "host": "host", "port": "port", "dbname": "dbname",
        "user": "user", "password": "password", "role": "role",
    }
    result = {}
    for src, dst in mapping.items():
        if src in kv:
            result[dst] = kv[src]
    if "port" in result:
        result["port"] = int(result["port"])
    return result



def resolve_db_schema(db_kw: dict) -> str:
    role = str(db_kw.get("role", "")).strip()
    if role:
        return role
    schema = os.environ.get("TNA_DB_SCHEMA", "").strip()
    return schema or "user_project"



def build_table_prefix(project_id: int, user_id: int, case_id: int) -> str:
    if case_id > 0:
        prefix = f"project{project_id}_user{user_id}_case{case_id}_"
    else:
        prefix = f"project{project_id}_user{user_id}_"
    return prefix



def build_logical_table_name(schema: str, prefix: str, raw_name: str) -> str:
    return f"{schema}.{prefix}{raw_name}"



def _connect_db(db_kw: dict):
    dsn = str(db_kw.get("dsn", "")).strip()
    if dsn:
        return psycopg2.connect(dsn)
    return psycopg2.connect(
        host=db_kw.get("host", "localhost"),
        port=db_kw.get("port", 5432),
        dbname=db_kw.get("dbname", "postgres"),
        user=db_kw.get("user", "postgres"),
        password=db_kw.get("password", ""),
    )



def ensure_required_tables_exist(db_kw: dict, table_names: list[str]) -> None:
    missing: list[str] = []
    conn = _connect_db(db_kw)
    try:
        with conn.cursor() as cur:
            for qualified_name in table_names:
                schema, dot, table = qualified_name.partition(".")
                if not dot:
                    schema = resolve_db_schema(db_kw)
                    table = qualified_name
                cur.execute(
                    "SELECT 1 FROM information_schema.tables WHERE table_schema=%s AND table_name=%s LIMIT 1",
                    (schema, table),
                )
                if cur.fetchone() is None:
                    missing.append(qualified_name)
    finally:
        conn.close()

    if missing:
        raise FileNotFoundError(
            "Required input tables do not exist for project-style naming: " + ", ".join(missing)
        )



def build_runtime_db_kw(db_kw: dict, project_id: int, user_id: int, case_id: int) -> tuple[dict, str, str]:
    schema = resolve_db_schema(db_kw)
    prefix = build_table_prefix(project_id, user_id, case_id)
    runtime_db_kw = {**db_kw, "role": schema, "scenario_prefix": prefix}
    return runtime_db_kw, schema, prefix
