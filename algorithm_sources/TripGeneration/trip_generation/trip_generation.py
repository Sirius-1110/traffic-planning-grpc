import json
import argparse
import os

import pandas as pd
import psycopg2

from pandas import DataFrame


__all__ = ["fs_run_trip_generation"]
# Legacy pop=2.0 → p112 OSM2 synthetic TAZ trip-only avg V/C≈10.3; scale for V/C≈1
_VC_CALIB = 1.0 / 10.303

DEFAULT_PROD_RATES = {
    "pop": 2.0 * _VC_CALIB,
    "cars": 0.7 * _VC_CALIB,
    "eind": 0.1 * _VC_CALIB,
    "eret": 0.2 * _VC_CALIB,
    "eoth": 0.15 * _VC_CALIB,
    "du": 0.1 * _VC_CALIB,
    "hinc": 0.05 / 10000 * _VC_CALIB,
    "area": 0.0,
    "etot": 0.0,
}

DEFAULT_ATTR_RATES = {
    "pop": 0.5 * _VC_CALIB,
    "cars": 0.1 * _VC_CALIB,
    "eind": 1.5 * _VC_CALIB,
    "eret": 3.0 * _VC_CALIB,
    "eoth": 1.5 * _VC_CALIB,
    "du": 0.1 * _VC_CALIB,
    "hinc": 0.05 / 10000 * _VC_CALIB,
    "area": 0.1 * _VC_CALIB,
    "etot": 0.0,
}


def _connect(db: dict) -> "psycopg2.extensions.connection":
    if db.get("connection_string"):
        conn = psycopg2.connect(db.get("connection_string"))
    else:
        conn = psycopg2.connect(
            host=db.get("host", "localhost"),
            port=db.get("port", 5432),
            dbname=db.get("dbname"),
            user=db.get("user"),
            password=db.get("password"),
        )
    conn.autocommit = True
    return conn


def _qt(name: str) -> str:
    """在 SQL 执行时对 schema.table 进行 PostgreSQL 标识符引用。"""
    if not name:
        return name
    if '"' in name:
        return name
    schema, dot, table = name.partition('.')
    if dot:
        return f'"{schema}"."{table}"'
    return f'"{name}"'


def _resolve_tables(db: dict, taz_table_name: str) -> tuple[str, str]:
    role = db.get("role", "user_project")
    prefix = db.get("scenario_prefix", "")
    if taz_table_name:
        taz = taz_table_name
    else:
        taz = f"{role}.{prefix}other_taz_socioeconomic"
    out_pa = f"{role}.{prefix}other_trip_production_attraction"
    return taz, out_pa


def _get_taz_columns(conn, taz_table: str) -> list[str]:
    q_taz_table = _qt(taz_table)
    with conn.cursor() as cur:
        cur.execute(f"SELECT * FROM {q_taz_table} LIMIT 0")
        cols = [desc[0] for desc in cur.description]
    return cols


def _read_taz(conn, taz_table: str, columns: list[str]) -> DataFrame:
    cols_sql = ", ".join(columns)
    q_taz_table = _qt(taz_table)
    with conn.cursor() as cur:
        sql = f"SELECT {cols_sql} FROM {q_taz_table}"
        cur.execute(sql)
        rows = cur.fetchall()
        cols = [desc[0] for desc in cur.description]
    df = pd.DataFrame(rows, columns=cols)
    for col in df.columns:
        if col != "id":
            df[col] = pd.to_numeric(df[col])
    return df


