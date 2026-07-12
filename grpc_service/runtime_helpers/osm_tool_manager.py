#!/usr/bin/env python3
"""
OSM Tool Manager - 管理 OSM HTTP 服务器的启动/停止/状态查询
用法：
  python osm_tool_manager.py start [port]
  python osm_tool_manager.py stop
  python osm_tool_manager.py status
"""
import os
import sys
import json
import time
import signal
import subprocess
from pathlib import Path

STATE_FILE = Path(__file__).parent / ".osm_tool_state.json"
LOG_FILE = Path(__file__).parent / "osm_tool_server.log"

# urban_traffic_app 与 tna_total_service 同级：opt_algorithms/{tna_total_service,urban_traffic_app}
DEFAULT_OSM_APP_DIR = Path(__file__).resolve().parent.parent.parent / "urban_traffic_app" / "app"


def load_state():
    if not STATE_FILE.exists():
        return {}
    try:
        with open(STATE_FILE, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def save_state(state):
    with open(STATE_FILE, "w", encoding="utf-8") as f:
        json.dump(state, f, ensure_ascii=False, indent=2)


def is_process_alive(pid):
    if not pid:
        return False
    try:
        os.kill(pid, 0)
        return True
    except (OSError, ProcessLookupError):
        return False


def resolve_osm_app_dir():
    osm_app_env = os.environ.get("OSM_APP_DIR", "").strip()
    if osm_app_env:
        return Path(osm_app_env)
    return DEFAULT_OSM_APP_DIR


def start_server(port=8000):
    state = load_state()
    if "pid" in state and is_process_alive(state["pid"]):
        return {
            "code": 1,
            "message": f"Server already running on port {state.get('port', 8000)}",
            "data": json.dumps(
                {
                    "running": True,
                    "pid": state["pid"],
                    "port": state["port"],
                    "url": f"http://localhost:{state['port']}/map.html",
                    "start_time": state.get("start_time", ""),
                }
            ),
        }

    osm_app_dir = resolve_osm_app_dir()
    server_script = osm_app_dir / "server.py"
    if not server_script.exists():
        return {
            "code": 0,
            "message": f"OSM server.py not found at {server_script}. Set OSM_APP_DIR environment variable.",
            "data": "{}",
        }

    try:
        env = os.environ.copy()
        env["PYTHONUNBUFFERED"] = "1"
        if not env.get("OSM_APP_DIR"):
            env["OSM_APP_DIR"] = str(osm_app_dir)

        log_fp = open(LOG_FILE, "a", encoding="utf-8")
        log_fp.write(f"\n--- start {time.strftime('%Y-%m-%d %H:%M:%S')} port={port} ---\n")
        log_fp.flush()

        process = subprocess.Popen(
            [sys.executable, str(server_script)],
            cwd=str(osm_app_dir),
            env=env,
            stdout=log_fp,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )

        time.sleep(1.5)
        if process.poll() is not None:
            return {
                "code": 0,
                "message": f"Server failed to start (process died immediately). See {LOG_FILE}",
                "data": "{}",
            }

        start_time = time.strftime("%Y-%m-%d %H:%M:%S")
        state = {"pid": process.pid, "port": port, "start_time": start_time}
        save_state(state)

        return {
            "code": 1,
            "message": "Server started successfully",
            "data": json.dumps(
                {
                    "running": True,
                    "pid": process.pid,
                    "port": port,
                    "url": f"http://localhost:{port}/map.html",
                    "start_time": start_time,
                }
            ),
        }
    except Exception as e:
        return {"code": 0, "message": f"Failed to start server: {e}", "data": "{}"}


def stop_server():
    state = load_state()
    if "pid" not in state:
        return {"code": 1, "message": "No server process found in state file", "data": "{}"}

    pid = state["pid"]
    if not is_process_alive(pid):
        if STATE_FILE.exists():
            STATE_FILE.unlink()
        return {"code": 1, "message": "Server process is not running", "data": "{}"}

    try:
        os.kill(pid, signal.SIGTERM)
        for _ in range(30):
            if not is_process_alive(pid):
                break
            time.sleep(0.1)
        if is_process_alive(pid):
            os.kill(pid, signal.SIGKILL)
            time.sleep(0.5)
        if STATE_FILE.exists():
            STATE_FILE.unlink()
        return {"code": 1, "message": "Server stopped successfully", "data": "{}"}
    except Exception as e:
        return {"code": 0, "message": f"Failed to stop server: {e}", "data": "{}"}


def get_status():
    state = load_state()
    if "pid" not in state:
        return {
            "code": 1,
            "message": "Server is not running",
            "data": json.dumps(
                {"running": False, "pid": 0, "port": 0, "url": "", "uptime_seconds": 0}
            ),
        }

    pid = state["pid"]
    port = state.get("port", 8000)
    start_time_str = state.get("start_time", "")

    if not is_process_alive(pid):
        if STATE_FILE.exists():
            STATE_FILE.unlink()
        return {
            "code": 1,
            "message": "Server process has died",
            "data": json.dumps(
                {"running": False, "pid": 0, "port": 0, "url": "", "uptime_seconds": 0}
            ),
        }

    uptime = 0
    if start_time_str:
        try:
            start_time = time.mktime(time.strptime(start_time_str, "%Y-%m-%d %H:%M:%S"))
            uptime = int(time.time() - start_time)
        except Exception:
            pass

    return {
        "code": 1,
        "message": "Server is running",
        "data": json.dumps(
            {
                "running": True,
                "pid": pid,
                "port": port,
                "url": f"http://localhost:{port}/map.html",
                "uptime_seconds": uptime,
                "start_time": start_time_str,
            }
        ),
    }


def main():
    if len(sys.argv) < 2:
        print(
            json.dumps(
                {
                    "code": 0,
                    "message": "Usage: osm_tool_manager.py {start|stop|status} [port]",
                    "data": "{}",
                }
            )
        )
        return

    command = sys.argv[1].lower()
    if command == "start":
        port = int(sys.argv[2]) if len(sys.argv) > 2 else 8000
        result = start_server(port)
    elif command == "stop":
        result = stop_server()
    elif command == "status":
        result = get_status()
    else:
        result = {"code": 0, "message": f"Unknown command: {command}", "data": "{}"}

    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
