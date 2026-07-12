import json
import argparse
import os

import numpy as np
import pandas as pd
import psycopg2

from pandas import DataFrame


__all__ = ["fs_run_trip_distribution"]



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


def _resolve_tables(db: dict, taz_table_name: str, trip_generation_table_name: str, hist_od_table_name: str = None) -> tuple[str, str, str, str]:
    role = db.get("role", "user_project")
    prefix = db.get("scenario_prefix", "")
    taz = taz_table_name if taz_table_name else f"{role}.{prefix}other_taz_socioeconomic"
    pa = trip_generation_table_name if trip_generation_table_name else f"{role}.{prefix}other_trip_production_attraction"
    if hist_od_table_name is not None and str(hist_od_table_name).strip() != "":
        hist_od = hist_od_table_name
    else:
        hist_od = ""
    out_distribution_table = f"{role}.{prefix}other_od"
    return taz, pa, hist_od, out_distribution_table


def _read_taz(conn, taz_table: str) -> DataFrame:
    q_taz_table = _qt(taz_table)
    with conn.cursor() as cur:
        sql = f"SELECT id,longitude,latitude FROM {q_taz_table}"
        cur.execute(sql)
        rows = cur.fetchall()
        cols = [desc[0] for desc in cur.description]
    df = pd.DataFrame(rows, columns=cols)
    for col in df.columns:
        df[col] = pd.to_numeric(df[col])
    df = df.sort_values("id").reset_index(drop=True)
    return df


def _read_pa(conn, pa_table: str) -> DataFrame:
    q_pa_table = _qt(pa_table)
    with conn.cursor() as cur:
        sql = f"SELECT zone_id,production,attraction FROM {q_pa_table}"
        cur.execute(sql)
        rows = cur.fetchall()
        cols = [desc[0] for desc in cur.description]
    df = pd.DataFrame(rows, columns=cols)
    for col in df.columns:
        df[col] = pd.to_numeric(df[col])
    df = df.sort_values("zone_id").reset_index(drop=True)
    return df


def _gravity_doubly_constrained(production: np.ndarray, attraction: np.ndarray, C: np.ndarray,
    param_lambda: float, max_iter: int, tol: float) -> np.ndarray:
    n = len(production)
    F = np.exp(-param_lambda * C)
    F[F < 1e-12] = 1e-12
    alpha = np.ones(n, dtype=float)
    beta = np.ones(n, dtype=float)
    for _ in range(max_iter):
        denom_alpha = (beta * attraction)[None, :] * F
        denom_alpha = denom_alpha.sum(axis=1)
        alpha_new = np.where(denom_alpha > 0, 1.0 / denom_alpha, 0.0)
        denom_beta = (alpha_new * production)[:, None] * F
        denom_beta = denom_beta.sum(axis=0)
        beta_new = np.where(denom_beta > 0, 1.0 / denom_beta, 0.0)
        diff_alpha = np.max(np.abs(alpha_new - alpha))
        diff_beta = np.max(np.abs(beta_new - beta))
        alpha, beta = alpha_new, beta_new
        if max(diff_alpha, diff_beta) < tol:
            break
    T = (alpha[:, None] * production[:, None]) * (beta[None, :] * attraction[None, :]) * F
    return T


# doubly-constrained gravity model
def _trip_distribution(pa_df: DataFrame, taz_df: DataFrame, alg: dict, on_progress, hist_od_df: DataFrame | None = None) -> list[list[float]]:
    param_lambda = alg.get("param_lambda", 0.1)
    diag_eps = alg.get("diag_eps", 0.1)
    max_iter = alg.get("max_iter", 20)
    tol = alg.get("tol", 1e-6)
    production = pa_df["production"].to_numpy().astype(float)
    attraction = pa_df["attraction"].to_numpy().astype(float)
    C = _obtain_taz_cost_from_coor(taz_df, diag_eps=diag_eps)
    if hist_od_df is not None and not hist_od_df.empty:
        lambda_calib = _calibrate_lambda_with_hist_od(taz_df, hist_od_df, C, alg)
        if lambda_calib is not None:
            param_lambda = lambda_calib
    on_progress(6, 60)
    T = _gravity_doubly_constrained(production, attraction, C, param_lambda, max_iter, tol)
    on_progress(7, 70)
    return T


