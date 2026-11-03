#!/usr/bin/env python3
"""Install the pinned Android test kit for the Android test lane: platform-tools, the emulator and the API 31 x86_64
system image (tools/android-platform-tools-linux, android-emulator-linux-x64 and android-system-image-31-x86_64 in
cmake/pins.json), which boot the device, and the build-tools, the API 31 platform and a JDK (android-build-tools-linux,
android-platform-31 and temurin-jdk-21-linux-x64), which build the instrumentation APK (scripts/android-apk.py); then
write the AVD the lane boots.

Each archive is its publisher's (Google's SDK repository zips, Adoptium's JDK tarball), verified by its SHA-256 (the
registry also records the size and SHA-1 the repository index lists) and unpacked by pins.install_tree into its place
in an SDK root: `<sdk>/platform-tools`, `<sdk>/emulator`, `<sdk>/system-images/android-31/default/x86_64`,
`<sdk>/build-tools/<version>`, `<sdk>/platforms/android-31` and `<sdk>/jdk`, each beside a stamp, so a matching install
is reused. Every part must report the pinned version (source.properties, or the JDK's release file); the image must
report the API level, ABI and tag its place names. The AVD (`cerid-api31-x86_64`) goes under `<sdk>/avd` unless
another home is given. Prints ANDROID_HOME, ANDROID_SDK_ROOT and ANDROID_AVD_HOME, which scripts/android-device.py,
the Android preset's configure and the SDK's own tools read (with `--github-env` also appended to that file): the
emulator takes ANDROID_HOME before ANDROID_SDK_ROOT, and a hosted runner's image sets ANDROID_HOME to its own SDK. Only
the test lanes use it (NET.2): never a shipped build. Linux x64 only: the registry pins no other host build.
Contract: docs/design/pinned-inputs.md.
"""
from pathlib import Path
import argparse
import platform
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

STAMP = 'cerid-android-sdk.json'
AVD = 'cerid-api31-x86_64'
IMAGE = {'AndroidVersion.ApiLevel': '31', 'SystemImage.Abi': 'x86_64', 'SystemImage.TagId': 'default'}
IMAGE_DIR = 'system-images/android-31/default/x86_64'
BUILD_TOOLS = '37.0.0'
BUILD_TOOLS_DIR = 'build-tools/' + BUILD_TOOLS
PLATFORM_DIR = 'platforms/android-31'
JDK_DIR = 'jdk'


def default_sdk():
    return Path.home() / '.cerid' / 'tools' / 'android-sdk'


def properties(directory, name='source.properties'):
    """The key/value pairs of `directory`/`name` (unquoted), empty when it is missing."""
    try:
        text = (Path(directory) / name).read_text(encoding='utf-8')
    except OSError:
        return {}
    pairs = (line.partition('=') for line in text.splitlines())
    return {key.strip(): value.strip().strip('"') for key, _, value in pairs if key.strip()}


def reported_version(binary):
    """The Pkg.Revision of the SDK package holding `binary`."""
    return properties(Path(binary).parent).get('Pkg.Revision')


def reported_platform(jar):
    """`<API level>-r<revision>` of the SDK platform holding `jar` (android.jar), the platform pin's version form."""
    found = properties(Path(jar).parent)
    level, revision = found.get('AndroidVersion.ApiLevel'), found.get('Pkg.Revision')
    return '%s-r%02d' % (level, int(revision)) if level and revision and revision.isdigit() else None


def reported_jdk(javac):
    """The JDK's runtime version without its `-LTS` suffix, from the release file beside its bin directory."""
    version = properties(Path(javac).parent.parent, 'release').get('JAVA_RUNTIME_VERSION')
    return version.partition('-')[0] if version else None


# (pin, place in the SDK root, the file the install must hold, how the install reports its version: None for the
# system image, whose place and properties are checked after)
PARTS = (('android-platform-tools-linux', 'platform-tools', 'adb', reported_version),
         ('android-emulator-linux-x64', 'emulator', 'emulator', reported_version),
         ('android-system-image-31-x86_64', IMAGE_DIR, 'system.img', None),
         ('android-build-tools-linux', BUILD_TOOLS_DIR, 'aapt2', reported_version),
         ('android-platform-31', PLATFORM_DIR, 'android.jar', reported_platform),
         ('temurin-jdk-21-linux-x64', JDK_DIR, 'bin/javac', reported_jdk))


