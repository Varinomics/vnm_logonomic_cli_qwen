# Qwen Code add-on for Logonomic

[![CI](https://github.com/Varinomics/vnm_logonomic_cli_qwen/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_logonomic_cli_qwen/actions/workflows/ci.yml)

Standalone C++20 add-on for Qwen Code on Windows/Linux x86-64 and macOS ARM64. It uses
the Logonomic CLI add-on SDK and the owned filesystem observation service,
with QtCore for native child observation. Qwen Code is installed separately;
this package does not install a CLI, log in, copy credentials, or call a paid API.

The integration uses Qwen's native command hooks, exact UUID session resume,
and Agent Client Protocol. Structured and delegated implementation sessions
use native ACP with the existing configured authentication and model. They do
not offer read-only review. The launcher options and user's Qwen configuration
determine permission mode. The package approves only this Task's own tools,
once per request, and rejects every other permission request once. It never
selects persistent approval or an approval that switches modes. A launcher
that needs file edits or commands must grant them itself. These operations
require `structured-session@3` and, for delegation,
`delegated-session@3`. ACP negotiates Qwen's active-work heartbeat before sending
the prompt, accepting the categories the native version reports. A root `end_turn`
keeps native stdin open while background children and the parent turns consuming
their results remain active. Only fresh, ordered root accounting permits a
native `qwen/control/session/close` request with `onlyIfUnheld` and `requireFlush`;
native close checks all work holds, including categories absent from the heartbeat.
Its acknowledgment establishes the final ownership boundary.
Validated root output is drained through clean process exit before the result is
released. Child output remains internal to the native delegation. Missing negotiation
or invalid accounting, timeout, failed exit and incomplete final records fail the operation.

A background service that never exits can retain a native shell hold indefinitely.
The task remains open until that work ends or the task is cancelled; the provider
does not impose a task-duration limit or force-close native work. A textless
intermediate automatic turn can be followed by successful typed parent synthesis.
If the final automatic turn has no typed completion, the task reports incomplete:
Qwen gives the same end marker to a successful textless turn and an error before
its first response.

`delegated-model-selection@1` replaces saved `--model`/`-m` options only for an
explicit task model. Native reasoning effort is applied per session through ACP
`session/set_config_option` without persistence metadata. The task waits for an
exact effort acknowledgment; unsupported or clamped values fail before its
prompt. The selected account, permission mode and saved defaults stay unchanged.
Without task settings, the saved model and native defaults remain effective.
An explicit model also sets Qwen's sole fallback to that same model, which its
retry loop skips, so configured alternate models cannot silently take over.
This requires native `--fallback-model` support. Model values with commas,
surrounding whitespace or an option-like leading hyphen are refused because
Qwen's option parser cannot preserve their exact fallback meaning.

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
notifications provide unpaired inferred attention.

Native `SubagentStart` hooks enroll children under the authenticated current root
activation. The adapter observes those children's exact metadata files through
the SDK's shared filesystem lifecycle driver and the owned filesystem observation
service. Only persisted `completed`, `failed`,
`cancelled`, or `paused` state closes an admitted child attempt; `SubagentStop`
alone does not, because that hook precedes Qwen's blockable completion gate.
Repeated helper delivery is idempotent, and a resumed child receives a distinct
attempt identity. An unchanged terminal sidecar from an earlier attempt cannot
prove that the newly started attempt has finished.

An unreadable or invalid child record leaves that child open while observation
continues for the root and other children. Detached interactive forks that do
not persist terminal metadata remain open until the root session ends. Set
`run_in_background: true` when starting a top-level interactive fork to use Qwen's
background path with persisted completion metadata.

Qwen publishes metadata on a best-effort basis. On Windows, rename-over can fail
while a reader has the preceding file open. The adapter closes its read handle
before parsing the bounded snapshot, but a publication concurrent with the read
can still fail. Qwen's task result and transcript remain available; without a
persisted terminal state, the child stays open until later native evidence or
the root session ends.

The shared driver retains correlation reservations, independent watch and poll
cursors, and filesystem subscriptions. It refuses new child admissions locally
at the host's 128-active-child bound or when its bounded queue cannot reserve
their terminal facts. Already admitted child stops and root SessionEnd retain
their queue space; capacity refusal does not disable root lifecycle tracking.

Observation requires the root hook's native transcript location. It does not
discover children by scanning other projects or replaying stored history.
Within one adapter runtime, only one live Logonomic lifecycle binding can observe
a native session metadata directory. A conflicting binding fails observation with
`subagent_session_already_bound`; Logonomic degrades the affected terminal's
whole lifecycle profile, including root turns, attention and child tracking.
This refusal does not stop the native CLI or change its task result or transcript.
Missing or unavailable evidence does not turn into a successful child completion.
Child tracking uses Logonomic's existing flat lifecycle under the root, and native
children retain the root's coordination principal. Qwen's native agent
configuration determines their tools and permissions.

In a live terminal, native root `SessionStart` hooks select the conversation after
`/clear` or a successful `/resume`. Logonomic retains the departed conversation in
session history; returning to it starts a new activation. Cancelling the native
picker or requesting a missing UUID emits no replacement activation. The selected
conversation's native `cwd` supplies the optional `execution-context@1` snapshot.
This is a snapshot at activation; later directory changes require another native
activation before the add-on can confirm the new default directory.

The optional `interactive-resume@3` and `@4` inspections use Qwen's native
project-scoped transcript store. Version 4 also reports whether validated history
contains conversation content. Missing or incomplete native evidence remains
unknown; only a validated transcript proves content or emptiness. Resume records
retain the native transcript path, activation directory and selected profile so
preparation can verify the same store. Existing ID-only records remain supported
by `interactive-resume@2`.

The optional `session-wake@1` capability uses Qwen's documented cross-session
peer protocol on Linux. A managed launch binds its selected profile; each wake
resolves the exact native session and verifies the registry's process start,
PID namespace and authenticated socket peer. A private per-launch environment
nonce proves that process belongs to the original managed launch, even if another
process later resumes the same native conversation. Inaccessible process binding
evidence makes wake unavailable. Windows reports native peer inbox
support unavailable. Missing or incompatible live peer records leave messages
queued in Logonomic until the session becomes active.

Notifications enter Qwen's peer input queue independently of the terminal
composer. They identify Logonomic and ask the agent to read its mailbox; message
bodies remain in that mailbox. The add-on presents only the ordinary peer token,
does not claim another agent's approval mode, and preserves the recipient's own
`agents.crossSessionInbound` policy. Its default policy may hold notifications
for review; an explicitly configured `accept` policy admits them. A native hold
counts as retained acceptance, with a `held:` receipt, to prevent automatic
resubmission while Qwen owns it. Retention does not prove a model turn or mailbox
read. A lost receipt after writing is uncertain and must not be retried
automatically. No controller grant or settings change is created by this capability.

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

Accounts identify the selected profile directory. Windows account keys follow
the native case rules of each containing directory. An older key shared by two
case-distinct profiles can resume access only with its matching saved path.
Accounts do not claim a vendor
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

On Linux, Qwen removes `..` from the profile path before opening its files.
If a previous add-in cached usage from a path containing `..`, the scan returns
`usage_cache_profile_reset_required`. Reset token usage to rebuild it from the
selected native profile.

The optional `usage-temporal@1` capability accompanies `usage-scan@3`. It
preserves admitted native UTC millisecond event timestamps, resolves original
UUID ownership before assigning portions, and uses the same model-wide total
or input/output/thoughts fallback as session accounting. A nonfork summary may
retain partial proved increments up to its exact total; excess transcript
evidence withdraws time proof without changing that total. Summary completion
dates never supply event instants. Missing, conflicting or capacity-limited
time evidence remains an exact unavailable amount.

Time metadata is an independently versioned optional sibling of each unchanged
accounting event in the single `qwen-evidence-cache@1` records file. Its canonical
digest binds the original event before replay and metadata rebinding. Damaged,
stale or unsupported optional data cannot invalidate accounting, and bounded
future carriers remain opaque. Original accounting has priority within source,
cache, snapshot, result and page budgets. Published proof binds the actual
account, record, total, snapshot and selected accepted manifest.

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

Structured preparation requires a direct Qwen launcher with plain
model/authentication/approval options. Resume also accepts the host's Windows
PowerShell command launcher when its script is a plain Qwen invocation with
those options. PowerShell resolves the selected Qwen file and evaluates its
literal argument expressions during resume; dynamic expressions and unrelated
commands are refused. A quoted executable path requires PowerShell's `&` call
operator. Windows script shims retain their selected script and run through
PowerShell; arguments that cannot safely survive a `.cmd` expansion are refused.

## Building and qualification

The [contributor guide](https://github.com/Varinomics/vnm_logonomic_cli_addon/blob/master/CONTRIBUTING.md) covers local package checks.
Send provider pull requests to [the qwen repository](https://github.com/Varinomics/vnm_logonomic_cli_qwen/pulls).

The owned SDK and filesystem observation source dependencies track `master`.
A local SDK override is available as `LOGONOMIC_CLI_ADDON_SDK_SOURCE`, pointing
at `sdk/logonomic_cli_addon_sdk`. `QWEN_OBSERVATION_SOURCE` selects a local
`vnm_filesystem_observation` checkout; the observer reuses an embedding build's existing
`vnm_framework::vnm_filesystem_observation` target when available. The observer uses Qt Core and `vnm_qt_dispatch`.
Build `qwen_package` in a separate build tree;
its indexed output is `package/logonomic.cli.qwen` below a single-configuration
build tree, or `package/<config>/logonomic.cli.qwen` with a multi-configuration
generator. The provider CI's preparation callback acquires the selected platform's
Qt kit, matching notices, and current owned sources before configuration.

The SDK's shared Qt recipe places `qwen_adapter`, `qwen_helper` and `qwen_launch_bridge` in
`adapter/` (with `.exe` on Windows). The producer resolves and indexes their
complete non-system runtime closure. Windows and Linux share the adapter
directory's libraries; macOS uses the SDK's native deployment
and signing helpers. A runnable build-tree adapter does not establish that its
package is deployable.

`QWEN_QTBASE_SOURCE` supplies the selected Qt version's source for license
material. `QWEN_RUNTIME_NOTICES_DIRECTORY` supplies external-runtime notices when
the host does not provide them. Linux additionally requires exact source-package
provenance: `QWEN_LINUX_RUNTIME_MATERIAL` supplies the SDK deployer's verified
prebuilt input record for Qt, ICU, or other acquired runtimes. Missing provenance
fails package assembly. These inputs do not relax integrity or loader-closure
checks.

Use `cmake --install` to export the indexed package to a fresh directory. See [SDK package export](https://github.com/Varinomics/vnm_logonomic_cli_addon/blob/master/sdk/logonomic_cli_addon_sdk/docs/package_contract.md#exporting-a-package).

The package includes focused contract, lifecycle observer, usage, ACP and native peer fixtures,
SDK package validation and package conformance registrations. The native peer
fixture uses isolated Linux sockets and profiles. For interoperability against
an installed `@qwen-code/sdk/peer`, run `node tests/qwen_peer_sdk_tests.mjs
<sdk-dist>/peer/index.js <build>/qwen_wake_tests`; it uses a disposable SDK
endpoint without a model API or user session. This proves the native wire and
receipt boundary, not idle-turn scheduling in the actual Qwen terminal UI.

Set `QWEN_NATIVE_TEST_COMMAND` to a CMake list containing an installed Qwen
command and its arguments to register the opt-in Linux x86-64 native tests.
They use isolated profiles and a loopback model without a model account or API
spend. Native ACP scenarios and evidence-retention instructions are documented in
[`tests/qwen_acp_native_tests.py`](tests/qwen_acp_native_tests.py).

## Source provenance

Behavior was traced against the official Qwen Code source at
`6a211b465227dd35ad3c35a2dcb86a809590bcd0`. This records the research input; it is
not an upstream compatibility pin. Primary contracts are in
`packages/core/src/hooks/{types,hookEventHandler}.ts`,
`packages/cli/src/config/{config,environment,settings,shared-env-keys}.ts`,
`packages/core/src/config/storage.ts`, the native ACP implementation, and
`packages/core/src/services/{chatRecordingService,usageHistoryService}.ts`.

Native child lifecycle observation also follows Qwen Code
[`f35dde84157f992bc324c0a3f59b1119344a683d`](https://github.com/QwenLM/qwen-code/tree/f35dde84157f992bc324c0a3f59b1119344a683d):
`packages/core/src/agents/agent-transcript.ts`, `background-agent-resume.ts`,
`packages/core/src/tools/agent/agent.ts`, and `packages/core/src/hooks/hookEventHandler.ts`.
The hook identity and durable metadata schema govern admission; this provenance
does not impose an upper CLI version limit.

The peer integration follows the public
[`cross-session-protocol.md`](https://github.com/QwenLM/qwen-code/blob/6212189f1f8334da78a69e3fb6b63ff6ca88251e/docs/users/features/cross-session-protocol.md)
contract, with that revision recorded as research provenance rather than a
CLI-version constraint. The selected live registry and protocol shape decide
availability.

The bundled dark and light Qwen icons use the user-supplied downloads from
[theSVG](https://thesvg.org/). The source catalog identifies Qwen's icons as
MIT-licensed; its license is retained in `package/notices/qwen-LICENSE.txt`.
`package/provenance/qwen_logo.json` records the source files, hashes and changes.
The dark icon's white fill is expanded from `#ffff` to `#ffffff` for Qt SVG
rendering. Browser layout styling and `1em` dimensions are normalized for the
host's SVG subset. Both variants preserve the downloaded path and viewBox.
