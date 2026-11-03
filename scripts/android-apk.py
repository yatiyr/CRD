#!/usr/bin/env python3
"""Build an instrumentation APK around one native test library, without Gradle (the Android test lane, NET.2).

Only the pinned kit (scripts/install-android-emulator.py) is used, from the SDK root `--sdk` (default
ANDROID_SDK_ROOT, else ANDROID_HOME): its JDK compiles the Java sources against the API 31 platform's android.jar,
the build-tools' d8 turns them into classes.dex, aapt2 links the manifest, the library (stripped when `--strip` names
the NDK's llvm-strip) goes to lib/<abi>/, then zipalign and apksigner sign it with a debug key the script makes once
in the work directory. The APK is written to `--output` only when complete and verified by apksigner; every tool's
failure fails the build with its own output.
"""
from pathlib import Path
import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
import zipfile

KEY_ALIAS = 'cerid-test'
# A test key in the build tree signs a test APK; the password protects nothing.
KEY_PASSWORD = 'cerid-test'


def kit_module():
    spec = importlib.util.spec_from_file_location('install_android_emulator',
                                                  Path(__file__).resolve().parent / 'install-android-emulator.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sdk_root(given=None):
    if given:
        return Path(given)
    root = os.environ.get('ANDROID_SDK_ROOT') or os.environ.get('ANDROID_HOME')
    return Path(root) if root else kit_module().default_sdk()


class Kit:
    """The kit's tools, by their places in the SDK root."""

    def __init__(self, sdk):
        kit = kit_module()
        self.java = sdk / kit.JDK_DIR / 'bin' / 'java'
        self.javac = sdk / kit.JDK_DIR / 'bin' / 'javac'
        self.keytool = sdk / kit.JDK_DIR / 'bin' / 'keytool'
        tools = sdk / kit.BUILD_TOOLS_DIR
        self.d8 = tools / 'lib' / 'd8.jar'
        self.apksigner = tools / 'lib' / 'apksigner.jar'
        self.aapt2 = tools / 'aapt2'
        self.zipalign = tools / 'zipalign'
        self.android_jar = sdk / kit.PLATFORM_DIR / 'android.jar'

    def missing(self):
        return [str(path) for path in vars(self).values() if not Path(path).is_file()]


def run(argv):
    result = subprocess.run([str(word) for word in argv], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        raise RuntimeError('%s exited %d:\n%s' % (Path(argv[0]).name, result.returncode, result.stdout))
    return result.stdout


def package(linked, dex, library, abi, unsigned):
    """`linked` (aapt2's APK) with classes.dex and lib/<abi>/<library> added, written to `unsigned`. Every added
    entry carries aapt2's fixed time stamp, so the same inputs make the same APK."""
    with zipfile.ZipFile(linked) as source, zipfile.ZipFile(unsigned, 'w', zipfile.ZIP_DEFLATED) as target:
        for info in source.infolist():
            target.writestr(info, source.read(info))
        for path, name in ((dex, 'classes.dex'), (library, 'lib/%s/%s' % (abi, Path(library).name))):
            entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o644 << 16
            target.writestr(entry, Path(path).read_bytes())

def build(args):
    kit = Kit(sdk_root(args.sdk))
    missing = kit.missing()
    if missing:
        print('the pinned Android test kit is incomplete (scripts/install-android-emulator.py): ' + ', '.join(missing),
              file=sys.stderr)
        return 1
    work = Path(args.work)
    classes, dex = work / 'classes', work / 'dex'
    for directory in (classes, dex):
        shutil.rmtree(directory, ignore_errors=True)
        directory.mkdir(parents=True)
    sources = sorted(Path(args.java).rglob('*.java'))
    try:
        run([kit.javac, '--release', '11', '-Xlint:all', '-Werror', '-classpath', kit.android_jar, '-d', classes,
             *sources])
        run([kit.java, '-cp', kit.d8, 'com.android.tools.r8.D8', '--release', '--min-api', str(args.min_api),
             '--lib', kit.android_jar, '--output', dex, *sorted(classes.rglob('*.class'))])
        linked = work / 'linked.apk'
        run([kit.aapt2, 'link', '-I', kit.android_jar, '--manifest', args.manifest, '-o', linked])
        library = Path(args.library)
        if args.strip:
            stripped = work / library.name
            run([args.strip, '--strip-debug', '-o', stripped, library])
            library = stripped
        unsigned, aligned = work / 'unsigned.apk', work / 'aligned.apk'
        package(linked, dex / 'classes.dex', library, args.abi, unsigned)
        run([kit.zipalign, '-f', '-p', '4', unsigned, aligned])
        keystore = work / 'debug.keystore'
        if not keystore.is_file():
            run([kit.keytool, '-genkeypair', '-keystore', keystore, '-storepass', KEY_PASSWORD, '-alias', KEY_ALIAS,
                 '-keypass', KEY_PASSWORD, '-keyalg', 'EC', '-groupname', 'secp256r1', '-validity', '10000',
                 '-dname', 'CN=Cerid test'])
        signed = work / 'signed.apk'
        run([kit.java, '-jar', kit.apksigner, 'sign', '--ks', keystore, '--ks-pass', 'pass:' + KEY_PASSWORD,
             '--ks-key-alias', KEY_ALIAS, '--out', signed, aligned])
        run([kit.java, '-jar', kit.apksigner, 'verify', signed])
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 1
    os.replace(signed, args.output)
    return 0


def check(args):
    missing = Kit(sdk_root(args.sdk)).missing()
    if missing:
        print('missing: ' + ', '.join(missing), file=sys.stderr)
    return 1 if missing else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest='command', required=True)
    probe = commands.add_parser('check', help='whether the SDK root holds every tool the build uses')
    make = commands.add_parser('build', help='build and sign the APK')
    for command in (probe, make):
        command.add_argument('--sdk', help='the SDK root of the kit (default: ANDROID_SDK_ROOT, else ANDROID_HOME)')
    make.add_argument('--manifest', required=True)
    make.add_argument('--java', required=True, help='the directory holding the Java sources')
    make.add_argument('--library', required=True, help='the native library the APK loads')
    make.add_argument('--abi', default='x86_64')
    make.add_argument('--min-api', type=int, default=31)
    make.add_argument('--strip', help='the llvm-strip that removes debug information from the library')
    make.add_argument('--work', required=True, help='a work directory of this build alone')
    make.add_argument('--output', required=True)
    args = parser.parse_args(argv)
    return {'check': check, 'build': build}[args.command](args)

if __name__ == '__main__':
    sys.exit(main())