def avd_config():
    """The AVD's config.ini: the pinned image, two cores, 3 GiB of memory, no window, camera or sensors to start."""
    lines = ('AvdId=' + AVD, 'avd.ini.displayname=' + AVD, 'avd.ini.encoding=UTF-8', 'abi.type=x86_64',
             'hw.cpu.arch=x86_64', 'hw.cpu.ncore=2', 'hw.ramSize=3072', 'disk.dataPartition.size=6G',
             'hw.gpu.enabled=yes', 'hw.gpu.mode=swiftshader_indirect', 'hw.camera.back=none',
             'hw.camera.front=none', 'hw.audioInput=no', 'hw.audioOutput=no', 'hw.keyboard=yes',
             'image.sysdir.1=' + IMAGE_DIR + '/', 'tag.id=default', 'PlayStore.enabled=false')
    return '\n'.join(lines) + '\n'


def write_avd(home):
    """Write the AVD (its `<name>.ini` pointer and `<name>.avd/config.ini`) under `home`; return its directory."""
    home = Path(home)
    avd = home / (AVD + '.avd')
    avd.mkdir(parents=True, exist_ok=True)
    (avd / 'config.ini').write_text(avd_config(), encoding='utf-8', newline='\n')
    pointer = 'avd.ini.encoding=UTF-8\npath=%s\npath.rel=avd/%s.avd\ntarget=android-31\n' % (avd.as_posix(), AVD)
    (home / (AVD + '.ini')).write_text(pointer, encoding='utf-8', newline='\n')
    return avd


def install(sdk=None, download_dir=None, avd_home=None, registry=None):
    """Install the kit into `sdk` and write the AVD under `avd_home` (default `<sdk>/avd`); return (sdk, avd_home).
    `registry` maps each pin name to its entry (default: cmake/pins.json)."""
    sdk = Path(sdk or default_sdk()).resolve()
    download_dir = Path(download_dir or sdk / 'dl')
    for name, place, binary, version_of in PARTS:
        pin = pins.entry('tools', name) if registry is None else registry[name]
        pins.install_tree(pin, sdk / place, download_dir, binary, STAMP, label=name, version_of=version_of)
    image = properties(sdk / IMAGE_DIR)
    wrong = {key: image.get(key) for key, value in IMAGE.items() if image.get(key) != value}
    if wrong:
        raise pins.PinError(f'the system image reports {wrong}, not {IMAGE}')
    avd_home = Path(avd_home or sdk / 'avd').resolve()
    write_avd(avd_home)
    print(environment_lines(sdk, avd_home), end='')
    return sdk, avd_home


def environment_lines(sdk, avd_home):
    """The variables naming this kit to android-device.py and the SDK's tools, one `NAME=value` line each."""
    return f'ANDROID_HOME={sdk}\nANDROID_SDK_ROOT={sdk}\nANDROID_AVD_HOME={avd_home}\n'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--sdk', type=Path, help='the SDK root receiving the kit (default: ~/.cerid/tools/android-sdk)')
    parser.add_argument('--download-dir', type=Path, help='where the verified archives are kept (default: <sdk>/dl; '
                        'an archive already there is verified instead of downloaded)')
    parser.add_argument('--avd-home', type=Path, help='where the AVD is written (default: <sdk>/avd)')
    parser.add_argument('--github-env', type=Path,
                        help='append ANDROID_HOME, ANDROID_SDK_ROOT and ANDROID_AVD_HOME to this file')
    args = parser.parse_args(argv)
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        parser.error('the registry pins the Android test kit for Linux x64 only')
    sdk, avd_home = install(args.sdk, args.download_dir, args.avd_home)
    if args.github_env:
        with args.github_env.open('a', encoding='utf-8') as env:
            env.write(environment_lines(sdk, avd_home))
    return 0


if __name__ == '__main__':
    sys.exit(main())