def _trip_generation(
    taz_df: DataFrame,
    production_fields: list[str],
    attraction_fields: list[str],
    prod_rates: dict,
    attr_rates: dict,
    on_progress
) -> None:
    on_progress(5, 50)
    # 1) Trip production
    prod = 0.0
    for col in production_fields:
        prod += float(prod_rates[col]) * taz_df[col]
    taz_df["production"] = prod
    on_progress(6, 60)
    # 2) Raw trip attraction
    attr = 0.0
    for col in attraction_fields:
        attr += float(attr_rates[col]) * taz_df[col]
    taz_df["attraction_raw"] = attr
    on_progress(7, 70)
    # 3) balance attraction based on production
    total_P = taz_df["production"].sum()
    total_Araw = taz_df["attraction_raw"].sum()
    balance_factor = total_P / total_Araw if total_Araw > 0 else 0.0
    taz_df["attraction"] = taz_df["attraction_raw"] * balance_factor
    on_progress(8, 80)


def _write_trip_generation(conn, out_pa: str, taz_df: pd.DataFrame):
    q_out_pa = _qt(out_pa)
    with conn.cursor() as cur:
        cur.execute(f"""
        CREATE TABLE IF NOT EXISTS {q_out_pa} (
          zone_id INTEGER NOT NULL,
          production NUMERIC,
          attraction NUMERIC,
          created_at TIMESTAMPTZ NOT NULL DEFAULT now()
        )
        """)
        cols = ["id", "production", "attraction"]
        rows = list(taz_df[cols].itertuples(index=False, name=None))
        cur.execute(f"DELETE FROM {q_out_pa}")
        cur.executemany(
            f"INSERT INTO {q_out_pa}(zone_id,production,attraction) VALUES (%s,%s,%s)",
            rows,
        )
    return len(rows)


