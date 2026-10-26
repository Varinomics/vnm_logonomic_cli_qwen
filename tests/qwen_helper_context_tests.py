"""Exercise native hook input through the packaged helper and a lost response."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    package = Path(sys.argv[1]).resolve()
    transport = Path(sys.argv[2]).resolve()
    helper = package / "helpers" / ("qwen_helper.exe" if os.name == "nt" else "qwen_helper")
    # Native hookEventHandler creates these fields once per hook invocation.
    # The helper owns transport retries; Qwen supplies no hook replay identity.
    common = {
        "session_id": "12345678-1234-4234-8234-123456789abc",
        "prompt_id": "12345678-1234-4234-8234-123456789abc########7",
        "timestamp": "2026-10-26T00:30:00.000Z",
        "cwd": str(package),
        "transcript_path": "unused-native-transcript.jsonl",
    }
    events = [
        dict(common, hook_event_name="Stop", input_tokens=32000, context_limit=128000,
             context_usage=0.25, stop_hook_active=False),
        dict(common, hook_event_name="SessionStart", source="compact", model="qwen3-coder"),
    ]
    with tempfile.TemporaryDirectory(prefix="qwen-helper-context-") as scratch:
        environment = dict(os.environ)
        environment.update({
            "LOGONOMIC_VNMCTL_PATH": str(transport),
            "LOGONOMIC_SESSION_CAPABILITY": "c" * 64,
            "LOGONOMIC_PROVIDER_PACKAGE_ID": "logonomic.cli.qwen",
            "LOGONOMIC_PROVIDER_GENERATION_DIGEST": "d" * 64,
            "LOGONOMIC_PROVIDER_GENERATION_NONCE": "e" * 32,
            "LOGONOMIC_PROVIDER_HELPERS_DIR": str(helper.parent),
        })
        identities = set()
        for index, event in enumerate(events):
            capture = Path(scratch) / f"delivery-{index}.jsonl"
            environment["QWEN_TEST_HELPER_CAPTURE"] = str(capture)
            result = subprocess.run(
                [str(helper)], input=json.dumps(event), text=True, capture_output=True,
                env=environment, cwd=scratch, timeout=30, check=False,
            )
            if result.returncode != 0:
                raise RuntimeError(f"helper failed: {result.returncode}: {result.stderr}")
            deliveries = [json.loads(line) for line in capture.read_text(encoding="utf-8").splitlines()]
            if len(deliveries) != 2 or deliveries[0] != deliveries[1]:
                raise RuntimeError("a lost response must retry the identical event, key and transition nonce")
            delivery = deliveries[0]
            body = delivery["event"]["body"]
            if body["event_id"] != delivery["idempotency_key"] or not body["event_id"]:
                raise RuntimeError("one helper invocation must retain its event identity across transport retry")
            identities.add(body["event_id"])
            if body["session_id"] != common["session_id"] or body["prompt_id"] != common["prompt_id"]:
                raise RuntimeError("native session and prompt identity did not reach the adapter event")
            if event["hook_event_name"] == "Stop":
                if body.get("input_tokens") != 32000 or body.get("context_limit") != 128000:
                    raise RuntimeError("native current occupancy did not reach the adapter event")
            elif body.get("source") != "compact":
                raise RuntimeError("successful native compaction source did not reach the adapter event")
        if len(identities) != len(events):
            raise RuntimeError("distinct native hook invocations must own distinct delivery identities")
    print("Packaged Qwen helper preserves native context and retry identity.")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
