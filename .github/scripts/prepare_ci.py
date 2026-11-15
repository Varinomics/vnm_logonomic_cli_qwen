"""Prepare Qwen's QtCore and filesystem-observation inputs for provider CI.

Development checks out the owned dependencies at master. A release passes the
exact revisions it builds from with --revisions, and its selected aqtinstall
with --aqt-source; --record keeps what this preparation actually used.
"""

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request


QT_VERSION = "6.12.0"
# The compilers each platform builds with, and the Qt kit whose ABI each needs.
# Clang and GCC share the Linux kit's libstdc++ ABI.
QT_KITS = {
    "windows-x86_64": {"msvc": ("windows", "win64_msvc2022_64"),
                       "llvm-mingw": ("windows", "win64_llvm_mingw")},
    "linux-x86_64": {"gcc": ("linux", "linux_gcc_64"), "clang": ("linux", "linux_gcc_64")},
    "macos-arm64": {"clang": ("mac", "clang_64")},
}
DEFAULT_TOOLCHAINS = {"windows-x86_64": "msvc", "linux-x86_64": "gcc", "macos-arm64": "clang"}
OWNED_DEPENDENCIES = ("vnm_framework", "vnm_qt_dispatch", "vnm_cmake")
# Development follows aqtinstall's master: its upstream metadata fix is needed for
# Qt's per-architecture Windows repositories.
DEVELOPMENT_AQT = "git+https://github.com/miurahr/aqtinstall.git@master"
LLVM_RUNTIME_NOTICES = ("libc++.dll.txt", "libunwind.dll.txt")


def run(arguments, **options):
    subprocess.run(arguments, check=True, **options)


def qt_kit(platform, toolchain):
    kits = QT_KITS[platform]
    if toolchain not in kits:
        raise ValueError(f"The {toolchain} toolchain does not build {platform}; use one of {sorted(kits)}")
    return kits[toolchain]


def read_revisions(path):
    """A release's source revisions; Qwen takes its own owned dependencies from them."""
    selected = json.loads(path.read_text(encoding="utf-8"))
    revisions = {}
    for name in OWNED_DEPENDENCIES:
        revision = selected.get(name) if isinstance(selected, dict) else None
        if not isinstance(revision, str) or not re.fullmatch(r"[0-9a-f]{40}", revision):
            raise ValueError(f"The release revisions must name {name} at an exact commit")
        revisions[name] = revision
    return revisions


def dependency_sources(root, token, revisions):
    environment = os.environ.copy()
    config_index = int(environment.get("GIT_CONFIG_COUNT", "0"))
    authorization = base64.b64encode(("x-access-token:" + token).encode()).decode()
    environment["GIT_CONFIG_COUNT"] = str(config_index + 1)
    environment[f"GIT_CONFIG_KEY_{config_index}"] = "http.https://github.com/.extraheader"
    environment[f"GIT_CONFIG_VALUE_{config_index}"] = "AUTHORIZATION: basic " + authorization
    sources = {}
    for repository in OWNED_DEPENDENCIES:
        source = root / repository
        requested = revisions[repository] if revisions else "master"
        if not source.exists():
            run(["git", "init", "--quiet", str(source)])
            run(["git", "-C", str(source), "remote", "add", "origin",
                 f"https://github.com/Varinomics/{repository}.git"])
        run(["git", "-C", str(source), "fetch", "--depth", "1", "origin", requested], env=environment)
        run(["git", "-C", str(source), "checkout", "--quiet", "--detach", "FETCH_HEAD"])
        revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
        if revisions and revision != requested:
            raise RuntimeError(f"{repository} checked out {revision}, not the requested {requested}")
        sources[repository] = {"path": source, "repository": f"Varinomics/{repository}",
                               "requested": requested, "revision": revision}
    return sources


def qt_prefix(root):
    candidates = list((root / QT_VERSION).glob("*/lib/cmake/Qt6Core/Qt6CoreConfig.cmake"))
    if len(candidates) != 1:
        raise RuntimeError("Expected one installed QtCore kit")
    return candidates[0].parents[3]