def _obtain_taz_cost_from_coor(taz_df: DataFrame, diag_eps: float) -> np.ndarray:
    R_earth_km = 6371.0
    latitude = np.radians(taz_df["latitude"].to_numpy(dtype=float))
    longitude = np.radians(taz_df["longitude"].to_numpy(dtype=float))
    latitude_diff = latitude[:, None] - latitude[None, :]
    longitude_diff = longitude[:, None] - longitude[None, :]
    a = np.sin(latitude_diff / 2.0)**2 + np.cos(latitude[:, None]) * np.cos(latitude[None, :]) * np.sin(longitude_diff / 2.0)**2
    c = 2 * np.arcsin(np.sqrt(a))
    C_km = R_earth_km * c  # km
    if diag_eps is not None and diag_eps > 0:
        np.fill_diagonal(C_km, diag_eps)
    return C_km


def _read_hist_od(conn, hist_od_table: str) -> DataFrame:
    q_hist_od_table = _qt(hist_od_table)
    with conn.cursor() as cur:
        sql = f"SELECT f_id, t_id, demand FROM {q_hist_od_table}"
        cur.execute(sql)
        rows = cur.fetchall()
        if not rows:
            return pd.DataFrame(columns=["f_id", "t_id", "demand"])
        cols = [desc[0] for desc in cur.description]
    df = pd.DataFrame(rows, columns=cols)
    for col in df.columns:
        df[col] = pd.to_numeric(df[col])
    return df


def _hist_od_to_matrix(taz_df: DataFrame, hist_od_df: DataFrame, f_col: str = "f_id", t_col: str = "t_id", val_col: str = "demand") -> np.ndarray:
    zone_ids = taz_df["id"].to_numpy()
    id_to_idx = {int(z): idx for idx, z in enumerate(zone_ids)}
    n = len(zone_ids)
    T0 = np.zeros((n, n), dtype=float)
    for _, row in hist_od_df.iterrows():
        fid = int(row[f_col]); tid = int(row[t_col]); val = float(row[val_col])
        i = id_to_idx.get(fid); j = id_to_idx.get(tid)
        if i is None or j is None:
            continue
        T0[i, j] = val
    return T0


def _calibrate_lambda_with_hist_od(taz_df: DataFrame, hist_od_df: DataFrame, C: np.ndarray, alg: dict) -> float | None:
    if hist_od_df is None or hist_od_df.empty:
        return None
    T_hist = _hist_od_to_matrix(taz_df, hist_od_df)
    total_hist = T_hist.sum()
    if total_hist <= 0:
        return None
    P0 = T_hist.sum(axis=1)
    A0 = T_hist.sum(axis=0)
    c_obs = float((T_hist * C).sum() / total_hist)
    lambda_min = alg.get("lambda_min", 0.01)
    lambda_max = alg.get("lambda_max", 1.0)
    lambda_steps = alg.get("lambda_steps", 10)
    max_iter_calib = alg.get("max_iter_calib", alg.get("max_iter", 50))
    tol_calib = alg.get("tol_calib", alg.get("tol", 1e-5))
    best_lambda = None
    best_err = float("inf")
    for lam in np.linspace(lambda_min, lambda_max, lambda_steps):
        T_model = _gravity_doubly_constrained(P0, A0, C, lam, max_iter_calib, tol_calib)
        total_model = T_model.sum()
        if total_model <= 0:
            continue
        c_model = float((T_model * C).sum() / total_model)
        err = abs(c_model - c_obs)
        if err < best_err:
            best_err = err
            best_lambda = lam
    return best_lambda


