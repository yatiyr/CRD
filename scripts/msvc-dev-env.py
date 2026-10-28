#!/usr/bin/env python3
"""Export the Visual Studio developer environment (vcvars64.bat, or vcvarsarm64.bat) to later workflow steps.

Finds the newest Visual Studio with the target's C++ tools through vswhere (as scripts/msvc-env.bat does), runs its
vcvars script in a child cmd.exe that then prints its whole environment as JSON through this interpreter, and appends
every variable vcvars added or changed to --github-env (the workflow's $GITHUB_ENV), the same variables the
developer command prompt sets. The environment must then name the compiler (`cl.exe` on the new PATH) and its
INCLUDE and LIB, or the step fails. It replaced ilammy/msvc-dev-cmd, which has no Node 24 release (2026-10-26).
Windows only. Contract: docs/design/pinned-inputs.md (workflow trust).
"""
from pathlib import Path
import argparse
import json
import os
import platform
import shutil
import subprocess
import sys

# Per target: the vswhere component that holds its C++ tools and the native-host vcvars script (x64 on an x64 host,
# ARM64 on an ARM64 host such as windows-11-arm).
ARCHES = {
    'x64': ('Microsoft.VisualStudio.Component.VC.Tools.x86.x64', 'vcvars64.bat'),
    'arm64': ('Microsoft.VisualStudio.Component.VC.Tools.ARM64', 'vcvarsarm64.bat'),
}
# nt.environ keeps each name's own case (os.environ upper-cases them on Windows), as the developer prompt spells it.
DUMP = 'import json, nt, sys; sys.stdout.write(json.dumps(dict(nt.environ)))'
REQUIRED = ('INCLUDE', 'LIB', 'VCToolsInstallDir')


class MsvcEnvironmentError(RuntimeError):
    """The developer environment could not be found or set up."""


def vswhere():
    path = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio' / \
        'Installer' / 'vswhere.exe'
    if not path.is_file():
        raise MsvcEnvironmentError(f'vswhere.exe was not found at {path}')
    return path


def installation(locator, arch):
    result = subprocess.run([str(locator), '-latest', '-products', '*', '-requires', ARCHES[arch][0],
                             '-property', 'installationPath'], capture_output=True, text=True, check=True)
    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    if not lines:
        raise MsvcEnvironmentError(f'no Visual Studio installation has the {arch} C++ build tools')
    return Path(lines[-1])


def developer_environment(vcvars, locator, base):
    """The environment the vcvars script leaves, as a dict, run from `base` with the vswhere directory on PATH (VsDevCmd
    runs vswhere by bare name, and a shell that skips the current directory would not find it)."""
    child = dict(base)
    child['PATH'] = f'{locator.parent};{base.get("PATH", "")}'
    command = f'"{vcvars}" >nul && "{sys.executable}" -I -c "{DUMP}"'
    # One command line, as cmd.exe reads it (/s: the outer quotes go, the rest runs as typed); a list would be
    # re-quoted for C runtime parsing, which cmd.exe does not do.
    result = subprocess.run(f'cmd.exe /d /s /c "{command}"', env=child, capture_output=True, text=True)
    if result.returncode != 0:
        raise MsvcEnvironmentError(f'{vcvars} failed with exit code {result.returncode}: {result.stderr.strip()}')
    return json.loads(result.stdout)


def changed(before, after):
    """Variables `after` adds or changes, compared case-insensitively as Windows names them; not PROMPT, the
    interactive prompt's text."""
    folded = {key.upper(): value for key, value in before.items()}
    return {key: value for key, value in after.items()
            if key.upper() != 'PROMPT' and folded.get(key.upper()) != value}


def github_env_lines(variables):
    """$GITHUB_ENV lines: `NAME=value`, or the delimited form for a value with a line break."""
    lines = []
    for key in sorted(variables):
        value = variables[key]
        if '\n' in value or '\r' in value:
            delimiter = 'CRD_MSVC_ENV_EOF'
            while delimiter in value:
                delimiter += '_'
            lines += [f'{key}<<{delimiter}', value, delimiter]
        else:
            lines.append(f'{key}={value}')
    return lines


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--github-env', type=Path, required=True, help='the file receiving NAME=value lines')
    parser.add_argument('--arch', choices=sorted(ARCHES), default='x64', help='the target and host architecture')
    args = parser.parse_args(argv)
    if platform.system() != 'Windows':
        parser.error('the Visual Studio developer environment exists on Windows only')
    try:
        locator = vswhere()
        root = installation(locator, args.arch)
        vcvars = root / 'VC' / 'Auxiliary' / 'Build' / ARCHES[args.arch][1]
        if not vcvars.is_file():
            raise MsvcEnvironmentError(f'{vcvars} is missing')
        base = dict(os.environ)
        after = developer_environment(vcvars, locator, base)
        missing = [name for name in REQUIRED if not after.get(name) and not after.get(name.upper())]
        compiler = shutil.which('cl.exe', path=after.get('PATH') or after.get('Path'))
        if missing or compiler is None:
            raise MsvcEnvironmentError(f'{vcvars.name} left no {", ".join(missing) or "cl.exe on PATH"}')
    except (MsvcEnvironmentError, subprocess.CalledProcessError, ValueError) as error:
        print(f'error: {error}', file=sys.stderr)
        return 1
    variables = changed(base, after)
    with args.github_env.open('a', encoding='utf-8') as out:
        for line in github_env_lines(variables):
            out.write(line + '\n')
    version = after.get('VCToolsVersion') or after.get('VCTOOLSVERSION')
    print(f'Visual Studio at {root}: MSVC tools {version}, compiler {compiler}; exported {len(variables)} variables.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