def prepare_qt(root, platform, toolchain, aqt_source):
    host, architecture = qt_kit(platform, toolchain)
    # Each kit has its own root and marker, so a restored kit of another ABI
    # can never stand in for the selected one.
    qt_root = root / f"qt-{architecture}"
    marker = root / f"qt-{QT_VERSION}-{architecture}.ready"
    archives = ["qtbase"]
    if platform == "linux-x86_64":
        archives.append("icu")
    elif architecture == "win64_llvm_mingw":
        # The kit's own LLVM runtime archive installs libc++.dll and libunwind.dll
        # into its bin, the only directory the package's Windows closure searches.
        archives.append("llvm")
    # The marker names what installed the kit, so --record can state it and a
    # restored kit from another aqtinstall source or archive set is replaced.
    installation = {"aqt_source": aqt_source, "archives": archives}
    if not marker.is_file() or marker.read_text(encoding="utf-8") != json.dumps(installation):
        if qt_root.exists():
            shutil.rmtree(qt_root)
        tools = root / "aqt"
        run([sys.executable, "-m", "pip", "install", "--upgrade", "--target", str(tools),
             "--cache-dir", str(root / "pip-cache"), aqt_source])
        environment = os.environ.copy()
        environment["PYTHONPATH"] = str(tools)
        run([sys.executable, "-m", "aqt", "install-qt", host, "desktop", QT_VERSION,
             architecture, "--outputdir", str(qt_root), "--archives", *archives], env=environment)
        qt_prefix(qt_root)
        marker.write_text(json.dumps(installation), encoding="utf-8")
    prefix = qt_prefix(qt_root)
    notices = root / f"qtbase-{QT_VERSION}"
    if not notices.exists():
        run(["git", "clone", "--depth", "1", "--filter=blob:none", "--sparse",
             "--branch", f"v{QT_VERSION}", "https://github.com/qt/qtbase.git", str(notices)])
    run(["git", "-C", str(notices), "sparse-checkout", "set", "LICENSES", "src/3rdparty"])
    return prefix, notices


def prepare_llvm_runtime_notices(toolchain_root, destination):
    """The package ships the LLVM C++ runtime DLLs; their notice is the supplier's LLVM
    licence and the selected x86_64 MinGW-w64 runtime terms, joined by two LF bytes."""
    licence = (toolchain_root / "LICENSE.TXT").read_bytes()
    runtime = (toolchain_root / "x86_64-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt").read_bytes()
    destination.mkdir(parents=True, exist_ok=True)
    for name in LLVM_RUNTIME_NOTICES:
        (destination / name).write_bytes(licence + b"\n\n" + runtime)


def prepare_icu_notices(prefix, destination):
    destination.mkdir(parents=True, exist_ok=True)
    licenses = {}
    inputs = {}
    for runtime in sorted((prefix / "lib").glob("libicu*.so*")):
        match = re.fullmatch(r"libicu\w+\.so\.(\d+)\.(\d+)", runtime.resolve().name)
        if not match:
            raise RuntimeError(f"Cannot identify ICU release for {runtime.name}")
        version = "-".join(match.groups())
        if version not in licenses:
            notice = destination / f"icu-{version}-LICENSE"
            if not notice.is_file():
                url = f"https://raw.githubusercontent.com/unicode-org/icu/release-{version}/icu4c/LICENSE"
                with urllib.request.urlopen(url, timeout=60) as response:
                    notice.write_bytes(response.read())
            licenses[version] = notice.read_bytes()
        (destination / (runtime.name + ".txt")).write_bytes(licenses[version])
        real_runtime = runtime.resolve(strict=True)
        inputs[real_runtime] = (version.replace("-", "."), destination / (runtime.name + ".txt"))
    return inputs


