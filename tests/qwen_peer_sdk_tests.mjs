// Run against an independently installed @qwen-code/sdk/peer and the built
// qwen_wake_tests. The endpoint uses a disposable profile and no model API.
import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { spawn } from 'node:child_process';
import { pathToFileURL } from 'node:url';

const [sdk_path, executable] = process.argv.slice(2);
assert(sdk_path && executable, 'Pass the installed peer/index.js path and built qwen_wake_tests');
const fixture_binding = 'bfc0b538f38596aef4ac803b9f182cde';
if (process.env.QWEN_LOGONOMIC_WAKE_BINDING !== fixture_binding) {
    const child = spawn(process.execPath, process.argv.slice(1), {
        env: { ...process.env, QWEN_LOGONOMIC_WAKE_BINDING: fixture_binding },
        stdio: 'inherit',
    });
    process.exitCode = await new Promise((resolve_exit, reject) => {
        child.once('error', reject);
        child.once('exit', (code) => resolve_exit(code ?? 1));
    });
}
else {
    const { PeerEndpoint } = await import(pathToFileURL(resolve(sdk_path)).href);
    const profile = await mkdtemp(join(tmpdir(), 'logonomic-qwen-sdk-'));
    const session = randomUUID();
    let received = 0;
    const peer = await PeerEndpoint.start({
        name: 'Logonomic native protocol fixture',
        qwenHome: profile,
        sessionId: session,
        socketPath: join(profile, 'peer.sock'),
        onMessage(message) {
            assert.equal(message.content, '/shell @file\nMailbox ready');
            assert.equal(message.fromName, 'Logonomic mailbox');
            assert.equal(message.fromMode, undefined);
            received += 1;
        },
    });
    try {
        const child = spawn(executable, ['--native-fixture', profile, session], { stdio: 'inherit' });
        const code = await new Promise((resolve_exit, reject) => {
            child.once('error', reject);
            child.once('exit', resolve_exit);
        });
        assert.equal(code, 0);
        assert.equal(received, 1);
        console.log('Native Qwen peer SDK: exact-session authenticated delivery and receipt passed');
    }
    finally {
        await peer.close();
        await rm(profile, { recursive: true, force: true });
    }
}
