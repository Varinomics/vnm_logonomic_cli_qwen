"""Inspect the failing Windows launcher analysis without compiling or starting a CLI."""

import base64
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time


def quote_argument(argument):
    quoted = '"'
    slashes = 0
    for character in argument:
        if character == '\\':
            slashes += 1
            continue
        quoted += '\\' * (slashes * 2 + 1 if character == '"' else slashes) + character
        slashes = 0
    return quoted + '\\' * (slashes * 2) + '"'


source = Path('src/qwen_launcher.cpp').read_text(encoding='utf-8')
analysis = re.search(r'k_powershell_analysis = R"PS\((.*?)\)PS";', source, re.S).group(1)
for anchor, stage in (
    ('    $tokens = $null', 'parse'),
    ('    function LiteralValue($node)', 'parsed'),
    ('    $matches = @(Get-Command', 'resolve'),
    ('    if ($matches.Count', 'resolved'),
    ('    $json = ConvertTo-Json', 'serialize'),
):
    if anchor not in analysis:
        raise RuntimeError('The diagnostic anchor no longer matches the actual analysis.')
    analysis = analysis.replace(anchor, "    [Console]::Out.WriteLine('QWEN_STAGE:" + stage + "')\n" + anchor)
analysis = "[Console]::Out.WriteLine('QWEN_STAGE:started')\n" + analysis
powershell = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
with tempfile.TemporaryDirectory(prefix='qwen-launcher-diagnostic-') as scratch:
    root = Path(scratch).resolve()
    fake = root / 'qwen.exe'
    shutil.copy2(Path(os.environ['SystemRoot']) / 'System32/cmd.exe', fake)
    preparation = "qwen --model 'selected-model' --approval-mode 'default'"
    initial = "& '" + str(fake).replace("'", "''") + "' --model 'chosen-model' --approval-mode ('plan')"
    commands = (
        ('simple startup', "[Console]::Out.WriteLine('QWEN_STARTUP_READY')", (), ()),
        ('simple startup without profiles', "[Console]::Out.WriteLine('QWEN_STARTUP_READY')", (), ('-NoProfile',)),
        ('preparation', "$SourceBase64='" + base64.b64encode(preparation.encode()).decode() + "'\n" + analysis, (), ()),
        ('initial prompt', "$SourceBase64='" + base64.b64encode(initial.encode()).decode() + "'\n" + analysis,
         ('TEMP', 'TMP'), ()),
    )
    for name, command, extra_environment, extra_options in commands:
        environment = {key: os.environ[key] for key in
                       ('SystemRoot', 'SystemDrive', 'PATHEXT', 'ComSpec', *extra_environment)
                       if key in os.environ}
        environment['PATH'] = str(root) + ';' + os.environ['PATH']
        environment['QWEN_HOME'] = str(root)
        arguments = ' '.join(quote_argument(value) for value in
                             (str(powershell), '-NoLogo', '-NonInteractive', *extra_options, '-Command', command))
        started = time.monotonic()
        try:
            result = subprocess.run(arguments, executable=powershell, stdin=subprocess.DEVNULL,
                                    cwd=root, env=environment, capture_output=True, text=True,
                                    errors='replace', timeout=10,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            record = {'case': name, 'exit': result.returncode,
                      'stdout': result.stdout, 'stderr': result.stderr}
        except subprocess.TimeoutExpired as error:
            record = {'case': name, 'timeout': True,
                      'stdout': str(error.stdout), 'stderr': str(error.stderr)}
        record['elapsed_seconds'] = round(time.monotonic() - started, 3)
        print(json.dumps(record), flush=True)
