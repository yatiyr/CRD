#!/usr/bin/env python3
"""Cross-platform Vulkan development environment helper for Cerid.

Default mode is read-only: detect the current platform, inspect the Vulkan
runtime / SDK / shader tools, and print recommended installation commands.

Optional flags:

  --print-install   Print the install commands for the detected platform.
  --install         Execute the recommended install commands.
  --json            Emit machine-readable JSON instead of pretty text.

The script is intentionally conservative:
- it does not assume the Vulkan SDK is required for every step
- shader tools are treated as optional for early RHI bootstrap
- install commands are best-effort and platform-specific
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass
from typing import List, Sequence


@dataclass
class ToolStatus:
    name: str
    found: bool
    path: str


def which(name: str) -> ToolStatus:
    path = shutil.which(name)
    return ToolStatus(name=name, found=path is not None, path=path or "")


def detect_platform() -> str:
    value = platform.system().lower()
    if value.startswith("win"):
        return "windows"
    if value == "darwin":
        return "macos"
    if value == "linux":
        return "linux"
    return value


def recommended_install_commands(host: str) -> List[str]:
    if host == "windows":
        return [
            "winget install KhronosGroup.VulkanSDK",
            "winget install KhronosGroup.VulkanRT",
        ]
    if host == "linux":
        return [
            "sudo apt-get update",
            "sudo apt-get install -y libvulkan-dev vulkan-tools vulkan-validationlayers-dev glslang-tools spirv-tools",
        ]
    if host == "macos":
        return [
            "brew install molten-vk glslang spirv-tools",
        ]
    return []


def gather_state() -> dict:
    host = detect_platform()
    tools = [which("vulkaninfo"), which("glslangValidator"), which("dxc")]
    sdk = os.environ.get("VULKAN_SDK", "")

    runtime_present = any(tool.name == "vulkaninfo" and tool.found for tool in tools)
    shader_tools_present = {
        tool.name: tool.found
        for tool in tools
        if tool.name in {"glslangValidator", "dxc"}
    }

    return {
        "platform": host,
        "vulkan_sdk": sdk,
        "tools": [asdict(tool) for tool in tools],
        "runtime_present": runtime_present,
        "shader_tools_present": shader_tools_present,
        "recommended_install_commands": recommended_install_commands(host),
        "notes": [
            "crd-rhi v1a does not require the Vulkan SDK.",
            "crd-rhi-vulkan bootstrap needs Vulkan headers and the loader at build time.",
            "glslangValidator and dxc are optional for early backend bootstrap; they matter later for shader tooling.",
        ],
    }


def print_human(state: dict, print_install: bool) -> None:
    print("== Cerid Vulkan Dev Check ==")
    print()
    sdk = state["vulkan_sdk"]
    print(f"Platform: {state['platform']}")
    print(f"VULKAN_SDK: {sdk if sdk else 'not set'}")
    print()
    print("Tools:")
    for tool in state["tools"]:
        prefix = "[OK]" if tool["found"] else "[MISS]"
        suffix = f" -> {tool['path']}" if tool["found"] else ""
        print(f"  {prefix:<6} {tool['name']}{suffix}")
    print()
    print("Interpretation:")
    for note in state["notes"]:
        print(f"  - {note}")
    print()
    if state["runtime_present"]:
        print(
            "Recommendation: Vulkan runtime appears present. Proceed to crd-rhi-vulkan."
        )
    else:
        print(
            "Recommendation: install/configure Vulkan runtime/SDK before crd-rhi-vulkan work."
        )

    if print_install:
        print()
        print("Recommended install commands:")
        commands: Sequence[str] = state["recommended_install_commands"]
        if not commands:
            print("  (no known package-manager commands for this platform)")
        for command in commands:
            print(f"  {command}")


def run_install(commands: Sequence[str]) -> int:
    if not commands:
        print("No install commands known for this platform.", file=sys.stderr)
        return 1

    for command in commands:
        print(f"[install] {command}")
        completed = subprocess.run(command, shell=True)
        if completed.returncode != 0:
            return completed.returncode
    return 0


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Cerid Vulkan development environment helper"
    )
    parser.add_argument(
        "--print-install",
        action="store_true",
        help="print recommended install commands",
    )
    parser.add_argument(
        "--install", action="store_true", help="execute recommended install commands"
    )
    parser.add_argument("--json", action="store_true", help="emit JSON")
    args = parser.parse_args(argv)

    state = gather_state()

    if args.json:
        print(json.dumps(state, indent=2))
    else:
        print_human(state, print_install=args.print_install or args.install)

    if args.install:
        return run_install(state["recommended_install_commands"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
