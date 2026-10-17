"""Restart one explicitly named disposable test container while retaining a Client and pool."""
import argparse
import json
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", choices=["mysql", "postgres"], required=True)
    parser.add_argument("--container", required=True, help="dedicated Docker test container to stop/start")
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--config-directory", type=Path,
                        help="optional shared scratch directory for a containerized probe")
    parser.add_argument("--recovery-timeout", type=int, default=90)
    parser.add_argument("--mode", choices=["query", "async"], default="query")
    args = parser.parse_args()
    prefix = "SQLCONDUIT_TEST_MYSQL_" if args.engine == "mysql" else "SQLCONDUIT_TEST_PG_"
    with tempfile.TemporaryDirectory(prefix="sqlconduit-live-reset-", dir=args.config_directory) as tmp:
        config = {"default_datasource": "db", "heartbeat_interval_ms": 0,
                  "pool": {"min": 0, "max": 1, "borrow_timeout_ms": 3000},
                  "retry": {"max_attempts": 1},
                  "async": {"threads": 2, "statement_timeout_ms": 3000},
                  "datasources": [{"name": "db", "type": args.engine,
                      "host": os.environ.get(prefix + "HOST", "127.0.0.1"),
                      "port": int(os.environ.get(prefix + "PORT", "3306" if args.engine == "mysql" else "5432")),
                      "user": os.environ.get(prefix + "USER", "root" if args.engine == "mysql" else "postgres"),
                      "password_env": prefix + "PASSWORD",
                      "database": os.environ.get(prefix + "DATABASE", "sqlconduit_test"),
                      "connection_timeout_ms": 2000, "socket_timeout_ms": 2000, "query_timeout_ms": 2000}]}
        config_path = Path(tmp) / "datasource.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        process = subprocess.Popen([str(args.probe.resolve()), str(config_path)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, text=True, bufsize=1)
        messages = queue.Queue()

        def read():
            for line in process.stdout:
                try:
                    messages.put(json.loads(line))
                except ValueError:
                    pass
            messages.put({"eof": True})

        threading.Thread(target=read, daemon=True).start()
        last_result = {}

        def command(value):
            process.stdin.write(value + "\n")
            process.stdin.flush()
            message = messages.get(timeout=15)
            if "ok" not in message:
                raise RuntimeError("probe terminated without a structured result")
            last_result.clear()
            last_result.update(message)
            return message["ok"]

        stopped = False
        try:
            if not messages.get(timeout=15).get("ready") or not command(args.mode):
                raise RuntimeError("initial database probe failed")
            subprocess.run(["docker", "stop", "--time", "2", args.container], check=True, stdout=subprocess.DEVNULL)
            stopped = True
            if command(args.mode):
                raise RuntimeError("cached connection unexpectedly succeeded while the database was stopped")
            subprocess.run(["docker", "start", args.container], check=True, stdout=subprocess.DEVNULL)
            stopped = False
            deadline = time.monotonic() + args.recovery_timeout
            while not command(args.mode):
                if time.monotonic() >= deadline:
                    raise RuntimeError("same Client did not recover after database restart: " + json.dumps(last_result))
                time.sleep(0.5)
            if not command("async"):
                raise RuntimeError("async query failed after reconnection")
            print(f"{args.engine} ({args.mode}): cached disconnect, reconnect and async reuse passed")
        finally:
            if stopped:
                subprocess.run(["docker", "start", args.container], check=False, stdout=subprocess.DEVNULL)
            if process.poll() is None:
                try:
                    process.stdin.write("quit\n")
                    process.stdin.flush()
                    process.wait(timeout=5)
                except (BrokenPipeError, subprocess.TimeoutExpired):
                    process.kill()
                    process.wait(timeout=5)


if __name__ == "__main__":
    main()
