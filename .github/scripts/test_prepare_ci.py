"""Exercise Qwen's thin callback with the native provider driver's arguments."""

import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


PARSER = argparse.ArgumentParser()
PARSER.add_argument("--sdk", type=Path, required=True)
ARGS, TEST_ARGUMENTS = PARSER.parse_known_args()
SCRIPT = Path(__file__).with_name("prepare_ci.py")


class Preparation_callback_tests(unittest.TestCase):
    def test_selected_sdk_exposes_the_shared_entry_point(self):
        specification = importlib.util.spec_from_file_location(
            "prepare_observation_ci", ARGS.sdk / "cmake/prepare_observation_ci.py")
        preparation = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(preparation)
        self.assertTrue(callable(preparation.main))

    def test_native_driver_arguments_reach_the_selected_sdk(self):
        # Acquisition is outside this wrapper's ownership. A recording module
        # isolates the invocation boundary without downloading dependencies.
        for explicit_sdk in (False, True):
            with tempfile.TemporaryDirectory() as directory, self.subTest(explicit_sdk=explicit_sdk):
                root = Path(directory)
                sdk = root / ("selected-sdk" if explicit_sdk else "sdk-owner/sdk/logonomic_cli_addon_sdk")
                module = sdk / "cmake/prepare_observation_ci.py"
                module.parent.mkdir(parents=True)
                output = root / "arguments.json"
                module.write_text(
                    "import json\nfrom pathlib import Path\n"
                    "def main(provider, arguments):\n"
                    f"    Path({str(output)!r}).write_text(json.dumps([provider, arguments]))\n",
                    encoding="utf-8")
                arguments = [
                    "--platform", "windows-x86_64", "--toolchain", "llvm-mingw",
                    "--aqt-source", "git+https://github.com/miurahr/aqtinstall.git@" + "a" * 40,
                    "--record", str(root / "provider-preparation.json"),
                    "--workspace", str(root), "--output", str(root / "provider-configure.json"),
                    "--toolchain-root", str(root / "llvm-mingw"),
                    "--revisions", str(root / "provider-revisions.json"),
                ]
                invocation = [sys.executable, "-B", str(SCRIPT), *arguments]
                if explicit_sdk:
                    invocation += ["--sdk-source", str(sdk)]
                subprocess.run(invocation, check=True, capture_output=True, text=True)
                provider, forwarded = json.loads(output.read_text(encoding="utf-8"))
                self.assertEqual(provider, "qwen")
                self.assertEqual(dict(zip(forwarded[::2], forwarded[1::2])),
                                 dict(zip(arguments[::2], arguments[1::2])))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *TEST_ARGUMENTS])
