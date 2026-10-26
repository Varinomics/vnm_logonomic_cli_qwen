#include "qwen_launcher_approval.h"

namespace qwen {

const logonomic::cli_addon::Launcher_approval_rules& launcher_approval_rules()
{
    static const logonomic::cli_addon::Launcher_approval_rules rules{
        .program_names = {"qwen", "qwen.exe", "qwen.cmd", "qwen.bat", "qwen.ps1"},
        .canonical_argument = "--yolo=true",
        .bypass_arguments = {"--yolo", "-y"},
        .approval_modes = {
            {"--approval-mode", {"yolo"}, {"plan", "default", "auto-edit", "auto"}},
        },
        .conflicting_arguments = {},
        .value_arguments = {
            "-m", "--model", "-p", "--prompt", "-i", "--prompt-interactive", "--output-format",
            "--input-format", "--system-prompt", "--append-system-prompt", "--output-style",
            "--openai-api-key",
            "--openai-base-url", "--max-session-turns", "--max-wall-time", "--max-tool-calls",
        },
        .optional_value_arguments = {"-r", "--resume"},
        .flag_arguments = {"-d", "--debug", "-c", "--continue", "--safe-mode", "-s", "--sandbox"},
        .session_commands = {},
        .unsupported_commands = {
            "mcp", "extensions", "extension", "plugins", "plugin", "auth", "login", "logout",
            "update", "upgrade", "init", "serve", "daemon", "agents", "skills", "models",
            "help", "version", "completion", "worktree", "session",
        },
        .value_arguments_consume_options = false,
        .flag_arguments_consume_booleans = true,
        .variadic_arguments = {
            {"--allowed-tools", false, false},
            {"--include-directories", false, false},
            {"--exclude-tools", false, false},
        },
        .inactive_bypass_argument = "--yolo=false",
    };
    return rules;
}

} // namespace qwen
