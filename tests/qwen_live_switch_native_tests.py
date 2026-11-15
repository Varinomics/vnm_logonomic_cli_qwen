"""Opt-in Linux Qwen TUI -> packaged helper -> packaged adapter acceptance.

Run with package directory, SDK host simulator, qwen_contract_tests transport,
and an installed native command after --. No provider account is used. The
fixture supplies an isolated profile and a scripted loopback OpenAI endpoint.
It does not exercise Logonomic's desktop projection or native wake delivery.
"""

import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid

from qwen_native_test_support import native_environment, send_model_response

if sys.platform == "linux":
    import fcntl
    import pty
    import select
    import termios


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def append_json(path, value):
    descriptor = os.open(path, os.O_APPEND | os.O_CREAT | os.O_WRONLY, 0o600)
    try:
        os.write(descriptor, (json.dumps(value) + "\n").encode())
    finally:
        os.close(descriptor)


def read_json_lines(path):
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def run_hook(root, helper):
    """Delay only clear-end delivery; leave native event identity untouched."""
    source = sys.stdin.buffer.read()
    native = json.loads(source)
    if native["hook_event_name"] == "SessionEnd" and native.get("reason") == "clear":
        deadline = time.monotonic() + 20
        while not (root / "release-clear-end").exists() and time.monotonic() < deadline:
            time.sleep(0.05)
    capture = root / ("delivery-" + uuid.uuid4().hex + ".jsonl")
    environment = dict(os.environ, QWEN_TEST_HELPER_CAPTURE=str(capture))
    result = subprocess.run([str(helper)], input=source, capture_output=True,
                            env=environment, timeout=25, check=False)
    require(result.returncode == 0, "packaged native hook helper failed")
    deliveries = read_json_lines(capture)
    require(len(deliveries) == 2 and deliveries[0] == deliveries[1],
            "helper retry changed native event or transition identity")
    append_json(root / "deliveries.jsonl", {"native": native, "delivery": deliveries[0]})
    sys.stdout.buffer.write(result.stdout)
    sys.stderr.buffer.write(result.stderr)


