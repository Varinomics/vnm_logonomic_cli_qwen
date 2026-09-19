#!/usr/bin/env python3
"""Exercise a provider's real package target in an isolated source/build copy.

This is a recipe regression, NOT clean-runtime or native qualification. Run only
in an authorized build lane. Explicit resource mutations use checked scratch
paths. Configure/build execute trusted repository code, not a general sandbox.
The selected local dependency overrides are forwarded with fetch disabled.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def run(command: list[str]) -> None:
    print('+', subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, check=True, timeout=900)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'sdk', 'cmake', 'target', 'package-id', 'generator', 'validator', 'platform'):
        parser.add_argument('--' + name, required=True)
    for name in ('config', 'build-type', 'generator-platform', 'toolset', 'toolchain', 'compiler', 'make-program', 'prefix-path'):
        parser.add_argument('--' + name, default='')
    args = parser.parse_args()
    source, sdk = Path(args.source).resolve(), Path(args.sdk).resolve()
    require((source / 'package/assets').is_dir(), 'provider assets directory missing')
    require((sdk / 'include/logonomic_cli_addon/sdk.h').exists() or
            (sdk / 'include/logonomic_cli_addon/adapter_runtime.h').exists(), 'not a captured SDK source directory')
    with tempfile.TemporaryDirectory(prefix='cli-addon-membership-') as temp:
        root = Path(temp).resolve()
        # TMPDIR may itself point inside an input checkout. Refuse before
        # copying so the temporary source cannot recursively include itself.
        protected_inputs = [source, sdk]
        for protected in protected_inputs:
            require(root != protected and root not in protected.parents and
                    protected not in root.parents,
                    'scratch directory overlaps an input source')
        copied, build = root / 'source', root / 'build'
        # Ignore development outputs only at the checkout root. Applying these
        # patterns recursively would also omit real package/assets/build-* or
        # package/assets/out resources and test a different source membership.
        root_ignores = shutil.ignore_patterns(
            '.git', '.vs', '__pycache__', 'build', 'build-*', '_build', 'out', 'CMakeFiles')
        def ignore_root_outputs(directory: str, names: list[str]) -> set[str]:
            return root_ignores(directory, names) if Path(directory) == source else set()
        shutil.copytree(source, copied, symlinks=True, ignore=ignore_root_outputs)
        # Retaining a source symlink is not isolation: writing through a copied
        # package/assets symlink can modify the original checkout or another tree.
        # Refuse that fixture layout before any resource write, including dangling
        # reserved-file links (Path.exists alone does not detect those).
        package_source = copied / 'package'
        asset_root = package_source / 'assets'
        require(not package_source.is_symlink() and not asset_root.is_symlink(),
                'resource mutation requires real scratch directories, not symlinks')
        require(asset_root.is_dir() and copied.resolve() in asset_root.resolve().parents,
                'resource mutation path escapes scratch source')
        asset_a = asset_root / 'cli-addon-membership-a.txt'
        asset_b = asset_root / 'cli-addon-membership-b.txt'
        require(not any(path.exists() or path.is_symlink() for path in (asset_a, asset_b)),
                'reserved test resource already present')
        asset_a.write_bytes(b'isolated membership oracle\n')
        configure = [args.cmake, '-S', str(copied), '-B', str(build), '-G', args.generator,
                     '-DLOGONOMIC_CLI_ADDON_SDK_SOURCE=' + str(sdk),
                     '-DLOGONOMIC_CLI_ADDON_RECIPE_TESTS=OFF',
                     '-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
                     '-DFETCHCONTENT_UPDATES_DISCONNECTED=ON']
        if args.generator_platform:
            configure += ['-A', args.generator_platform]
        if args.toolset:
            configure += ['-T', args.toolset]
        for field, key in (('build_type', 'CMAKE_BUILD_TYPE'), ('toolchain', 'CMAKE_TOOLCHAIN_FILE'),
                           ('compiler', 'CMAKE_CXX_COMPILER'), ('make_program', 'CMAKE_MAKE_PROGRAM'),
                           ('prefix_path', 'CMAKE_PREFIX_PATH')):
            value = getattr(args, field)
            if value:
                configure.append('-D' + key + '=' + value)
        run(configure)
        command = [args.cmake, '--build', str(build), '--target', args.target]
        if args.config:
            command += ['--config', args.config]
        run(command)
        candidates = []
        for index in (build / 'package').rglob('package.files.json'):
            bootstrap = index.parent / 'logonomic-cli-addon.json'
            if bootstrap.is_file() and json.loads(bootstrap.read_text(encoding='utf-8'))['package_id'] == args.package_id:
                candidates.append(index.parent)
        require(len(candidates) == 1, 'expected exactly one built configuration package')
        package = candidates[0]
        def check(a: bool, b: bool) -> None:
            index = json.loads((package / 'package.files.json').read_text(encoding='utf-8'))
            for relative, expected in (('assets/cli-addon-membership-a.txt', a),
                                       ('assets/cli-addon-membership-b.txt', b)):
                payload, original = package / relative, package_source / relative
                entries = [entry for entry in index['files'] if entry['path'] == relative]
                require(len(entries) == int(expected), 'resource index membership mismatch')
                require(payload.exists() == expected and not payload.is_symlink(),
                        'stale, missing or symlinked payload resource')
                if expected:
                    expected_bytes = original.read_bytes()
                    require(payload.read_bytes() == expected_bytes,
                            'payload differs from the exact current source resource')
                    require(entries[0]['sha256'].lower() == hashlib.sha256(expected_bytes).hexdigest(),
                            'indexed digest differs from exact source and payload bytes')
            # The public validator checks index membership and content, not
            # just the existence of a freshly written index file.
            run([args.validator, str(package), '--platform', args.platform])
        def rebuild() -> None:
            run(command)  # Do not manually reconfigure: CONFIGURE_DEPENDS owns it.
        check(True, False)
        stamp = (package / 'package.files.json').stat().st_mtime_ns
        digest = hashlib.sha256((package / 'package.files.json').read_bytes()).hexdigest()
        rebuild()
        require((package / 'package.files.json').stat().st_mtime_ns == stamp, 'no-op rebuild rewrote package index')
        require(hashlib.sha256((package / 'package.files.json').read_bytes()).hexdigest() == digest, 'no-op rebuild changed index')
        # Separate filesystem timestamp ticks; this is not an asynchronous
        # safety assertion or a replacement for a synchronous product oracle.
        time.sleep(1.1)
        asset_a.write_bytes(b'exact replacement content oracle\n')
        rebuild()
        check(True, False)
        time.sleep(1.1)
        asset_a.unlink()
        rebuild()
        check(False, False)
        rebuild()
        check(False, False)
        time.sleep(1.1)
        asset_b.write_bytes(b'backdated membership oracle\n')
        os.utime(asset_b, (1, 1))
        rebuild()
        check(False, True)
        time.sleep(1.1)
        asset_b.rename(asset_a)
        rebuild()
        check(True, False)
    print('real package target: exact content, no-op, replacement, deletion, backdated addition and rename checked')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
