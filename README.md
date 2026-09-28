# Qwen Code add-on for Logonomic

Standalone C++20 add-on for Qwen Code on Windows and Linux x86-64. It consumes
only the public Logonomic CLI add-on SDK. Qwen Code is installed separately;
this package does not install a CLI, log in, copy credentials, or call a paid API.

The integration uses Qwen's native command hooks, exact UUID session resume,
and Agent Client Protocol. Structured and delegated implementation sessions
use native ACP with the existing configured authentication and model. They do
not offer read-only review: the implementation protocol selects Qwen's `yolo`
mode, which runs tools without confirmation; a permission request fails the
task. These operations require `structured-session@3` and, for delegation,
`delegated-session@3`. An authenticated prompt response requests native stdin
EOF; validated advisory output is drained through clean process exit before the
result is released. Timeout, failed exit and incomplete final records fail the
operation.

Delegated preparation configures each Task server in ACP `session/new` as the
host relay with `--descriptor`, `--server`, a catalog receipt path and its
nonce, and no environment, and acknowledges connection version 1; a request
without a connection is refused as `task_server_connection_unavailable`. The
relay writes the nonce to the receipt only after the Task server's catalog,
which it limits to the required actions, succeeded. Qwen otherwise discovers MCP
servers in the background after it answers `session/new`, and its ACP prompt
does not wait for them, so delegated runs set Qwen's
`QWEN_CODE_LEGACY_MCP_BLOCKING=1`: discovery of every configured server then
completes or times out before the reply. At that reply every receipt must hold
its nonce, or the task fails as `task_servers_not_ready` before the prompt is
sent. Qwen's ACP reports no per-session MCP tool list, so the receipt is the
evidence that the required actions reached Qwen.

Proper host delegated-task support is ABSOLUTELY MANDATORY, not optional. The
package MUST implement and validate end-to-end host delegated tasks, including
required control tools and action admission, actual CLI integration,
authentication, host process custody, and cleanup. Capability declarations,
mode acknowledgments, and blanket refusal do not constitute completion. Any
established upstream limitation MUST be reported with concrete evidence; it
does not silently waive the product requirement.

Interactive hooks deliver host-admitted title and coordination instructions as
Qwen `hookSpecificOutput.additionalContext`. Prompt submission and Stop provide inferred phase
evidence; Stop does not prove success because another native hook can resume
the turn. StopFailure confirms failure of an identified root turn. Permission
notifications provide unpaired inferred attention. Child lifecycle tracking is
not advertised: Qwen's child stop hook runs before its blockable completion gate.

The optional `attention-kind@1` companion labels native `permission_prompt`
notifications as `permission` and `elicitation_dialog` notifications as
`interactive_question`. Both remain inferred and unpaired: native session and
turn identities are preserved, the helper's delivery ID is not a question request ID, and neither signal
proves an answer or cancellation. Base helper-ingress results are unchanged.
The companion does not classify notification prose or Stop output as a
conversational hint. Host presentation must retain the kind of unpaired
attention to display this distinction.

Qwen's user settings, credentials and runtime history retain their native
locations. The shared host resolves Qwen's user `.env` bootstrap before
selecting the profile. Managed hook installation transforms a host-granted
`settings.json` snapshot; the host owns the validated atomic write. Existing
settings and hook comments survive, and modified same-name managed definitions
or an explicit `disableAllHooks` are refused rather than overwritten.

Accounts identify the selected profile directory. They do not claim a vendor
user identity or enumerate hypothetical alternate profiles. Static credential
evidence cannot prove an authenticated server session, so no standalone auth
status capability is advertised. Native ACP reports actual authentication
failures. No quota capability is advertised because the inspected vendor
sources provide no suitable durable quota observation.

Usage reads `usage_record.jsonl` summaries and `ui_telemetry` from the selected
global profile's `projects/*/chats/*.jsonl`. Runtime-directory overrides do not
relocate that native history-service scan. Clean last-wins summaries own totals
for sessions without observed fork provenance. Torn or recovered summaries cannot
replace accepted same-session evidence. The package retains telemetry by UUID,
including observations missing after truncation or deletion; an observed counter
correction can reduce its amount. Input, output and cache-read observations retain
their coverage; cache creation is unsupported.