class Native_terminal:
    def __init__(self, command, environment, cwd, output):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 36, 120, 0, 0))
        self.log = output.open("wb")
        self.output = b""
        self.process = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave,
                                        env=environment, cwd=cwd, start_new_session=True)
        os.close(slave)

    def pump(self, duration=0.1):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.master], [], [],
                                         max(0, min(0.1, deadline - time.monotonic())))
            if ready:
                try:
                    chunk = os.read(self.master, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                self.output += chunk
                self.log.write(chunk)
                self.log.flush()

    def wait(self, predicate, description, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            require(self.process.poll() is None, "Qwen exited while waiting for " + description)
            self.pump()
        raise RuntimeError("Timed out waiting for " + description)

    def send(self, text):
        os.write(self.master, text.encode())
        self.pump(0.2)
        os.write(self.master, b"\r")

    def close(self):
        if self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGTERM)
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait()
        self.pump()
        self.log.close()
        os.close(self.master)


def run_native(args, root):
    profile, runtime, home, workspace = [root / name for name in ("profile", "runtime", "home", "workspace")]
    for path in (profile, runtime, home, workspace):
        path.mkdir()
    helper = args.package / "adapter" / "qwen_helper"
    hook_command = shlex.join([sys.executable, str(Path(__file__).resolve()), "--hook", str(root), str(helper)])
    settings = {
        "hooks": {name: [{"hooks": [{"type": "command", "command": hook_command,
                                    "shell": "bash", "timeout": 25}]}]
                  for name in ("SessionStart", "SessionEnd", "UserPromptSubmit", "Stop", "StopFailure")},
        "security": {"auth": {"selectedType": "openai"}},
        "telemetry": {"enabled": False},
        "ui": {"hideBanner": True, "hideTips": True},
    }
    (profile / "settings.json").write_text(json.dumps(settings))
    (profile / "trustedFolders.json").write_text(json.dumps({str(workspace): "TRUST_FOLDER"}))
    (root / "system.json").write_text("{}")

    class Model_handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            require(self.headers.get("Authorization") == "Bearer native-fixture-key", "unexpected model credential")
            append_json(root / "model-requests.jsonl", {"path": self.path, "request": request})
            require(request.get("stream"), "fixture expects a streaming OpenAI request")
            send_model_response(self, request, {"role": "assistant", "content": "Native fixture response."})

    socket_root = Path(tempfile.mkdtemp(prefix="qwen-run-"))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Model_handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    environment = native_environment(root, socket_root)
    environment.update({
        "LOGONOMIC_VNMCTL_PATH": str(args.transport), "LOGONOMIC_SESSION_CAPABILITY": "c" * 64,
        "LOGONOMIC_PROVIDER_PACKAGE_ID": "logonomic.cli.qwen",
        "LOGONOMIC_PROVIDER_GENERATION_DIGEST": "d" * 64, "LOGONOMIC_PROVIDER_GENERATION_NONCE": "e" * 32,
        "LOGONOMIC_PROVIDER_HELPERS_DIR": str(args.package / "helpers"),
    })
    native_command = args.native_command + ["--auth-type", "openai", "--model", "fixture-model",
        "--openai-api-key", "native-fixture-key", "--openai-base-url", f"http://127.0.0.1:{server.server_port}/v1"]
    version = subprocess.run(args.native_command + ["--version"], env=environment, cwd=workspace,
                             text=True, capture_output=True, timeout=15, check=True).stdout.strip()
    ledger = {"current_epoch": 0, "current_transition": None}
    invocation_count = 0

    def invoke(capability, version, operation, payload):
        nonlocal invocation_count
        invocation_count += 1
        script = root / f"invoke-{invocation_count}.json"
        transcript = root / f"invoke-{invocation_count}.txt"
        steps = []
        if operation == "classify_event" and payload.get("event", {}).get("name") == "SessionStart":
            steps.extend([
                {"op": "invoke", "id": "reserve", "capability_id": "lifecycle-correlation-lifetime",
                 "version": 1, "operation": "reserve", "deadline_ms": 5000,
                 "payload": {"transition_nonce": payload["transition_nonce"]}},
                {"op": "wait_result", "id": "reserve", "timeout_ms": 8000},
            ])
        steps.extend([
            {"op": "invoke", "id": "request", "capability_id": capability, "version": version,
             "operation": operation, "payload": payload, "deadline_ms": 5000},
            {"op": "wait_result", "id": "request", "timeout_ms": 8000},
            {"op": "shutdown", "grace_ms": 3000}])
        script.write_text(json.dumps({"script_version": 1, "steps": steps}))
        subprocess.run([str(args.simulator), str(args.package), "--script", str(script),
                        "--transcript", str(transcript), "--platform", "linux-x86_64"],
                       capture_output=True, text=True, timeout=15, check=True)
        frames = [json.loads(line[2:]) for line in transcript.read_text().splitlines() if line.startswith("< ")]
        results = [frame for frame in frames if frame.get("type") == "result"]
        expected = sum(step["op"] == "invoke" for step in steps)
        require(len(results) == expected and all(result["status"] == "success" for result in results),
                "packaged operation failed: " + str(results))
        return results[-1]["payload"]

    def classify(delivery, transition=True):
        result = invoke("helper-ingress", 2, "classify_event", {
            "event": delivery["event"], "ledger": ledger, "transition_nonce": delivery["transition_nonce"],
            "execution_context": {"version": 1}})
        if not transition:
            require(result["classification"] == "ignore" and not result["observations"], "delayed clear-end ended returning A")
            return None
        body = delivery["event"]["body"]
        require(result["classification"] == "transition" and len(result["observations"]) == 1, "native switch was not classified")
        observation = result["observations"][0]
        require(observation["provider_session_id"] == body["session_id"] and not observation["provider_turn_id"],
                "native activation borrowed the departed turn identity")
        update = result["execution_context"]["updates"][0]
        require(update["state"] == "confirmed" and update["cwd"] == str(workspace)
                and update["transition_nonce"] == delivery["transition_nonce"], "native cwd lost activation binding")
        record = observation["resume_record"]
        require(record["native_source"] == {"transcript_path": body["transcript_path"],
                "cwd": str(workspace), "profile_home": str(profile)}, "native resume source was not preserved")
        ledger["current_epoch"] += 1
        ledger["current_transition"] = {"kind": result["transition"]["kind"],
            "provider_session_id": body["session_id"], "nonce": delivery["transition_nonce"]}
        return record

    def inspect(record, availability, content=None):
        # Runtime helper transport metadata is injected after launcher resolution.
        launch_environment = {key: value for key, value in environment.items()
                              if not key.startswith("LOGONOMIC_")}
        payload = {"launcher": {"script_body": shlex.join(args.native_command), "runner_key": "direct", "environment_text": ""},
            "launcher_argv": args.native_command, "working_directory": str(workspace), "declared_assets": [],
            "resume": {"version": 1, "record": record},
            "launch_environment": {"platform": "linux-x86_64", "effective_environment": launch_environment,
                                   "bootstrap": {"version": 1, "resolved_home": str(profile)}}}
        result = invoke("interactive-resume", 4, "inspect_resume", payload)
        digest = hashlib.sha256(json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()).hexdigest()
        require(result["request_digest"] == digest and result["availability"] == availability,
                "native source inspection was not exact: " + str(result))
        if content is not None:
            require(result["content"] == content, "native conversation content was misclassified")
        return result

    def deliveries(name):
        return [item["delivery"] for item in read_json_lines(root / "deliveries.jsonl") if item["delivery"]["event"]["name"] == name]

    terminal = Native_terminal(native_command, environment, workspace, root / "terminal.bin")
    fork = None
    try:
        terminal.wait(lambda: len(deliveries("SessionStart")) == 1, "native startup")
        initial = classify(deliveries("SessionStart")[0])
        require(not Path(initial["native_source"]["transcript_path"]).exists(),
                "fresh native startup unexpectedly contains a transcript")
        inspect(initial, "unknown", "unknown")
        terminal.wait(lambda: b"Type your message" in terminal.output, "native input readiness")
        terminal.send("Native session switching fixture.")
        terminal.wait(lambda: bool(deliveries("Stop")), "native completed turn")
        inspect(initial, "available", "nonempty")
        terminal.send("/clear")
        terminal.wait(lambda: len(deliveries("SessionStart")) == 2, "native clear")
        cleared = classify(deliveries("SessionStart")[1])
        require(cleared["provider_session_id"] != initial["provider_session_id"], "clear retained the old identity")
        require(not Path(cleared["native_source"]["transcript_path"]).exists(),
                "fresh native clear unexpectedly contains a transcript")
        inspect(cleared, "unknown", "unknown")
        terminal.send("/resume " + initial["provider_session_id"])
        terminal.wait(lambda: len(deliveries("SessionStart")) == 3, "idle native resume")
        returning = classify(deliveries("SessionStart")[2])
        require(returning == initial, "native resume changed the original store or conversation")
        require(not deliveries("SessionEnd"), "native clear-end escaped the deliberate delay")
        (root / "release-clear-end").touch()
        terminal.wait(lambda: bool(deliveries("SessionEnd")), "delayed native clear-end")
        classify(deliveries("SessionEnd")[0], transition=False)
        inspect(returning, "available", "nonempty")
        terminal.send("/resume " + str(uuid.uuid4()))
        terminal.wait(lambda: b"No session found" in terminal.output, "missing native resume refusal")
        require(len(deliveries("SessionStart")) == 3, "failed resume activated a conversation")
        terminal.send("/resume")
        terminal.pump(1)
        os.write(terminal.master, b"\x1b")
        terminal.pump(1)
        require(len(deliveries("SessionStart")) == 3, "cancelled picker activated a conversation")
        fork = Native_terminal(native_command + ["--resume", initial["provider_session_id"], "--fork-session"],
                               environment, workspace, root / "fork-terminal.bin")
        fork.wait(lambda: len(deliveries("SessionStart")) == 4, "native inherited fork")
        # --fork-session starts a distinct terminal; it has its own host ledger.
        ledger.update(current_epoch=0, current_transition=None)
        fork_record = classify(deliveries("SessionStart")[3])
        require(fork_record["provider_session_id"] not in (initial["provider_session_id"], cleared["provider_session_id"]),
                "native fork did not create a distinct conversation")
        inspect(fork_record, "available", "nonempty")
        summary = {"native_version": version, "native_command": args.native_command,
                   "package": str(args.package), "status": "passed",
                   "activation_ids": [initial["provider_session_id"], cleared["provider_session_id"], returning["provider_session_id"]],
                   "checks": ["idle clear/resume A-B-A", "fresh missing source stays unknown; retained content nonempty", "delayed clear-end after return",
                              "missing and cancelled resume", "native fork inherited content", "helper retry identity",
                              "exact runtime/profile provenance", "activation-bound cwd"]}
        (root / "result.json").write_text(json.dumps(summary, indent=2))
        print(json.dumps(summary))
    finally:
        (root / "release-clear-end").touch()
        if fork:
            fork.close()
        terminal.close()
        server.shutdown()
        server.server_close()
        shutil.rmtree(socket_root)


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--hook":
        run_hook(Path(sys.argv[2]), Path(sys.argv[3]))
        return 0
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, help="Keep native evidence beneath this existing directory")
    parser.add_argument("package", type=Path)
    parser.add_argument("simulator", type=Path)
    parser.add_argument("transport", type=Path)
    parser.add_argument("native_command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.native_command[:1] == ["--"]:
        args.native_command.pop(0)
    if (sys.platform != "linux" or platform.machine().lower() not in ("x86_64", "amd64")
            or not args.native_command or not shutil.which(args.native_command[0])):
        print("SKIP: this native fixture requires Linux x86-64 and an explicitly installed Qwen command")
        return 77
    args.package, args.simulator, args.transport = [path.resolve() for path in (args.package, args.simulator, args.transport)]
    root = Path(tempfile.mkdtemp(prefix="qwen-live-", dir=args.output_dir))
    try:
        run_native(args, root)
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
