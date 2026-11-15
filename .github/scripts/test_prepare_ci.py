"""Check Qwen's CI dependency preparation without downloading or compiling."""

import importlib.util
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location("prepare_ci", Path(__file__).with_name("prepare_ci.py"))
PREPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREPARE)

OWNED = ("vnm_framework", "vnm_qt_dispatch", "vnm_cmake")
EXACT = {name: str(index) * 40 for index, name in enumerate(OWNED, start=1)}
AQT = "git+https://github.com/miurahr/aqtinstall.git@" + "c" * 40


def git_revision(requested):
    """A fake run/check_output pair whose checkouts report the fetched revision."""
    fetched = {}

    def run(arguments, **options):
        if "fetch" in arguments:
            fetched[arguments[2]] = arguments[-1]

    def check_output(arguments, **options):
        reference = fetched[arguments[2]]
        return (requested.get(reference, reference) if reference != "master" else "f" * 40) + "\n"
    return run, check_output


class Preparation_tests(unittest.TestCase):
    def test_development_sources_follow_master_without_persisting_token(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for repository in OWNED:
                (root / repository).mkdir()
            run, check_output = git_revision({})
            with patch.object(PREPARE, "run", side_effect=run) as commands, \
                    patch.object(PREPARE.subprocess, "check_output", side_effect=check_output):
                sources = PREPARE.dependency_sources(root, "private-read-token", None)
            fetches = [call for call in commands.call_args_list if "fetch" in call.args[0]]
            self.assertEqual(len(fetches), 3)
            for call in fetches:
                self.assertEqual(call.args[0][-2:], ["origin", "master"])
                self.assertNotIn("private-read-token", " ".join(call.args[0]))
                self.assertIn("http.https://github.com/.extraheader", call.kwargs["env"].values())
            self.assertEqual({name: row["requested"] for name, row in sources.items()},
                             dict.fromkeys(OWNED, "master"))
            self.assertEqual({row["revision"] for row in sources.values()}, {"f" * 40})

    def test_release_sources_fetch_and_record_the_exact_revisions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run, check_output = git_revision({})
            with patch.object(PREPARE, "run", side_effect=run) as commands, \
                    patch.object(PREPARE.subprocess, "check_output", side_effect=check_output):
                sources = PREPARE.dependency_sources(root, "private-read-token", EXACT)
            fetched = [call.args[0][-1] for call in commands.call_args_list if "fetch" in call.args[0]]
            self.assertEqual(sorted(fetched), sorted(EXACT.values()))
            for name in OWNED:
                self.assertEqual(sources[name]["revision"], EXACT[name])
                self.assertEqual(sources[name]["requested"], EXACT[name])
                self.assertEqual(sources[name]["repository"], f"Varinomics/{name}")

    def test_a_checkout_other_than_the_requested_revision_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            run, check_output = git_revision({EXACT["vnm_cmake"]: "9" * 40})
            with patch.object(PREPARE, "run", side_effect=run), \
                    patch.object(PREPARE.subprocess, "check_output", side_effect=check_output):
                with self.assertRaisesRegex(RuntimeError, "vnm_cmake"):
                    PREPARE.dependency_sources(Path(directory), "private-read-token", EXACT)

    def test_release_revisions_name_every_owned_dependency_exactly(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "revisions.json"
            path.write_text(json.dumps({**EXACT, "logonomic": "a" * 40}), encoding="utf-8")
            self.assertEqual(PREPARE.read_revisions(path), EXACT)
            path.write_text(json.dumps({"vnm_framework": EXACT["vnm_framework"]}), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "vnm_qt_dispatch"):
                PREPARE.read_revisions(path)
            path.write_text(json.dumps({**EXACT, "vnm_cmake": "master"}), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "vnm_cmake"):
                PREPARE.read_revisions(path)

    def test_each_toolchain_selects_its_own_qt_kit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # A retail kit restored next to a Free request must never satisfy it.
            retail = root / "qt-win64_msvc2022_64" / PREPARE.QT_VERSION / "msvc2022_64/lib/cmake/Qt6Core"
            retail.mkdir(parents=True)
            (retail / "Qt6CoreConfig.cmake").touch()
            (root / f"qt-{PREPARE.QT_VERSION}-win64_msvc2022_64.ready").write_text(
                json.dumps({"aqt_source": AQT, "archives": ["qtbase"]}), encoding="utf-8")
            (root / f"qtbase-{PREPARE.QT_VERSION}").mkdir()

            def install(arguments, **options):
                if "install-qt" in arguments:
                    output = Path(arguments[arguments.index("--outputdir") + 1])
                    config = output / PREPARE.QT_VERSION / "llvm-mingw_64/lib/cmake/Qt6Core/Qt6CoreConfig.cmake"
                    config.parent.mkdir(parents=True)
                    config.touch()
            with patch.object(PREPARE, "run", side_effect=install) as commands:
                prefix, _ = PREPARE.prepare_qt(root, "windows-x86_64", "llvm-mingw", AQT)
            self.assertEqual(prefix, root / "qt-win64_llvm_mingw" / PREPARE.QT_VERSION / "llvm-mingw_64")
            installs = [call.args[0] for call in commands.call_args_list if "install-qt" in call.args[0]]
            self.assertEqual(len(installs), 1)
            self.assertIn("win64_llvm_mingw", installs[0])
            # The kit's LLVM runtime archive puts libc++.dll and libunwind.dll beside QtCore,
            # the only directory the package's Windows runtime closure searches.
            self.assertEqual(installs[0][installs[0].index("--archives") + 1:], ["qtbase", "llvm"])
            self.assertTrue((root / f"qt-{PREPARE.QT_VERSION}-win64_llvm_mingw.ready").is_file())
            with patch.object(PREPARE, "run") as commands:
                prefix, _ = PREPARE.prepare_qt(root, "windows-x86_64", "msvc", AQT)
            self.assertEqual(prefix, retail.parents[2])
            self.assertFalse([call for call in commands.call_args_list if "install-qt" in call.args[0]])

    def test_a_kit_installed_by_another_aqt_source_or_archive_set_is_installed_again(self):
        stale_installations = (
            {"aqt_source": PREPARE.DEVELOPMENT_AQT, "archives": ["qtbase", "llvm"]},
            {"aqt_source": AQT, "archives": ["qtbase"]},
            None)
        for stale in stale_installations:
            with tempfile.TemporaryDirectory() as directory, self.subTest(stale=stale):
                root = Path(directory)
                kit = root / "qt-win64_llvm_mingw" / PREPARE.QT_VERSION / "llvm-mingw_64"
                (kit / "lib/cmake/Qt6Core").mkdir(parents=True)
                (kit / "lib/cmake/Qt6Core/Qt6CoreConfig.cmake").touch()
                (kit / "bin").mkdir()
                (kit / "bin/stale.dll").touch()
                marker = root / f"qt-{PREPARE.QT_VERSION}-win64_llvm_mingw.ready"
                marker.write_text("" if stale is None else json.dumps(stale), encoding="utf-8")
                (root / f"qtbase-{PREPARE.QT_VERSION}").mkdir()

                def install(arguments, **options):
                    if "install-qt" in arguments:
                        output = Path(arguments[arguments.index("--outputdir") + 1])
                        config = output / PREPARE.QT_VERSION / "llvm-mingw_64/lib/cmake/Qt6Core/Qt6CoreConfig.cmake"
                        config.parent.mkdir(parents=True)
                        config.touch()
                with patch.object(PREPARE, "run", side_effect=install) as commands:
                    PREPARE.prepare_qt(root, "windows-x86_64", "llvm-mingw", AQT)
                self.assertEqual(len([call for call in commands.call_args_list if "install-qt" in call.args[0]]), 1)
                self.assertFalse((kit / "bin/stale.dll").exists())
                self.assertEqual(json.loads(marker.read_text(encoding="utf-8")),
                                 {"aqt_source": AQT, "archives": ["qtbase", "llvm"]})

    def test_qt_installation_uses_the_selected_aqt_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / f"qtbase-{PREPARE.QT_VERSION}").mkdir()

            def install(arguments, **options):
                if "install-qt" in arguments:
                    output = Path(arguments[arguments.index("--outputdir") + 1])
                    config = output / PREPARE.QT_VERSION / "gcc_64/lib/cmake/Qt6Core/Qt6CoreConfig.cmake"
                    config.parent.mkdir(parents=True)
                    config.touch()
            with patch.object(PREPARE, "run", side_effect=install) as commands:
                PREPARE.prepare_qt(root, "linux-x86_64", "clang", AQT)
            installs = [call.args[0] for call in commands.call_args_list if "pip" in call.args[0]]
            self.assertEqual(len(installs), 1)
            self.assertEqual(installs[0][-1], AQT)
            kit = next(call.args[0] for call in commands.call_args_list if "install-qt" in call.args[0])
            self.assertEqual(kit[kit.index("--archives") + 1:], ["qtbase", "icu"])

    def test_cached_qt_reuses_binaries_and_selects_matching_notice_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = root / "qt-linux_gcc_64" / PREPARE.QT_VERSION / "kit/lib/cmake/Qt6Core/Qt6CoreConfig.cmake"
            config.parent.mkdir(parents=True)
            config.touch()
            (root / f"qt-{PREPARE.QT_VERSION}-linux_gcc_64.ready").write_text(
                json.dumps({"aqt_source": AQT, "archives": ["qtbase", "icu"]}), encoding="utf-8")
            notices = root / f"qtbase-{PREPARE.QT_VERSION}"
            notices.mkdir()
            with patch.object(PREPARE, "run") as run:
                prefix, selected_notices = PREPARE.prepare_qt(root, "linux-x86_64", "gcc", AQT)
            self.assertEqual(prefix, root / "qt-linux_gcc_64" / PREPARE.QT_VERSION / "kit")
            self.assertEqual(selected_notices, notices)
            self.assertEqual(len(run.call_args_list), 1)
            self.assertIn("sparse-checkout", run.call_args.args[0])

    def test_toolchain_must_belong_to_the_platform(self):
        with self.assertRaisesRegex(ValueError, "llvm-mingw.*linux-x86_64"):
            PREPARE.qt_kit("linux-x86_64", "llvm-mingw")
        self.assertEqual(PREPARE.qt_kit("windows-x86_64", "msvc"), ("windows", "win64_msvc2022_64"))
        self.assertEqual(PREPARE.qt_kit("linux-x86_64", "clang"), PREPARE.qt_kit("linux-x86_64", "gcc"))

    def test_llvm_runtime_notices_join_the_supplier_licence_and_mingw_runtime_copying(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "llvm-mingw2217_64"
            copying = root / "x86_64-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt"
            copying.parent.mkdir(parents=True)
            (root / "LICENSE.TXT").write_bytes(b"LLVM licence\n\n")
            copying.write_bytes(b"MinGW-w64 runtime \xa9\n")
            destination = Path(directory) / "notices"
            PREPARE.prepare_llvm_runtime_notices(root, destination)
            expected = b"LLVM licence\n\n\n\nMinGW-w64 runtime \xa9\n"
            self.assertEqual(sorted(path.name for path in destination.iterdir()),
                             ["libc++.dll.txt", "libunwind.dll.txt"])
            for name in ("libc++.dll.txt", "libunwind.dll.txt"):
                self.assertEqual((destination / name).read_bytes(), expected)
            copying.unlink()
            with self.assertRaises(FileNotFoundError):
                PREPARE.prepare_llvm_runtime_notices(root, Path(directory) / "other")

    def test_icu_notice_comes_from_the_runtime_release_and_preserves_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "lib").mkdir()
            for name in ("libicuuc.so.73.2", "libicui18n.so.73.2"):
                (root / "lib" / name).touch()
            license_bytes = b"Copyright ICU contributors\nLicense bytes: \xa9\n"
            with patch.object(PREPARE.urllib.request, "urlopen", return_value=io.BytesIO(license_bytes)) as request:
                PREPARE.prepare_icu_notices(root, root / "notices")
            self.assertEqual(request.call_count, 1)
            self.assertIn("/release-73-2/icu4c/LICENSE", request.call_args.args[0])
            for name in ("libicuuc.so.73.2", "libicui18n.so.73.2"):
                self.assertEqual((root / "notices" / (name + ".txt")).read_bytes(), license_bytes)

    def run_main(self, root, arguments):
        output = root / "arguments.json"
        github_path = root / "github-path"
        sources = {name: {"path": root / name, "repository": f"Varinomics/{name}",
                          "requested": EXACT[name], "revision": EXACT[name]} for name in OWNED}
        command = ["prepare_ci.py", "--workspace", str(root), "--output", str(output), *arguments]
        with patch.object(PREPARE.sys, "argv", command), \
             patch.dict(os.environ, {"VARINOMICS_REPO_READ_TOKEN": "private-read-token",
                                     "GITHUB_PATH": str(github_path)}), \
             patch.object(PREPARE, "dependency_sources", return_value=sources) as dependencies, \
             patch.object(PREPARE, "prepare_qt", return_value=(root / "qt", root / "qtbase")) as qt:
            PREPARE.main()
            self.assertNotIn("VARINOMICS_REPO_READ_TOKEN", os.environ)
        self.assertNotIn("private-read-token", output.read_text(encoding="utf-8"))
        self.assertEqual(github_path.read_text(encoding="utf-8"), str(root / "qt/bin") + "\n")
        return json.loads(output.read_text(encoding="utf-8")), dependencies, qt

    def test_callback_emits_source_overrides_and_qt_runtime_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cmake_args, dependencies, qt = self.run_main(root, ["--platform", "macos-arm64"])
            self.assertIn(f"-DQWEN_FRAMEWORK_SOURCE={(root / 'vnm_framework').as_posix()}", cmake_args)
            self.assertIn(f"-DFETCHCONTENT_SOURCE_DIR_VNM_CMAKE={(root / 'vnm_cmake').as_posix()}", cmake_args)
            self.assertIn(f"-DQWEN_QTBASE_SOURCE={(root / 'qtbase').as_posix()}", cmake_args)
            self.assertIn("-DVNM_FILESYSTEM_OBSERVATION_BUILD_TESTS=OFF", cmake_args)
            self.assertFalse(any(argument.startswith("-DCMAKE_EXE_LINKER_FLAGS_RELEASE=") for argument in cmake_args))
            self.assertIsNone(dependencies.call_args.args[2])
            self.assertEqual(qt.call_args.args[2:], ("clang", PREPARE.DEVELOPMENT_AQT))

    def test_llvm_mingw_callback_supplies_runtime_notices_and_records_its_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            toolchain = root / "llvm-mingw2217_64"
            copying = toolchain / "x86_64-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt"
            copying.parent.mkdir(parents=True)
            (toolchain / "LICENSE.TXT").write_bytes(b"LLVM\n")
            copying.write_bytes(b"MinGW\n")
            revisions = root / "revisions.json"
            revisions.write_text(json.dumps(EXACT), encoding="utf-8")
            record = root / "record.json"
            cmake_args, dependencies, qt = self.run_main(root, [
                "--platform", "windows-x86_64", "--toolchain", "llvm-mingw", "--toolchain-root", str(toolchain),
                "--aqt-source", AQT, "--revisions", str(revisions), "--record", str(record)])
            notices = root / ".ci-dependencies/qwen/llvm-mingw-runtime-notices"
            self.assertIn(f"-DQWEN_RUNTIME_NOTICES_DIRECTORY={notices.as_posix()}", cmake_args)
            self.assertIn("-DCMAKE_EXE_LINKER_FLAGS_RELEASE=-Wl,--strip-all", cmake_args)
            self.assertEqual((notices / "libc++.dll.txt").read_bytes(), b"LLVM\n\n\nMinGW\n")
            self.assertEqual(dependencies.call_args.args[2], EXACT)
            self.assertEqual(qt.call_args.args[2:], ("llvm-mingw", AQT))
            facts = json.loads(record.read_text(encoding="utf-8"))
            self.assertEqual(facts["schema_version"], 1)
            self.assertEqual(facts["toolchain"], "llvm-mingw")
            self.assertEqual(facts["qt"], {"version": PREPARE.QT_VERSION, "architecture": "win64_llvm_mingw"})
            self.assertEqual(facts["aqt_source"], AQT)
            self.assertEqual(facts["dependencies"], {name: {"repository": f"Varinomics/{name}",
                                                            "requested": EXACT[name], "revision": EXACT[name]}
                                                     for name in OWNED})

    def test_llvm_mingw_requires_its_toolchain_root(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "--toolchain-root"):
                self.run_main(Path(directory), ["--platform", "windows-x86_64", "--toolchain", "llvm-mingw"])

    def test_linux_acquisition_material_binds_qt_and_icu_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, notices = root / "qt", root / "qtbase"
            (prefix / "lib").mkdir(parents=True)
            (notices / "LICENSES").mkdir(parents=True)
            qt = prefix / "lib/libQt6Core.so.6"
            icu = prefix / "lib/libicuuc.so.73.2"
            qt.write_bytes(b"supplied QtCore")
            icu.write_bytes(b"supplied ICU")
            (notices / "LICENSES/LGPL-3.0-only.txt").write_text("Qt license")
            icu_notice = root / "icu-LICENSE"
            icu_notice.write_text("ICU license")
            output = root / "material.json"
            PREPARE.prepare_linux_runtime_material(prefix, notices, {icu: ("73.2", icu_notice)}, output)
            facts = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(facts["schema_version"], 1)
            qt_row, icu_row = facts["inputs"]
            self.assertEqual(qt_row["source"], str(qt.resolve()))
            self.assertEqual(qt_row["source_sha256"], hashlib.sha256(qt.read_bytes()).hexdigest())
            self.assertEqual(icu_row["source_version"], "73.2")
            self.assertEqual(icu_row["source_url"], "https://github.com/unicode-org/icu/tree/release-73-2")
            self.assertEqual(icu_row["notice_sha256"], hashlib.sha256(icu_notice.read_bytes()).hexdigest())
            self.assertEqual(icu_row["acquisition"], {"provider": "qt-online-installer", "host": "linux",
                                                    "target": "desktop", "version": PREPARE.QT_VERSION,
                                                    "architecture": "linux_gcc_64", "archive": "icu"})


if __name__ == "__main__":
    unittest.main()