def _distribution_matrix_to_table(distribution: list[list[float]], taz_df: DataFrame, id_col: str ="id") -> DataFrame:
    zone_ids = taz_df[id_col].to_numpy()
    if isinstance(distribution, pd.DataFrame):
        mat = distribution.to_numpy()
    else:
        mat = np.asarray(distribution)
    n = len(zone_ids)
    assert mat.shape == (n, n)
    f_ids = np.repeat(zone_ids, n)
    t_ids = np.tile(zone_ids, n)
    demand = mat.reshape(-1)
    distribution_df = pd.DataFrame({
        "f_id": f_ids,
        "t_id": t_ids,
        "demand": demand,
        "m_special": "N"   # default N
    })
    return distribution_df


def _write_trip_distribution_df(conn, out_distribution_table_name: str, distribution_df: pd.DataFrame):
    q_out_distribution_table_name = _qt(out_distribution_table_name)
    with conn.cursor() as cur:
        cur.execute(f"""
        CREATE TABLE IF NOT EXISTS {q_out_distribution_table_name} (
          f_id INTEGER NOT NULL,
          t_id INTEGER NOT NULL,
          demand NUMERIC,
          m_special VARCHAR,
          created_at TIMESTAMPTZ NOT NULL DEFAULT now()
        )
        """)
        cur.execute(f"DELETE FROM {q_out_distribution_table_name}")
        cols = ["f_id", "t_id", "demand", "m_special"]
        rows = list(distribution_df[cols].itertuples(index=False, name=None))
        cur.executemany(
            f"INSERT INTO {q_out_distribution_table_name}(f_id, t_id, demand, m_special) VALUES (%s,%s,%s,%s)",
            rows,
        )
    conn.commit()
    return len(rows)


def progress(iteration: int, percent: float):
    print(iteration, percent)
    # return iteration, percent


