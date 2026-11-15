"""Prepare Qwen's observation dependencies using the SDK selected by provider CI."""

import argparse
import importlib.util
from pathlib import Path


def main(arguments=None):
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--sdk-source", type=Path)
    args, preparation_arguments = parser.parse_known_args(arguments)
    # The native driver configures with this same SDK owner. A standalone
    # invocation may select its source explicitly instead of recreating CI layout.
    sdk = args.sdk_source or args.workspace / "sdk-owner/sdk/logonomic_cli_addon_sdk"
    script = sdk / "cmake/prepare_observation_ci.py"
    specification = importlib.util.spec_from_file_location("prepare_observation_ci", script)
    preparation = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(preparation)
    preparation.main("qwen", ["--workspace", str(args.workspace), *preparation_arguments])


if __name__ == "__main__":
    main()