def fs_run_trip_generation(on_progress, cfg: dict, taz_table_name: str):
    logs: list[str] = []
    db, alg = cfg.get("db_conn_str", {}), cfg.get("algorithm_params", {})
    trip_generation_table_name = ""
    try:
        # 1) connect database
        on_progress(1, 10)
        conn = _connect(db)
        logs.append(
            f"db.connect ok: host={db.get('host','localhost')} port={db.get('port',5432)} dbname={db.get('dbname')} user={db.get('user')}"
        )
        on_progress(2, 20)

        # 2) resolve table
        taz_table_name, trip_generation_table_name = _resolve_tables(db, taz_table_name)
        logs.append(f"tables: taz={taz_table_name}, out_pa={trip_generation_table_name}")
        on_progress(3, 30)

        # 3) select valid fields by inspecting TAZ columns
        all_taz_cols = set(_get_taz_columns(conn, taz_table_name))
        if "id" not in all_taz_cols:
            logs.append(f"No id columns in table {taz_table_name}")
            raise ValueError("taz table missing id column")

        production_fields = [c for c in DEFAULT_PROD_RATES.keys() if c in all_taz_cols]
        attraction_fields = [c for c in DEFAULT_ATTR_RATES.keys() if c in all_taz_cols]
        if not production_fields:
            logs.append(f"No valid production fields in {taz_table_name} table")
            logs.append(f"Support fields: {list(DEFAULT_PROD_RATES.keys())}")
            raise ValueError("no valid production fields")
        if not attraction_fields:
            logs.append(f"No valid attraction fields in {taz_table_name} table")
            logs.append(f"Support fields: {list(DEFAULT_ATTR_RATES.keys())}")
            raise ValueError("no valid attraction fields")
        logs.append(f"Used production fields: {production_fields}")
        logs.append(f"Used attraction fields: {attraction_fields}")

        # 4) build rates: defaults + optional overrides from algorithm_params
        prod_rates = dict(DEFAULT_PROD_RATES)
        attr_rates = dict(DEFAULT_ATTR_RATES)
        if isinstance(alg.get("production_rates"), dict):
            for k, v in alg.get("production_rates").items():
                if k in prod_rates:
                    prod_rates[k] = float(v)
        if isinstance(alg.get("attraction_rates"), dict):
            for k, v in alg.get("attraction_rates").items():
                if k in attr_rates:
                    attr_rates[k] = float(v)
        for fname in list(prod_rates.keys()):
            key = f"p_{fname}_rate"
            if key in alg:
                prod_rates[fname] = float(alg[key])
        for fname in list(attr_rates.keys()):
            key = f"a_{fname}_rate"
            if key in alg:
                attr_rates[fname] = float(alg[key])

        # 5) read taz with used columns
        used_cols = sorted(set(production_fields) | set(attraction_fields))
        taz_columns = ["id"] + used_cols
        taz_df = _read_taz(conn, taz_table_name, taz_columns)
        logs.append(f"read.taz rows={len(taz_df)}")
        on_progress(4, 40)

        # 6) generation
        _trip_generation(taz_df, production_fields, attraction_fields, prod_rates, attr_rates, on_progress)
        total_P = float(taz_df["production"].sum()) if len(taz_df) else 0.0
        total_A = float(taz_df["attraction"].sum()) if len(taz_df) else 0.0
        logs.append(f"compute.generation sumP={total_P} sumA={total_A}")

        # 7) write back
        inserted = _write_trip_generation(conn, trip_generation_table_name, taz_df)
        logs.append(f"write.pa inserted={inserted}")
        on_progress(9, 88)

        # success json（统一顶层四键）
        ret = {
            "status": 1,
            "message": "Success",
            "data": {
                "tables": {
                    "trip_generation_table": trip_generation_table_name,
                },
                "counts": {"pa_rows": inserted},
            },
            "summary": {
                "stage": "trip_generation",
                "logs": logs,
            },
        }
        on_progress(10, 100.0)
        return json.dumps(ret, ensure_ascii=False)

    except Exception as e:
        # error json with logs and detail
        err = f"trip_generation error: {e.__class__.__name__}: {e}"
        logs.append(err)
        ret = {
            "status": 0,
            "message": err,
            "data": {
                "tables": {
                    "trip_generation_table": trip_generation_table_name,
                }
            },
            "summary": {
                "stage": "trip_generation",
                "logs": logs,
            },
        }
        try:
            on_progress(10, 100.0)
        except Exception:
            pass
        return json.dumps(ret, ensure_ascii=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Trip generation runner')
    parser.add_argument('--cfg-json', type=str, required=True, help='Path to cfg.json')
    parser.add_argument('--taz-table-name', type=str, default='other_taz_socioeconomic', help='TAZ table name (with or without schema)')
    parser.add_argument('--out-csv', type=str, default=None, help='Path to save generated PA table as CSV (default: dist/trip_generation.csv)')
    args = parser.parse_args()

    with open(args.cfg_json, 'r', encoding='utf-8') as f:
        cfg = json.load(f)

    def progress_cb(iteration: int, percent: float):
        print(iteration, percent)

    data = fs_run_trip_generation(
        on_progress=progress_cb,
        cfg=cfg,
        taz_table_name=args.taz_table_name
    )
    print(data)

    # 保存结果为 CSV（默认 dist/trip_generation.csv）
    parent_dir = os.path.dirname(os.path.dirname(__file__))
    dist_dir = os.path.join(parent_dir, 'dist')
    os.makedirs(dist_dir, exist_ok=True)
    if not args.out_csv:
        args.out_csv = os.path.join(dist_dir, 'trip_generation.csv')

    if args.out_csv:
        try:
            ret = json.loads(data)
            pa_table = ret.get('tables', {}).get('trip_generation_table')
            if pa_table:
                conn = _connect(cfg.get('db_conn_str', {}))
                try:
                    with conn.cursor() as cur:
                        cur.execute(f"SELECT zone_id,production,attraction FROM {pa_table} ORDER BY zone_id")
                        rows = cur.fetchall()
                        cols = [d[0] for d in cur.description]
                    df = pd.DataFrame(rows, columns=cols)
                    df.to_csv(args.out_csv, index=False, encoding='utf-8-sig')
                finally:
                    conn.close()
        except Exception:
            # 失败不影响主流程
            pass