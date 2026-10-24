# Conversation extraction

The optional `conversation-export@1` capability extracts Qwen Code's retained
conversation into the host's request directory as UTF-8 Markdown, plain text or
the common structured conversation format.
The shared SDK provides the snapshot reader, renderer and result contract.

The source is the original bootstrapped Qwen home, checked against the selected
account when present. `QWEN_HOME`, `HOME` and `USERPROFILE` are source-location
environment names; the host preserves the resolved home from launch. Native
history lives at `projects/*/chats/<session-id>.jsonl`. Runtime directories,
usage summaries, artifact ledgers and nested subagent files are not substitute
transcripts. Every consumed record must name the requested session.

The last conversation record selects the active `parentUuid` chain. Rewind
records can therefore select an earlier ancestor without exporting discarded
branches. A fork's copied prefix is already present in its file; `forkedFrom`
is provenance and does not cause another parent-file traversal. Streaming
fragments with the same UUID accumulate their content parts into one message.
Conflicting parents, missing ancestors and cycles fail explicitly.

The provider renders text, function calls, function results, executable code
and image/file references from native `message.parts`. When paired prompt
display metadata is present, the original visible prompt replaces injected
model-facing text. Pairing can be explicit `hookContext` provenance or the
released `displayText`-only shape with a complete, non-nested final hook-context
part. Incomplete, nonfinal and single-part tag text is not pairing evidence.
Attachment/resource references remain in the export;
embedded binary data is described by media type instead of copied as base64.
System telemetry and artifact bookkeeping are not displayed as messages.
Agent sessions whose inherited context exists in an `agent_bootstrap` snapshot
are explicitly unavailable; their inherited prefix is not silently omitted.

Compact export keeps user content and final assistant replies. Explicit thinking,
tool content, assistant messages containing tool calls, and user-shaped records
with known nonhuman provenance are omitted. Missing or unknown provenance keeps
user content visible. A human-authored agent mention remains user content.

Qwen's `branch_checkpoint` records identify completed assistant answers and the
preceding turn boundary. Compact export uses a checkpoint only when its active
chain references are unique, its interval crosses no other checkpoint or new
user turn, and every tool call has a unique explicit ID matched by a result in
that interval. Internal Code Mode results do not participate in that matching.
The checkpoint must identify the sole visible assistant answer after the last
tool result. Earlier assistant messages in the proved interval are progress.
Malformed, conflicting or absent checkpoints, name-only tool matching, and
incomplete turns leave otherwise unclassified assistant content visible.
Unchecking Compact and exporting again preserves all extracted content and order.

The JSONL reader freezes the open file's length, exports complete records in
that prefix, and reports an unfinished final record. A malformed complete
record, unsupported message part or ambiguous source produces an error and no
completed output. Extraction works while the native CLI continues appending.

## Native schema and test oracle

The implementation follows Qwen Code's published definitions:

- [ChatRecord and native writer](https://github.com/QwenLM/qwen-code/blob/main/packages/core/src/services/chatRecordingService.ts)
- [Transcript leaf, UUID fragments and display projection](https://github.com/QwenLM/qwen-code/blob/main/packages/core/src/utils/transcript-records.ts)
- [Parent-chain reconstruction](https://github.com/QwenLM/qwen-code/blob/main/packages/core/src/utils/conversation-chain.ts)
- [Completed-turn checkpoints](https://github.com/QwenLM/qwen-code/blob/main/packages/core/src/services/branch-points.ts)
- [Representative native transcript fixture](https://github.com/QwenLM/qwen-code/blob/main/integration-tests/fixtures/chat-transcript-contract/v1/cases/representative/chat-records.jsonl)

These sources were inspected at `949a71e29b022c6ccad1c505a49680b3a7ce41b2`.
The revision identifies the oracle's provenance and is not a version gate.
`qwen_conversation_tests` checks fragments, rewind selection, inherited context,
prompt display metadata, tools, attachment references, Unicode, unfinished
tails and malformed/mismatched histories using schema-focused fixtures. It
does not claim to execute Qwen Code itself.
