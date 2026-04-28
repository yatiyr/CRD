#!/usr/bin/env python3
"""Cross-platform Vulkan development environment helper for Cerid.

Default mode is read-only: detect the current platform, inspect the Vulkan
runtime / SDK / shader tools, and print recommended installation commands.

Optional flags:

  --print-install   Print the install commands for the detected platform.
  --install         Execute the recommended install commands.
  --json            Emit machine-readable JSON instead of pretty text.
  --configure-env   Configure user environment for discovered tooling.

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
from pathlib import Path
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


def find_windows_vulkan_sdk() -> str:
    root = Path("C:/VulkanSDK")
    if not root.exists():
        return ""
    versions = [p for p in root.iterdir() if p.is_dir()]
    if not versions:
        return ""
    latest = sorted(versions, key=lambda p: p.name)[-1]
    return str(latest)


def fallback_tool_path(host: str, name: str, sdk: str) -> str:
    if host == "windows":
        if name == "glslangValidator":
            sdk_root = sdk or find_windows_vulkan_sdk()
            if sdk_root:
                candidate = Path(sdk_root) / "Bin" / "glslangValidator.exe"
                if candidate.exists():
                    return str(candidate)
        if name == "dxc":
            local = os.environ.get("LOCALAPPDATA", "")
            if local:
                candidate = (
                    Path(local)
                    / "Microsoft"
                    / "WinGet"
                    / "Packages"
                    / "Microsoft.DirectX.ShaderCompiler_Microsoft.Winget.Source_8wekyb3d8bbwe"
                    / "bin"
                    / "x64"
                    / "dxc.exe"
                )
                if candidate.exists():
                    return str(candidate)
    return ""


def detect_tool(host: str, name: str, sdk: str) -> ToolStatus:
    direct = which(name)
    if direct.found:
        return direct
    fallback = fallback_tool_path(host, name, sdk)
    if fallback:
        return ToolStatus(name=name, found=True, path=fallback)
    return direct


def detect_platform() -> str:
    value = platform.system().lower()
    if value.startswith("win"):
        return "windows"
    if value == "darwin":
        return "macos"
    if value == "linux":
        return "linux"
    return value


def recommended_install_commands(
    host: str, tools: Sequence[ToolStatus], sdk: str
) -> List[str]:
    has_vulkaninfo = any(tool.name == "vulkaninfo" and tool.found for tool in tools)
    has_glslang = any(tool.name == "glslangValidator" and tool.found for tool in tools)
    has_dxc = any(tool.name == "dxc" and tool.found for tool in tools)

    commands: List[str] = []

    if host == "windows":
        # Vulkan SDK is the cleanest way to get headers, loader import libs,
        # validation layers, and glslangValidator in one supported bundle.
        if (not sdk) or (not has_glslang):
            commands.append(
                "winget install --id KhronosGroup.VulkanSDK --accept-package-agreements --accept-source-agreements --disable-interactivity"
            )
        # If the runtime is somehow missing on a machine that still lacks vulkaninfo,
        # request it explicitly too.
        if not has_vulkaninfo:
            commands.append(
                "winget install --id KhronosGroup.VulkanRT --accept-package-agreements --accept-source-agreements --disable-interactivity"
            )
        if not has_dxc:
            commands.append(
                "winget install --id Microsoft.DirectX.ShaderCompiler --accept-package-agreements --accept-source-agreements --disable-interactivity"
            )
        return commands

    if host == "linux":
        if (not has_vulkaninfo) or (not has_glslang) or (not sdk):
            commands.extend(
                [
                    "sudo apt-get update",
                    "sudo apt-get install -y libvulkan-dev vulkan-tools vulkan-validationlayers-dev glslang-tools spirv-tools",
                ]
            )
        return commands

    if host == "macos":
        if (not has_vulkaninfo) or (not has_glslang):
            commands.append("brew install molten-vk glslang spirv-tools")
        if not has_dxc:
            commands.append("brew install directxshadercompiler")
        return commands

    return commands


def ensure_windows_user_env_var(name: str, value: str) -> None:
    current = os.environ.get(name, "")
    if current == value:
        return
    subprocess.run(
        [
            "powershell",
            "-NoProfile",
            "-Command",
            f"[Environment]::SetEnvironmentVariable('{name}', @'\n{value}\n'@.TrimEnd(), 'User')",
        ],
        check=True,
        shell=False,
    )


def ensure_windows_user_path_entries(entries: Sequence[str]) -> None:
    current = os.environ.get("PATH", "")
    parts = [p for p in current.split(";") if p]
    lower_parts = {p.lower() for p in parts}
    changed = False
    for entry in entries:
        if entry and entry.lower() not in lower_parts:
            parts.append(entry)
            lower_parts.add(entry.lower())
            changed = True
    if changed:
        new_path = ";".join(parts)
        subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-Command",
                f"[Environment]::SetEnvironmentVariable('PATH', @'\n{new_path}\n'@.TrimEnd(), 'User')",
            ],
            check=True,
            shell=False,
        )


def configure_environment(state: dict) -> None:
    host = state["platform"]
    if host != "windows":
        return

    sdk = state["vulkan_sdk"] or find_windows_vulkan_sdk()
    if sdk:
        ensure_windows_user_env_var("VULKAN_SDK", sdk)

    path_entries: List[str] = []
    if sdk:
        path_entries.append(str(Path(sdk) / "Bin"))

    for tool in state["tools"]:
        if tool["found"] and tool["path"]:
            path_entries.append(str(Path(tool["path"]).parent))

    ensure_windows_user_path_entries(path_entries)


def gather_state() -> dict:
    host = detect_platform()
    sdk = os.environ.get("VULKAN_SDK", "")
    if host == "windows" and not sdk:
        sdk = find_windows_vulkan_sdk()

    tools = [
        detect_tool(host, "vulkaninfo", sdk),
        detect_tool(host, "glslangValidator", sdk),
        detect_tool(host, "dxc", sdk),
    ]

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
        "recommended_install_commands": recommended_install_commands(host, tools, sdk),
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
    parser.add_argument(
        "--configure-env",
        action="store_true",
        help="configure user environment for discovered Vulkan tooling",
    )
    args = parser.parse_args(argv)

    state = gather_state()

    if args.json:
        print(json.dumps(state, indent=2))
    else:
        print_human(state, print_install=args.print_install or args.install)

    if args.install:
        result = run_install(state["recommended_install_commands"])
        if result != 0:
            return result
        state = gather_state()
        configure_environment(state)
        if not args.json:
            print()
            print(
                "Environment configured. Re-run the script in a new shell to see refreshed PATH/VULKAN_SDK."
            )
        return 0

    if args.configure_env:
        configure_environment(state)
        if not args.json:
            print()
            print(
                "Environment configured. Re-run the script in a new shell to see refreshed PATH/VULKAN_SDK."
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
