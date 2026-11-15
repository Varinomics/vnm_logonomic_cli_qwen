"""Check the packaged helper's native identity and directory evidence boundary."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    package = Path(sys.argv[1]).resolve()
    wrapper = Path(sys.argv[2]).resolve()
    helper = package / "adapter" / ("qwen_helper.exe" if os.name == "nt" else "qwen_helper")
    session = "12345678-1234-4234-8234-123456789abc"
    with tempfile.TemporaryDirectory(prefix="qwen-helper-") as temporary:
        root = Path(temporary).resolve()
        cwd = str(root / "native cwd" / "nested")
        transcript = str(root / "native runtime" / "projects" / "project" / "chats" / f"{session}.jsonl")
        environment = {
            key: os.environ[key]
            for key in ("PATH", "SystemRoot", "WINDIR", "TEMP", "TMP")
            if key in os.environ
        }
        environment.update({
            "LOGONOMIC_VNMCTL_PATH": str(wrapper),
            "LOGONOMIC_SESSION_CAPABILITY": "a" * 64,
            "LOGONOMIC_PROVIDER_PACKAGE_ID": "logonomic.cli.qwen",
            "LOGONOMIC_PROVIDER_GENERATION_DIGEST": "b" * 64,
            "LOGONOMIC_PROVIDER_GENERATION_NONCE": "c" * 32,
            "LOGONOMIC_PROVIDER_HELPERS_DIR": str(package / "helpers"),
            "QWEN_PROJECT_DIR": str(root / "unconfirmed fallback"),
            "QWEN_LOGONOMIC_PROFILE_HOME": str(root / "prepared-profile"),
        })
        for source in ("startup", "clear", "resume", "branch"):
            capture = root / f"{source}.jsonl"
            environment["QWEN_TEST_HELPER_CAPTURE"] = str(capture)
            native = {
                "hook_event_name": "SessionStart",
                "session_id": session,
                "source": source,
                "cwd": cwd,
                "transcript_path": transcript,
                "prompt_id": "87654321-1234-4234-8234-123456789abc########1",
                "event_id": "native-input-cannot-replace-delivery-identity",
                "profile_home": str(root / "untrusted-hook-profile"),
            }
            completed = subprocess.run(
                [str(helper)], input=json.dumps(native), text=True, capture_output=True,
                env=environment, cwd=root, timeout=30, check=True,
            )
            assert not completed.stdout, "an unaccepted title instruction must remain absent"
            deliveries = [json.loads(line) for line in capture.read_text(encoding="utf-8").splitlines()]
            assert len(deliveries) == 2, "the wrapper fixture must exercise the helper retry"
            assert deliveries[0] == deliveries[1], "retry must preserve all evidence and activation identity"
            event = deliveries[0]["event"]
            assert event["name"] == "SessionStart"
            body = event["body"]
            for key in ("session_id", "source", "cwd", "transcript_path", "prompt_id"):
                assert body[key] == native[key], f"native {key} evidence changed at the helper boundary"
            assert body["event_id"] == deliveries[0]["idempotency_key"]
            assert body["event_id"] != native["event_id"]
            assert len(deliveries[0]["transition_nonce"]) == 32
            assert body["profile_home"] == environment["QWEN_LOGONOMIC_PROFILE_HOME"]

        # Native child hooks identify their owning root session and the child
        # separately; both facts must reach authenticated provider ingress.
        for event_name in ("SubagentStart", "SubagentStop"):
            capture = root / f"{event_name}.jsonl"
            environment["QWEN_TEST_HELPER_CAPTURE"] = str(capture)
            native = {
                "hook_event_name": event_name,
                "session_id": session,
                "agent_id": "code-reviewer-a1b2",
                "agent_type": "code-reviewer",
                "cwd": cwd,
                "transcript_path": transcript,
            }
            completed = subprocess.run(
                [str(helper)], input=json.dumps(native), text=True, capture_output=True,
                env=environment, cwd=root, timeout=30, check=True,
            )
            assert not completed.stdout, "child hooks must not request root title instructions"
            assert capture.exists(), "native child hooks must reach authenticated provider ingress"
            deliveries = [json.loads(line) for line in capture.read_text(encoding="utf-8").splitlines()]
            assert len(deliveries) == 2 and deliveries[0] == deliveries[1]
            event = deliveries[0]["event"]
            assert event["name"] == event_name
            for key in ("session_id", "agent_id", "agent_type", "cwd", "transcript_path"):
                assert event["body"][key] == native[key], f"native child {key} evidence changed"
            assert event["body"]["event_id"] == deliveries[0]["idempotency_key"]
            assert len(deliveries[0]["transition_nonce"]) == 32

        capture = root / "missing-directory.jsonl"
        environment["QWEN_TEST_HELPER_CAPTURE"] = str(capture)
        subprocess.run(
            [str(helper)], input=json.dumps({
                "hook_event_name": "SessionStart", "session_id": session, "source": "resume",
            }), text=True, capture_output=True, env=environment, cwd=root, timeout=30, check=True,
        )
        body = json.loads(capture.read_text(encoding="utf-8").splitlines()[0])["event"]["body"]
        assert "cwd" not in body and "transcript_path" not in body, "missing native facts cannot be inferred"
        del environment["QWEN_LOGONOMIC_PROFILE_HOME"]
        capture = root / "missing-profile.jsonl"
        environment["QWEN_TEST_HELPER_CAPTURE"] = str(capture)
        subprocess.run(
            [str(helper)], input=json.dumps({
                "hook_event_name": "SessionStart", "session_id": session, "source": "resume",
                "cwd": cwd, "transcript_path": transcript, "profile_home": str(root / "untrusted-hook-profile"),
            }), text=True, capture_output=True, env=environment, cwd=root, timeout=30, check=True,
        )
        body = json.loads(capture.read_text(encoding="utf-8").splitlines()[0])["event"]["body"]
        assert "profile_home" not in body, "native JSON cannot substitute for missing prepared profile scope"


if __name__ == "__main__":
    main()
