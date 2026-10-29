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
powershell = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
with tempfile.TemporaryDirectory(prefix='qwen-launcher-diagnostic-') as scratch:
    root = Path(scratch).resolve()
    fake = root / 'qwen.exe'
    shutil.copy2(Path(os.environ['SystemRoot']) / 'System32/cmd.exe', fake)
    for name, body, extra_environment in (
        ('preparation', "qwen --model 'selected-model' --approval-mode 'default'", ()),
        ('initial prompt', "& '" + str(fake).replace("'", "''") +
         "' --model 'chosen-model' --approval-mode ('plan')", ('TEMP', 'TMP')),
    ):
        environment = {key: os.environ[key] for key in
                       ('SystemRoot', 'SystemDrive', 'PATHEXT', 'ComSpec', *extra_environment)
                       if key in os.environ}
        environment['PATH'] = str(root) + ';' + os.environ['PATH']
        environment['QWEN_HOME'] = str(root)
        command = "$SourceBase64='" + base64.b64encode(body.encode()).decode() + "'\n" + analysis
        arguments = ' '.join(quote_argument(value) for value in
                             (str(powershell), '-NoLogo', '-NonInteractive', '-Command', command))
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