Forked telemetry keeps its original UUID even though Qwen rewrites prompt IDs.
The package resolves retained copy links before summing, counts copied spend once,
and preserves new calls in each fork under stable session keys. Available
transcripts are inspected even when summaries exist. A known fork's gross native
summary must match its retained telemetry to establish the partition; missing
partition evidence or contradictory UUID observations fail the scan and leave
accepted usage unchanged. If ancestry is recovered later, the affected session
amounts are corrected together. A bounded atomic cache in host-acknowledged profile
storage preserves the evidence and identities across restarts and source deletion.
Native history remains unchanged.

Native summaries contain no UUIDs or fork markers. A fork whose transcript was
deleted before the package observed any copy provenance cannot be distinguished
from independent spend using those summaries alone. Such markerless summaries
retain native session accounting; the package cannot promise fork deduplication
for that unavailable historical provenance. Missing retained sources are reported
as incomplete historical coverage, not as permission to erase accepted evidence.

Profile-dependent operations require the host's `environment-bootstrap@1`
proof, and managed settings require `managed-configuration@2`. Missing host
support produces an unavailable operation instead of guessing a profile.

Install Qwen Code using the current official [installation instructions](https://github.com/QwenLM/qwen-code).
The standalone installers support Windows and Linux without an external Node
installation. The npm route uses `@qwen-code/qwen-code` and the Node requirement
declared by its current package. The adapter probes required command options at
runtime; it does not impose a maximum CLI version or pin a model.

Structured and resume preparation require a direct Qwen launcher with plain
model/authentication/approval options. Shell scripts with unrelated commands
are not reconstructed into structured executions. Windows script shims retain
their selected script and run through PowerShell; arguments that cannot safely
survive a `.cmd` expansion are refused.

## Building and qualification

The owned SDK source dependency tracks `master`. A local source override is
available as `LOGONOMIC_CLI_ADDON_SDK_SOURCE`, pointing at
`sdk/logonomic_cli_addon_sdk`. Build `qwen_package` in a separate build tree;
its indexed output is `package/logonomic.cli.qwen` below a single-configuration
build tree, or `package/<config>/logonomic.cli.qwen` with a multi-configuration
generator. Windows
compiler-invoking commands must use the user-wide queued-build policy.

The package recipe proves the Windows/Linux x86-64 compiler target during
configuration. `cmake --install` exports a fresh artifact beneath its install-time
prefix and preserves executable permissions. An existing per-package destination,
including a dangling symlink, is refused. POSIX `DESTDIR` staging is supported;
nonempty `DESTDIR` with a drive-letter prefix is refused. Export is not atomic:
a failure may leave a partial artifact directory. Choose a fresh destination or
manually remove only that identified disposable export before retrying; never
remove an installed generation this way. The application's package ingestion
owns atomic installation and updates.

The package includes focused contract, usage and ACP fixtures, SDK package
validation and package conformance registrations. They are authored but have
not been built or run during this source preparation. Qualification and live
activation belong to the later combined integration pass.

## Source provenance

Behavior was traced against the official Qwen Code source at
`6a211b465227dd35ad3c35a2dcb86a809590bcd0`. This records the research input; it is
not an upstream compatibility pin. Primary contracts are in
`packages/core/src/hooks/{types,hookEventHandler}.ts`,
`packages/cli/src/config/{config,environment,settings,shared-env-keys}.ts`,
`packages/core/src/config/storage.ts`, the native ACP implementation, and
`packages/core/src/services/{chatRecordingService,usageHistoryService}.ts`.

The bundled Qwen logo comes from
`packages/desktop-shell/bootstrap/qwen-code-logo.svg` in that source. Its
upstream Apache-2.0 license is retained in `package/notices/qwen-LICENSE.txt`;
`package/provenance/qwen_logo.json` records its indexed source attribution.
The upstream desktop shell's NOTICE is retained unchanged in
`package/notices/qwen-NOTICE.txt`. Its Node.js distribution statement describes
that upstream shell; the Logonomic add-on does not bundle the Node.js runtime.
