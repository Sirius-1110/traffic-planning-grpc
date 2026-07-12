"""四阶段法 gRPC 服务入口。

启动方式：
    python -m grpc_server.server [--port 50051]

或由 start.sh 通过 venv 调用。
"""
import os
import sys
import json
import time
import logging
import argparse
import threading
from concurrent import futures

import grpc

# pb2 放在 generated/ 子包中，已随源码提交
from grpc_server.generated import trip_model_pb2 as pb2
from grpc_server.generated import trip_model_pb2_grpc as pb2_grpc

from grpc_server.handlers import (
    trip_generation_handler,
    trip_distribution_handler,
    trip_model_all_handler,
)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger(__name__)

_DEFAULT_PORT = int(os.environ.get("TNA_GRPC_LISTEN", "0.0.0.0:50051").split(":")[-1])


# ──────────────────────────────────────────────────────────────────
# 工具：将 handler 的 on_progress 回调适配为进度帧队列
# ──────────────────────────────────────────────────────────────────
class _ProgressQueue:
    def __init__(self):
        self._frames: list[tuple[int, float]] = []
        self._lock = threading.Lock()

    def push(self, iteration: int, percent: float):
        with self._lock:
            self._frames.append((iteration, percent))

    def drain(self) -> list[tuple[int, float]]:
        with self._lock:
            frames, self._frames = self._frames, []
        return frames


def _make_progress_frame(percent: int, done: int, title: str, code: int = 1, msg: str = "") -> pb2.ProgressData:
    return pb2.ProgressData(
        code=code,
        message=msg,
        data=pb2.ProgressList(percent=percent, done=done, title=title),
    )


# ──────────────────────────────────────────────────────────────────
# Servicer
# ──────────────────────────────────────────────────────────────────
class TripModelServicer(pb2_grpc.FuncServiceServicer):

    # ── 同步接口辅助 ──────────────────────────────────────
    def _run_sync(self, handler_module, request):
        if request.project_id <= 0 or request.user_id <= 0:
            return pb2.ResultData(
                code=-1,
                message="project_id and user_id are required positive integers. case_id is optional.",
                data="{}",
            )
        pq = _ProgressQueue()
        code, msg, data = handler_module.run(
            project_id=request.project_id,
            user_id=request.user_id,
            case_id=request.case_id,
            param1=request.param1,
            param2=request.param2,
            on_progress=pq.push,
        )
        return pb2.ResultData(code=code, message=msg, data=data)

    # ── 流式接口辅助 ──────────────────────────────────────
    def _run_stream(self, handler_module, request, context):
        if request.project_id <= 0 or request.user_id <= 0:
            yield _make_progress_frame(
                100, 1,
                title="{}",
                code=-1,
                msg="project_id and user_id are required positive integers. case_id is optional.",
            )
            return
        # 算法在子线程跑；主线程边轮询边 yield 中间帧，避免一次性堆积。
        result_holder: dict = {}
        done_event = threading.Event()
        pq = _ProgressQueue()

        def worker():
            try:
                code, msg, data = handler_module.run(
                    project_id=request.project_id,
                    user_id=request.user_id,
                    case_id=request.case_id,
                    param1=request.param1,
                    param2=request.param2,
                    on_progress=pq.push,
                )
                result_holder["code"] = code
                result_holder["msg"] = msg
                result_holder["data"] = data
            except Exception as exc:
                result_holder["code"] = -99
                result_holder["msg"] = f"{exc.__class__.__name__}: {exc}"
                result_holder["data"] = "{}"
            finally:
                done_event.set()

        t = threading.Thread(target=worker, daemon=True)
        t.start()

        last_percent = -1
        last_iter = -1

        def _drain_and_yield():
            nonlocal last_percent, last_iter
            for iteration, percent in pq.drain():
                pct_int = int(max(0.0, min(99.0, float(percent))))
                if pct_int < last_percent:
                    pct_int = last_percent
                if pct_int == last_percent and iteration == last_iter:
                    continue
                yield _make_progress_frame(pct_int, 0, f"step {iteration}", code=0, msg="Running")
                last_percent = pct_int
                last_iter = iteration

        while not done_event.is_set():
            yield from _drain_and_yield()
            done_event.wait(0.3)

        # 收尾：把 worker 退出前最后追加进来的帧推完
        yield from _drain_and_yield()

        code = result_holder.get("code", -99)
        msg = result_holder.get("msg", "")
        data = result_holder.get("data", "{}")

        yield _make_progress_frame(
            100, 1,
            title=data,
            code=code,
            msg=msg,
        )

    # ── trip_generation ───────────────────────────────────
    def trip_generation(self, request, context):
        log.info("trip_generation project_id=%d user_id=%d case_id=%d", request.project_id, request.user_id, request.case_id)
        return self._run_sync(trip_generation_handler, request)

    def trip_generation_stream(self, request, context):
        log.info("trip_generation_stream project_id=%d user_id=%d case_id=%d", request.project_id, request.user_id, request.case_id)
        yield from self._run_stream(trip_generation_handler, request, context)

    # ── trip_distribution ─────────────────────────────────
    def trip_distribution(self, request, context):
        log.info("trip_distribution project_id=%d user_id=%d case_id=%d", request.project_id, request.user_id, request.case_id)
        return self._run_sync(trip_distribution_handler, request)

    def trip_distribution_stream(self, request, context):
        log.info("trip_distribution_stream project_id=%d user_id=%d case_id=%d", request.project_id, request.user_id, request.case_id)
        yield from self._run_stream(trip_distribution_handler, request, context)

    # ── trip_model_all ────────────────────────────────────
    def trip_model_all(self, request, context):
        log.info("trip_model_all project_id=%d user_id=%d case_id=%d", request.project_id, request.user_id, request.case_id)
        return self._run_sync(trip_model_all_handler, request)

    def trip_model_all_stream(self, request, context):
        log.info("trip_model_all_stream project_id=%d user_id=%d case_id=%d", request.project_id, request.user_id, request.case_id)
        yield from self._run_stream(trip_model_all_handler, request, context)


# ──────────────────────────────────────────────────────────────────
def serve(port: int):
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=10))
    pb2_grpc.add_FuncServiceServicer_to_server(TripModelServicer(), server)
    listen = f"0.0.0.0:{port}"
    server.add_insecure_port(listen)
    server.start()
    log.info("trip_model gRPC server listening on %s", listen)
    try:
        server.wait_for_termination()
    except KeyboardInterrupt:
        log.info("shutting down")
        server.stop(5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="四阶段法 gRPC 服务")
    parser.add_argument("--port", type=int, default=_DEFAULT_PORT, help="监听端口（默认 50051）")
    args = parser.parse_args()
    serve(args.port)
