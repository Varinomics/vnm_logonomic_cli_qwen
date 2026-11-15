"""Opt-in native ACP background lifetime regression with a loopback-only model.

Use an installed Qwen command and an SDK-valid provider package. This exercises
the real adapter's prepare/inspect path; it does not qualify shipping packaging.
"""

import argparse
import base64
import hashlib
import http.server
import json
import os
from pathlib import Path
import platform
import queue
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

from qwen_native_test_support import native_environment, send_model_response


ROOT_PROMPT = "NATIVE_ROOT_LIFETIME_FIXTURE"
CHILD_PROMPT = "NATIVE_CHILD_DELAYED_FIXTURE"
EXPECTED_ANSWER = "INITIAL_PARENT_COMPLETIONFINAL_PARENT_SYNTHESIS"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def chunks(data):
    return [base64.b64encode(data[start:start + 65536]).decode() for start in range(0, len(data), 65536)]


def unchunks(parts):
    return b"".join(base64.b64decode(part, validate=True) for part in parts)


def message_text(value):
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        return "".join(part.get("text", "") for part in value if isinstance(part, dict))
    return value.get("text", "") if isinstance(value, dict) else ""


def run_native(args, root, workspace, socket_root):
    for name in ("home", "profile", "runtime", "bin"):
        (root / name).mkdir()
    # Keep the persistent SDK host on the same supplied, indexed package generation.
    # Copy links as links so SDK validation still rejects invalid package content.
    package = root / "package"
    shutil.copytree(args.package, package, symlinks=True)
    (root / "system.json").write_text("{}")
    (root / "profile/settings.json").write_text(json.dumps({
        "security": {"auth": {"selectedType": "openai"}}, "permissions": {"allow": ["Agent"]},
        "telemetry": {"enabled": False}, "ui": {"hideBanner": True, "hideTips": True}}))
    (root / "profile/trustedFolders.json").write_text(json.dumps({str(workspace): "TRUST_FOLDER"}))
    launcher = root / "bin/qwen"
    launcher.write_text("#!/bin/sh\nexec " + shlex.join(args.native_command) + ' "$@"\n')
    launcher.chmod(0o700)
    parent_responded, background_admitted, child_replied = (threading.Event() for _ in range(3))
    record_lock = threading.Lock()
    model_requests, model_errors = [], []
    model_progress = {"discovered": False, "spawned": False}

    def record(name, value):
        with record_lock, (root / name).open("a") as stream:
            stream.write(json.dumps(value) + "\n")

    class Model_handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            try:
                require(self.headers.get("Authorization") == "Bearer native-fixture-key", "unexpected model credential")
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                with record_lock:
                    model_requests.append(request)
                    number = len(model_requests)
                record("model-requests.jsonl", {"number": number, "request": request})
                users = [message_text(message.get("content")) for message in request.get("messages", [])
                         if message.get("role") == "user"]
                last_user = users[-1] if users else ""
                root_request = ROOT_PROMPT in "\n".join(users)
                tool_names = [tool.get("function", {}).get("name") for tool in request.get("tools", [])]
                if CHILD_PROMPT in last_user and ROOT_PROMPT not in last_user:
                    require(parent_responded.wait(45), "parent never completed while its native child was blocked")
                    message, role = {"role": "assistant", "content": "NATIVE_CHILD_FINISHED"}, "child"
                elif root_request and not model_progress["discovered"]:
                    require("tool_search" in tool_names, "native deferred tool search unavailable: " + str(tool_names))
                    model_progress["discovered"] = True
                    message = {"role": "assistant", "tool_calls": [{"index": 0, "id": "fixture-search-call", "type": "function",
                        "function": {"name": "tool_search", "arguments": json.dumps({"query": "select:agent"})}}]}
                    role = "discover"
                elif root_request and not model_progress["spawned"]:
                    require("tool_call" in tool_names, "native deferred tool bridge unavailable: " + str(tool_names))
                    model_progress["spawned"] = True
                    message = {"role": "assistant", "tool_calls": [{"index": 0, "id": "fixture-agent-call", "type": "function",
                        "function": {"name": "tool_call", "arguments": json.dumps({"name": "agent", "arguments": {
                            "description": "Delayed native fixture child", "prompt": CHILD_PROMPT,
                            "run_in_background": True}})}}]}
                    role = "spawn"
                elif root_request and background_admitted.is_set():
                    message, role = {"role": "assistant", "content": "FINAL_PARENT_SYNTHESIS"}, "synthesis"
                elif root_request:
                    message, role = {"role": "assistant", "content": "INITIAL_PARENT_COMPLETION"}, "initial"
                else:
                    message, role = {"role": "assistant", "content": "Native lifetime fixture"}, "advisory"
                record("model-decisions.jsonl", {"number": number, "role": role})
                send_model_response(self, request, message, f"fixture-{number}")
                if role == "child":
                    child_replied.set()
            except Exception as error:
                model_errors.append(str(error))
                record("model-errors.jsonl", {"error": str(error)})

    invocation_count = 0
    driver = None
    driver_output = queue.Queue()

    def invoke(operation, payload):
        nonlocal invocation_count
        invocation_count += 1
        request = {"operation": operation, "payload": payload}
        (root / f"invoke-{invocation_count:04d}.json").write_text(json.dumps(request))
        driver.stdin.write(json.dumps(request) + "\n")
        driver.stdin.flush()
        line = driver_output.get(timeout=25)
        require(line is not None, "SDK driver exited before returning the provider outcome")
        result = json.loads(line)
        (root / f"invoke-{invocation_count:04d}-result.json").write_text(json.dumps(result))
        require(result.get("status") == "success", "SDK operation failed: " + str(result))
        return result["payload"]

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Model_handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    process = None
    stderr_path = root / "native-stderr.txt"
    try:
        environment = native_environment(root, socket_root)
        environment.update({"PATH": str(root / "bin") + os.pathsep + environment["PATH"], "NODE_DISABLE_COMPILE_CACHE": "1"})
        with (root / "adapter-stderr.txt").open("w") as stderr:
            driver = subprocess.Popen([str(args.driver), "--native-acp-driver", str(package), str(root / "adapter-transcript.txt")],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr, text=True, bufsize=1, env=environment)

        def read_driver():
            for line in driver.stdout:
                driver_output.put(line)
            driver_output.put(None)

        threading.Thread(target=read_driver, daemon=True).start()
        ready_line = driver_output.get(timeout=25)
        require(ready_line is not None, "SDK driver failed before validating the supplied package")
        ready = json.loads(ready_line)
        require(ready.get("status") == "ready" and ready.get("package_id") == "logonomic.cli.qwen"
                and ready.get("generation_digest") and ready.get("generation_nonce"), "SDK driver did not bind a Qwen generation")
        (root / "adapter-ready.json").write_text(json.dumps(ready, indent=2))
        version = subprocess.run(args.native_command + ["--version"], env=environment, cwd=workspace,
                                 capture_output=True, text=True, timeout=20, check=True).stdout.strip()
        argv = [str(launcher), "--approval-mode", "default", "--model", "fixture-model",
                "--openai-api-key", "native-fixture-key", "--openai-base-url", f"http://127.0.0.1:{server.server_port}/v1"]
        prepared = invoke("prepare", {"purpose": "implementation",
            "launcher": {"script_body": shlex.join(argv), "runner_key": "direct", "environment_text": ""},
            "launcher_argv": argv, "launch_environment": {"platform": "linux-x86_64", "effective_environment": environment,
                "bootstrap": {"version": 1, "resolved_home": str(root / "profile")}},
            "working_directory": str(workspace), "prompt_base64": base64.b64encode(ROOT_PROMPT.encode()).decode(),
            "declared_assets": []})
        (root / "prepared.json").write_text(json.dumps(prepared, indent=2))
        proposal = prepared["process"]
        require(proposal["executable"] == "helpers/qwen_launch_bridge" and proposal["start"] == "attached"
                and proposal["working_directory"] == "session_workspace" and proposal["argv"] == argv + ["--acp"],
                "unexpected native process proposal: " + str(proposal))
        require(prepared["inspection"]["incremental"] and prepared["inspection"]["keep_stdin_open"],
                "native ACP preparation did not retain incremental stdin ownership")
        environment.update(proposal["environment"])
        with stderr_path.open("wb") as stderr:
            process = subprocess.Popen([str(package / proposal["executable"])] + proposal["argv"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr, cwd=workspace,
                env=environment, start_new_session=True)
        received = queue.Queue()

        def read_stdout():
            for line in process.stdout:
                received.put(line)
            received.put(None)

        threading.Thread(target=read_stdout, daemon=True).start()
        requests = {}
        native_session = None
        synthesis_complete = background_end = close_acknowledged = eof_requested = False

        def send(data):
            for line in data.splitlines():
                outgoing = json.loads(line)
                record("client-frames.jsonl", outgoing)
                if "id" in outgoing and "method" in outgoing:
                    requests[outgoing["id"]] = outgoing["method"]
                if outgoing.get("method") == "qwen/control/session/close":
                    require(synthesis_complete and background_end, "provider closed before parent synthesis completed")
                    require(outgoing["params"] == {"sessionId": native_session, "onlyIfUnheld": True,
                            "requireFlush": True, "drainTimeoutMs": 8000}, "provider close lost its conditional flush boundary")
            process.stdin.write(data)
            process.stdin.flush()

        send(proposal["stdin_payload"].encode())
        state = prepared["inspection"]["protocol_state_base64_chunks"]
        final_answer = None
        pending_eof = False
        started = time.monotonic()
        while time.monotonic() - started < 90:
            try:
                line = None if pending_eof else received.get(timeout=1)
            except queue.Empty:
                continue
            if line is None:
                exit_code = process.wait(timeout=5)
                result = invoke("inspect", {"purpose": "implementation", "inspection": {
                    "protocol_state_base64_chunks": state, "stdout_base64_chunks": [],
                    "stderr_base64_chunks": chunks(stderr_path.read_bytes()),
                    "final": True, "timed_out": False, "exit_code": exit_code}})
                record("inspection-results.jsonl", result)
                require(exit_code == 0 and result["complete"] and "failure_reason" not in result,
                        "native final inspection failed: " + str(result))
                final_answer = unchunks(result["result_base64_chunks"]).decode()
                break
            # Native stdout can coalesce notifications. Drain what is already queued,
            # stopping at one RPC boundary so its reply is sent before another RPC.
            # The SDK host remains live for the entire native exchange.
            lines = [line]
            while len(lines) < 256 and "id" not in json.loads(lines[-1]):
                try:
                    following = received.get_nowait()
                except queue.Empty:
                    break
                if following is None:
                    pending_eof = True
                    break
                lines.append(following)
            initial_parent = False
            inspected_frames = []
            for line in lines:
                value = json.loads(line)
                record("native-frames.jsonl", value)
                method, params = value.get("method"), value.get("params", {})
                inspected_frames.append({"method": method, "id": value.get("id")})
                response_to = requests.get(value.get("id")) if method is None else None
                initial_response = response_to == "session/prompt"
                initial_parent = initial_parent or initial_response
                if response_to == "session/new":
                    native_session = value["result"]["sessionId"]
                if initial_response:
                    require(value.get("result", {}).get("stopReason") == "end_turn" and not child_replied.is_set(),
                            "initial parent did not complete while its native child remained blocked")
                if method == "_qwencode/start_turn" and "id" in value:
                    require(params.get("sessionId") == native_session and params.get("source") == "background_notification",
                            "unexpected native background turn")
                    background_admitted.set()
                if method == "session/update":
                    meta = params.get("update", {}).get("_meta", {})
                    if meta.get("source") == "background_notification_response" and meta.get("backgroundTask", {}).get("turnComplete"):
                        require(not meta["backgroundTask"].get("partial"), "native parent synthesis was partial")
                        synthesis_complete = True
                if method == "_qwencode/end_turn" and params.get("source") == "background_notification":
                    require(params.get("reason") == "end_turn", "native background turn failed")
                    background_end = True
                if response_to == "qwen/control/session/close" and value.get("result", {}).get("closed") is True:
                    require(value["result"].get("sessionId") == native_session, "native close acknowledged a different session")
                    close_acknowledged = True
            result = invoke("inspect", {"purpose": "implementation", "inspection": {
                "protocol_state_base64_chunks": state, "stdout_base64_chunks": chunks(b"".join(lines)), "stderr_base64_chunks": [],
                "final": False, "timed_out": False, "exit_code": 0}})
            record("inspection-results.jsonl", {"native_frames": inspected_frames, "result": result})
            require("failure_reason" not in result, "provider rejected native frames: " + str({"frames": inspected_frames, "result": result}))
            require(not result["complete"], "provider completed before native process exit")
            state = result["protocol_state_base64_chunks"]
            if initial_parent:
                require(not result.get("close_stdin") and "stdin_base64_chunks" not in result,
                        "provider released native ACP at the initial parent end_turn")
                parent_responded.set()
            if "stdin_base64_chunks" in result:
                send(unchunks(result["stdin_base64_chunks"]))
            if result.get("close_stdin"):
                require(close_acknowledged and not eof_requested, "provider EOF preceded close acknowledgment or repeated")
                eof_requested = True
                process.stdin.close()
        require(parent_responded.is_set() and child_replied.is_set() and background_admitted.is_set()
                and synthesis_complete and background_end and close_acknowledged and eof_requested,
                "native background lifecycle did not reach every required boundary")
        require(final_answer == EXPECTED_ANSWER, "provider lost parent synthesis or exposed child/status text: " + str(final_answer))
        require(not model_errors, "loopback model failed: " + str(model_errors))
        driver.stdin.close()
        require(driver.wait(timeout=10) == 0, "SDK driver did not shut down its adapter cleanly")
        require(driver_output.get(timeout=5) is None, "SDK driver returned an unconsumed outcome")
        summary = {"version": version, "scope": "native ACP behavior through real provider and SDK; not shipping package qualification",
            "generation_digest": ready["generation_digest"],
            "package_index_sha256": hashlib.sha256((package / "package.files.json").read_bytes()).hexdigest(),
            "initial_parent_before_child": True, "native_synthesis_complete": synthesis_complete,
            "conditional_close_acknowledged": close_acknowledged, "eof_requested_after_close": eof_requested,
            "exit_code": process.returncode, "result": final_answer,
            "provider_invocations": invocation_count, "model_requests": len(model_requests)}
        (root / "result.json").write_text(json.dumps(summary, indent=2))
        print(json.dumps(summary))
    finally:
        parent_responded.set()
        if process and process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)
        if driver and driver.poll() is None:
            if not driver.stdin.closed:
                driver.stdin.close()
            try:
                driver.wait(timeout=25)
            except subprocess.TimeoutExpired:
                driver.terminate()
                try:
                    driver.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    driver.kill()
                    driver.wait(timeout=5)
        server.shutdown()
        server.server_close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, help="Keep native evidence beneath this existing directory")
    parser.add_argument("package", type=Path)
    parser.add_argument("driver", type=Path, help="Built qwen_contract_tests with its SDK native ACP driver mode")
    parser.add_argument("native_command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.native_command[:1] == ["--"]:
        args.native_command.pop(0)
    if (sys.platform != "linux" or platform.machine().lower() not in ("x86_64", "amd64")
            or not args.native_command or not shutil.which(args.native_command[0])):
        print("SKIP: this native fixture requires Linux x86-64 and an explicitly installed Qwen command")
        return 77
    args.native_command[0] = str(Path(shutil.which(args.native_command[0])).resolve())
    args.package, args.driver = [path.resolve() for path in (args.package, args.driver)]
    root = Path(tempfile.mkdtemp(prefix="qwen-acp-", dir=args.output_dir)).resolve()
    try:
        # Avoid inheriting project settings or QWEN.md from the evidence directory.
        with tempfile.TemporaryDirectory(prefix="qwen-acp-work-") as workspace, tempfile.TemporaryDirectory(prefix="qwen-run-") as socket_root:
            run_native(args, root, Path(workspace), Path(socket_root))
    except Exception:
        print("Native fixture evidence: " + str(root), file=sys.stderr)
        raise
    if args.output_dir is None:
        shutil.rmtree(root)
    else:
        print("Native fixture evidence: " + str(root))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
