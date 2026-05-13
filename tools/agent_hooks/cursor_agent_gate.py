#!/usr/bin/env python3
"""Fallback wrapper for Cursor Agent CLI when native hooks are unavailable."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys

import dpm_gate


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prompt", required=True)
    parser.add_argument("--model")
    args = parser.parse_args()

    if shutil.which("cursor-agent") is None:
        print("cursor-agent not found on PATH", file=sys.stderr)
        return 127

    root = dpm_gate.project_root()
    prompt_event = {
        "hook_event_name": "UserPromptSubmit",
        "prompt": args.prompt,
        "cwd": str(root),
    }
    response = dpm_gate.handle_hook(prompt_event, root, "cursor")
    if response:
        print(json.dumps(response, separators=(",", ":")), file=sys.stderr)

    cmd = ["cursor-agent", "-p", args.prompt, "--output-format", "stream-json"]
    if args.model:
        cmd.extend(["--model", args.model])

    with subprocess.Popen(
        cmd,
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    ) as proc:
        assert proc.stdout is not None
        for line in proc.stdout:
            print(line, end="")
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            dpm_gate.append_ledger(
                root,
                "cursor_stream",
                {
                    "hook_event_name": "cursor_stream",
                    "cwd": str(root),
                    "tool_response": event,
                },
            )
        return proc.wait()


if __name__ == "__main__":
    raise SystemExit(main())