def fs_run_trip_distribution(on_progress, cfg: dict, taz_table_name: str, trip_generation_table_name, hist_od_table_name: str = None):
    logs: list[str] = []
    db, alg = cfg.get("db_conn_str", {}), cfg.get("algorithm_params", {})
    out_distribution_table_name = ""
    try:
        # 1) connect
        on_progress(1, 10)
        conn = _connect(db)
        logs.append(
            f"db.connect ok: host={db.get('host','localhost')} port={db.get('port',5432)} dbname={db.get('dbname')} user={db.get('user')}"
        )
        on_progress(2, 20)

        # 2) resolve & read
        taz, pa, hist_od_table_name, out_distribution_table_name = _resolve_tables(db, taz_table_name, trip_generation_table_name, hist_od_table_name)
        logs.append(f"tables: taz={taz}, pa={pa}, hist_pd={hist_od_table_name}, out_od={out_distribution_table_name}")
        on_progress(3, 30)

        taz_df = _read_taz(conn, taz)
        logs.append(f"read.taz rows={len(taz_df)}")
        on_progress(4, 40)

        pa_df = _read_pa(conn, pa)
        logs.append(f"read.pa rows={len(pa_df)} sumP={float(pa_df['production'].sum()) if len(pa_df) else 0.0} sumA={float(pa_df['attraction'].sum()) if len(pa_df) else 0.0}")
        on_progress(5, 50)

        # 历史OD：仅当显式传入非空表名时才尝试读取与标定
        hist_od_df = None
        hist_rows = 0
        if hist_od_table_name is not None and str(hist_od_table_name).strip() != "":
            try:
                hist_od_df = _read_hist_od(conn, hist_od_table_name)
                hist_rows = len(hist_od_df) if hist_od_df is not None else 0
            except Exception as e:
                hist_od_df = None
                hist_rows = 0
                logs.append(f"hist.od read.error: {e.__class__.__name__}: {e}")
        else:
            logs.append("hist.od not provided: skip calibration")
        use_hist_flag = bool(hist_rows > 0)
        logs.append(f"hist.od detected use_calib={use_hist_flag} rows={hist_rows}")

        # 3) distribution compute
        distribution_matrix = _trip_distribution(pa_df, taz_df, alg, on_progress, hist_od_df=hist_od_df)
        logs.append(f"compute.distribution n={len(taz_df)}")
        distribution_df = _distribution_matrix_to_table(distribution_matrix, taz_df)
        logs.append(f"to_table rows={len(distribution_df)}")
        on_progress(8, 80)

        # 4) write
        inserted = _write_trip_distribution_df(conn, out_distribution_table_name, distribution_df)
        logs.append(f"write.od inserted={inserted}")
        on_progress(9, 90)

        ret = {
            "status": 1,
            "message": "Success",
            "data": {
                "tables": {
                    "trip_distribution_table": out_distribution_table_name,
                },
                "counts": {"od_rows": inserted},
            },
            "summary": {
                "stage": "trip_distribution",
                "logs": logs,
                "hist_od_used": int(use_hist_flag),
                "hist_od_rows": int(hist_rows),
            },
        }
        on_progress(10, 100)
        return json.dumps(ret, ensure_ascii=False)

    except Exception as e:
        err = f"trip_distribution error: {e.__class__.__name__}: {e}"
        logs.append(err)
        # determine hist rows if available
        try:
            _rows = len(hist_od_df) if hist_od_df is not None else 0
        except Exception:
            _rows = 0
        ret = {
            "status": 0,
            "message": err,
            "data": {
                "tables": {
                    "trip_distribution_table": out_distribution_table_name,
                }
            },
            "summary": {
                "stage": "trip_distribution",
                "logs": logs,
                "hist_od_used": int(_rows > 0),
                "hist_od_rows": int(_rows),
            },
        }
        try:
            on_progress(10, 100)
        except Exception:
            pass
        return json.dumps(ret, ensure_ascii=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Trip distribution runner')
    parser.add_argument('--cfg-json', type=str, required=True, help='Path to cfg.json')
    parser.add_argument('--taz-table-name', type=str, required=True, help='Fully qualified TAZ table name or relies on search_path')
    parser.add_argument('--trip-generation-table-name', type=str, required=True, help='Fully qualified PA table name produced by generation')
    parser.add_argument('--out-csv', type=str, default=None, help='Path to save OD distribution table as CSV (default: dist/od_demand.csv)')
    args = parser.parse_args()

    with open(args.cfg_json, 'r', encoding='utf-8') as f:
        cfg = json.load(f)

    def progress_cb(iteration: int, percent: float):
        print(iteration, percent)

    data = fs_run_trip_distribution(
        on_progress=progress_cb,
        cfg=cfg,
        taz_table_name=args.taz_table_name,
        trip_generation_table_name=args.trip_generation_table_name
    )
    print(data)

    # 保存分布结果为 CSV（默认 dist/od_demand.csv）
    parent_dir = os.path.dirname(os.path.dirname(__file__))
    dist_dir = os.path.join(parent_dir, 'dist')
    os.makedirs(dist_dir, exist_ok=True)
    if not args.out_csv:
        args.out_csv = os.path.join(dist_dir, 'od_demand.csv')

    if args.out_csv:
        try:
            ret = json.loads(data)
            od_table = ret.get('tables', {}).get('trip_distribution_table')
            if od_table:
                conn = _connect(cfg.get('db_conn_str', {}))
                try:
                    with conn.cursor() as cur:
                        cur.execute(f"SELECT f_id,t_id,demand,m_special FROM {od_table} ORDER BY f_id, t_id")
                        rows = cur.fetchall()
                        cols = [d[0] for d in cur.description]
                    df = pd.DataFrame(rows, columns=cols)
                    df.to_csv(args.out_csv, index=False, encoding='utf-8-sig')
                finally:
                    conn.close()
        except Exception:
            pass