def prepare_linux_runtime_material(prefix, qt_notices, icu_inputs, destination):
    # This route installs qtbase and ICU from Qt's selected online-installer
    # kit. Record those acquisition inputs explicitly, before package copying.
    qt_core = (prefix / "lib/libQt6Core.so.6").resolve(strict=True)
    selected = [(qt_core, "qtbase", QT_VERSION,
                 f"https://code.qt.io/cgit/qt/qtbase.git/tag/?h=v{QT_VERSION}",
                 qt_notices / "LICENSES/LGPL-3.0-only.txt")]
    for runtime, (version, notice) in sorted(icu_inputs.items()):
        release = version.replace(".", "-")
        selected.append((runtime, "icu", version,
                         f"https://github.com/unicode-org/icu/tree/release-{release}", notice))
    rows = []
    for runtime, archive, version, source_url, notice in selected:
        rows.append({"source": str(runtime), "source_sha256": hashlib.sha256(runtime.read_bytes()).hexdigest(),
                     "binary_package": f"Qt online installer {QT_VERSION} linux_gcc_64 {archive}",
                     "binary_version": version, "source_package": archive, "source_version": version,
                     "source_url": source_url, "notice": str(notice.resolve(strict=True)),
                     "notice_sha256": hashlib.sha256(notice.read_bytes()).hexdigest(),
                     "acquisition": {"provider": "qt-online-installer", "host": "linux", "target": "desktop",
                                     "version": QT_VERSION, "architecture": "linux_gcc_64", "archive": archive}})
    destination.write_text(json.dumps({"schema_version": 1, "inputs": rows}, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=QT_KITS, required=True)
    parser.add_argument("--toolchain", choices=sorted({name for kits in QT_KITS.values() for name in kits}),
                        help="Compiler family of this build; defaults to the platform's retail compiler")
    parser.add_argument("--toolchain-root", type=Path, help="LLVM-MinGW installation, for --toolchain llvm-mingw")
    parser.add_argument("--aqt-source", default=DEVELOPMENT_AQT, help="pip requirement that installs aqtinstall")
    parser.add_argument("--revisions", type=Path, help="JSON object of the release's exact source revisions")
    parser.add_argument("--record", type=Path, help="Write the toolchain, Qt kit and revisions this used")
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    toolchain = args.toolchain or DEFAULT_TOOLCHAINS[args.platform]
    _, architecture = qt_kit(args.platform, toolchain)
    if (toolchain == "llvm-mingw") != (args.toolchain_root is not None):
        raise ValueError("--toolchain-root names the LLVM-MinGW installation of --toolchain llvm-mingw only")
    revisions = read_revisions(args.revisions) if args.revisions else None
    token = os.environ.pop("VARINOMICS_REPO_READ_TOKEN", "")
    if not token:
        raise RuntimeError("VARINOMICS_REPO_READ_TOKEN is required to read Qwen's owned dependencies")
    root = args.workspace.resolve() / ".ci-dependencies" / "qwen"
    root.mkdir(parents=True, exist_ok=True)
    sources = dependency_sources(root, token, revisions)
    prefix, qt_notices = prepare_qt(root, args.platform, toolchain, args.aqt_source)
    cmake_args = [
        f"-DCMAKE_PREFIX_PATH={prefix.as_posix()}",
        f"-DQWEN_FRAMEWORK_SOURCE={sources['vnm_framework']['path'].as_posix()}",
        f"-DFETCHCONTENT_SOURCE_DIR_VNM_QT_DISPATCH={sources['vnm_qt_dispatch']['path'].as_posix()}",
        f"-DFETCHCONTENT_SOURCE_DIR_VNM_CMAKE={sources['vnm_cmake']['path'].as_posix()}",
        f"-DQWEN_QTBASE_SOURCE={qt_notices.as_posix()}",
        "-DVNM_FILESYSTEM_OBSERVATION_BUILD_TESTS=OFF",
        "-DVNM_QT_DISPATCH_BUILD_TESTING=OFF",
    ]
    if toolchain == "llvm-mingw":
        runtime_notices = root / "llvm-mingw-runtime-notices"
        prepare_llvm_runtime_notices(args.toolchain_root.resolve(strict=True), runtime_notices)
        cmake_args.append(f"-DQWEN_RUNTIME_NOTICES_DIRECTORY={runtime_notices.as_posix()}")
        # Provider arguments follow the shared driver's generic Free defaults.
        # Only this Qt-aware recipe ships the LLVM DLLs beside its executables;
        # older Qwen revisions without this callback retain their static link.
        cmake_args.append("-DCMAKE_EXE_LINKER_FLAGS_RELEASE=-Wl,--strip-all")
    if args.platform == "linux-x86_64":
        runtime_notices = root / "runtime-notices"
        icu_inputs = prepare_icu_notices(prefix, runtime_notices)
        material = root / "linux-runtime-material.json"
        prepare_linux_runtime_material(prefix, qt_notices, icu_inputs, material)
        cmake_args.append(f"-DQWEN_RUNTIME_NOTICES_DIRECTORY={runtime_notices.as_posix()}")
        cmake_args.append(f"-DQWEN_LINUX_RUNTIME_MATERIAL={material.as_posix()}")
    with open(os.environ["GITHUB_PATH"], "a", encoding="utf-8") as output:
        output.write(str(prefix / "bin") + "\n")
    args.output.write_text(json.dumps(cmake_args) + "\n", encoding="utf-8")
    if args.record:
        args.record.write_text(json.dumps({
            "schema_version": 1, "toolchain": toolchain,
            "qt": {"version": QT_VERSION, "architecture": architecture}, "aqt_source": args.aqt_source,
            "dependencies": {name: {key: row[key] for key in ("repository", "requested", "revision")}
                             for name, row in sources.items()},
        }, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
