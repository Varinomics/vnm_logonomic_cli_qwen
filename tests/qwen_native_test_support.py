"""Mechanical helpers shared by the opt-in, account-free native fixtures."""

import json
import os


def native_environment(root, socket_root):
    home = root / "home"
    return {
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LANG": "C.UTF-8", "TERM": "xterm-256color",
        "HOME": str(home), "USERPROFILE": str(home), "TMPDIR": str(root),
        "XDG_CONFIG_HOME": str(home / "config"), "XDG_DATA_HOME": str(home / "data"),
        "XDG_CACHE_HOME": str(home / "cache"), "XDG_STATE_HOME": str(home / "state"),
        "XDG_RUNTIME_DIR": str(socket_root),
        "QWEN_HOME": str(root / "profile"), "QWEN_RUNTIME_DIR": str(root / "runtime"),
        "QWEN_CODE_SYSTEM_SETTINGS_PATH": str(root / "system.json"),
        "QWEN_CODE_SYSTEM_DEFAULTS_PATH": str(root / "system.json"),
        "QWEN_LOGONOMIC_PROFILE_HOME": str(root / "profile"),
        "HTTP_PROXY": "http://127.0.0.1:1", "HTTPS_PROXY": "http://127.0.0.1:1",
        "ALL_PROXY": "http://127.0.0.1:1", "NO_PROXY": "127.0.0.1,localhost", "CI": "true",
    }


def send_model_response(handler, request, message, completion_id="fixture-completion"):
    """Frame one scripted OpenAI reply; native side queries may be non-streaming."""
    handler.send_response(200)
    handler.send_header("Content-Type", "text/event-stream" if request.get("stream") else "application/json")
    handler.send_header("Connection", "close")
    handler.end_headers()
    finish = "tool_calls" if message.get("tool_calls") else "stop"
    usage = {"prompt_tokens": 7, "completion_tokens": 4, "total_tokens": 11}
    if request.get("stream"):
        for delta, reason in ((message, None), ({}, finish)):
            response = {"id": completion_id, "object": "chat.completion.chunk", "created": 1,
                        "model": "fixture-model", "choices": [{"index": 0, "delta": delta, "finish_reason": reason}]}
            if reason:
                response["usage"] = usage
            handler.wfile.write(("data: " + json.dumps(response) + "\n\n").encode())
            handler.wfile.flush()
        handler.wfile.write(b"data: [DONE]\n\n")
    else:
        response = {"id": completion_id, "object": "chat.completion", "created": 1, "model": "fixture-model",
                    "choices": [{"index": 0, "message": message, "finish_reason": finish}], "usage": usage}
        handler.wfile.write(json.dumps(response).encode())
    handler.wfile.flush()
