"""
PlatformIO pre-build script: inject git branch and short SHA into
CROSSPOINT_VERSION for development environments.

The base version is taken from the latest git release tag (vX.Y.Z) so every
build identifies against the newest published release. platformio.ini's
[crosspoint] version is only a fallback when no release tag is reachable, and
it must never be used as the sole source: a dev base that jumps ahead of the
newest tag would make otaIsVersionNewer() compare a phantom version and block
legitimate OTA updates.

Results in a version string like:  1.1.0-dev-feat-kosync-xpath-05c6cf8
Release environments are unaffected; they set CROSSPOINT_VERSION in the ini.
"""

import configparser
import os
import re
import subprocess
import sys

# Release tags only: vX.Y.Z. Prerelease tags (v1.5.0-rc-4) and the inert
# "1.6.0rc" / "oc-*" tags never match, so they cannot become a base version.
RELEASE_VERSION_RE = re.compile(r'^v?([0-9]+)\.([0-9]+)\.([0-9]+)$')


def warn(msg):
    print(f'WARNING [git_branch.py]: {msg}', file=sys.stderr)


def run_git_value(project_dir, args, label):
    try:
        value = subprocess.check_output(
            ['git', *args],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()
        # Strip characters that would break a C string literal
        return ''.join(c for c in value if c not in '"\\')
    except FileNotFoundError:
        warn(f'git not found on PATH; {label} suffix will be "unknown"')
        return 'unknown'
    except subprocess.CalledProcessError as e:
        warn(
            f'git command failed (exit {e.returncode}): '
            f'{e.stderr.strip()}; {label} suffix will be "unknown"'
        )
        return 'unknown'
    except OSError as e:
        warn(
            f'OS error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'
    except Exception as e:  # pylint: disable=broad-exception-caught
        warn(
            f'Unexpected error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'


def get_git_branch(project_dir):
    branch = run_git_value(
        project_dir, ['rev-parse', '--abbrev-ref', 'HEAD'], 'branch'
    )
    # Detached HEAD has no branch name.
    if branch == 'HEAD':
        return 'detached'
    return branch


def get_git_short_sha(project_dir):
    return run_git_value(
        project_dir, ['rev-parse', '--short', 'HEAD'], 'short SHA'
    )


def format_version(version_tuple):
    return '.'.join(str(part) for part in version_tuple)


def version_tuple(text):
    match = RELEASE_VERSION_RE.match(text)
    if not match:
        return None
    return tuple(int(part) for part in match.groups())


def get_latest_release_version(project_dir):
    tags = run_git_value(project_dir, ['tag', '--merged', 'HEAD'], 'tags')
    if tags == 'unknown':
        return None
    versions = [
        version_tuple(tag.strip())
        for tag in tags.splitlines()
        if version_tuple(tag.strip())
    ]
    return max(versions) if versions else None


def get_ini_version(project_dir):
    ini_path = os.path.join(project_dir, 'platformio.ini')
    if not os.path.isfile(ini_path):
        warn(f'platformio.ini not found at {ini_path}; base version will be "0.0.0"')
        return None
    config = configparser.ConfigParser()
    config.read(ini_path, encoding='utf-8')
    if not config.has_option('crosspoint', 'version'):
        warn('No [crosspoint] version in platformio.ini; base version will be "0.0.0"')
        return None
    # Match the leading N.N.N so an "-rc" or per-board suffix in the ini still
    # yields a usable base.
    value = config.get('crosspoint', 'version').strip()
    match = re.match(r'^([0-9]+)\.([0-9]+)\.([0-9]+)', value)
    if not match:
        warn(f'Invalid [crosspoint] version "{value}" in platformio.ini; '
             'base version will be "0.0.0"')
        return None
    return tuple(int(part) for part in match.groups())


def get_base_version(project_dir):
    tag_version = get_latest_release_version(project_dir)
    ini_version = get_ini_version(project_dir)

    if tag_version:
        if ini_version and ini_version != tag_version:
            warn(
                f'latest release tag ({format_version(tag_version)}) differs '
                f'from [crosspoint] version in platformio.ini '
                f'({format_version(ini_version)}); using the tag as the base '
                'version so OTA version comparison stays grounded'
            )
        return format_version(tag_version)
    if ini_version:
        warn('no release tag reachable from HEAD; using platformio.ini version')
        return format_version(ini_version)
    warn('no release tag and no platformio.ini version; base version will be "0.0.0"')
    return '0.0.0'


def env_pins_version(env):
    # Release/slim envs pin "-DCROSSPOINT_VERSION=..." in their build_flags and
    # must be skipped here. Dev envs carry only a comment mentioning
    # CROSSPOINT_VERSION, so match the "-D" define instead of the bare name.
    try:
        flags = env.GetProjectConfig().get(
            'env:' + env['PIOENV'], 'build_flags', default='')
        return '-DCROSSPOINT_VERSION' in str(flags)
    except Exception:  # pylint: disable=broad-exception-caught
        # Direct-run fake env below has no GetProjectConfig; treat as dev.
        return False


def inject_version(env):
    # Development environments get the git branch + short SHA so crash
    # reports identify the exact build. Release envs set the version via
    # build_flags in platformio.ini and are unaffected.
    if env_pins_version(env):
        return

    project_dir = env['PROJECT_DIR']
    base_version = get_base_version(project_dir)
    branch = get_git_branch(project_dir)
    short_sha = get_git_short_sha(project_dir)
    version_string = f'{base_version}-dev-{branch}-{short_sha}'

    def add_version(build_env, node):
        # Only these translation units depend on Git metadata. Keep library and
        # reader objects reusable when the branch or commit changes.
        if b'CROSSPOINT_VERSION' not in node.srcnode().get_contents():
            return node
        version_env = build_env.Clone()
        version_env.Append(CPPDEFINES=[('CROSSPOINT_VERSION', f'\\"{version_string}\\"')])
        return version_env.Object(node)

    env.AddBuildMiddleware(add_version)
    print(f'CrossPoint build version: {version_string}')


# PlatformIO/SCons entry point — Import and env are SCons builtins injected at runtime.
# When run directly with Python (e.g. for validation), a lightweight fake env is used
# so the git/version logic can be exercised without a full build.
try:
    Import('env')           # noqa: F821  # type: ignore[name-defined]
    inject_version(env)     # noqa: F821  # type: ignore[name-defined]
except NameError:
    # CLI mode: print the bare dev-version string (optionally for a given
    # commit SHA) so CI can capture it with a subshell. Reuses the exact
    # derivation the build middleware uses — no second copy exists.
    if len(sys.argv) > 1 and sys.argv[1] == '--print-dev-version':
        if len(sys.argv) > 3:
            warn('usage: git_branch.py --print-dev-version [commit-sha]')
            sys.exit(1)
        _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        if len(sys.argv) == 3:
            # ^{object} forces an existence check: a plain 40-hex argument is
            # echoed back abbreviated even when the object is absent (git
            # 2.53), which would stamp a fake identity.
            _short_sha = run_git_value(
                _project_dir, ['rev-parse', '--short', '--verify',
                               sys.argv[2] + '^{object}'],
                f'SHA {sys.argv[2]}'
            )
        else:
            _short_sha = get_git_short_sha(_project_dir)
        _branch = get_git_branch(_project_dir)
        if 'unknown' in (_short_sha, _branch):
            warn('git metadata unavailable; refusing to print a version with an unknown component')
            sys.exit(1)
        print(f'{get_base_version(_project_dir)}-dev-{_branch}-{_short_sha}')
        sys.exit(0)

    class _Env(dict):
        def AddBuildMiddleware(self, _):
            # Validation mode only reports the computed version.
            return None

    _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    inject_version(_Env({'PIOENV': 'default', 'PROJECT_DIR': _project_dir}))
