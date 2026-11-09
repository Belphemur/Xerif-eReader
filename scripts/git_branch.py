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

The stamp must never carry the word "detached": CI checkouts are detached
(actions/checkout pins GITHUB_SHA), so the branch is provided by the workflow
via the CROSSPOINT_GIT_BRANCH environment variable, or resolved from the
commit itself (branches pointing at it); when neither works the component
degrades to "unknown" with the same fail-closed rule as the other git fields.
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


def resolve_branch_from_commit(project_dir):
    """Name a branch pointing at HEAD when the checkout is detached, or fail.

    CI checkouts are always detached, so the commit itself is the only clue.
    for-each-ref is used instead of `git branch --points-at`, which prints an
    unwelcome "(HEAD detached at ...)" marker line on the first row when the
    checkout is detached. Local branches first, then remotes; when no ref
    resolves the caller's "unknown" degradation applies — never a
    "detached" label.
    """
    refs = run_git_value(
        project_dir,
        ['for-each-ref', '--points-at', 'HEAD',
         '--format', '%(refname)', 'refs/heads', 'refs/remotes'],
        'refs'
    )
    if refs != 'unknown':
        local, remotes = [], []
        for line in refs.splitlines():
            ref = line.strip()
            if ref.startswith('refs/heads/'):
                local.append(ref[len('refs/heads/'):])
            elif ref.startswith('refs/remotes/'):
                candidate = ref[len('refs/remotes/'):]
                # 'origin/HEAD' is HEAD's symref, not a resolvable name.
                if candidate and not candidate.endswith('/HEAD'):
                    remotes.append(candidate)
        if local:
            return local[0]
        # Prefer the main remote so a mirror clone with stray remotes does
        # not rename the stamped branch.
        for candidate in remotes:
            if candidate.startswith('origin/'):
                return candidate[len('origin/'):]
        if remotes:
            return remotes[0]
    warn('detached HEAD resolved to no branch; branch suffix will be "unknown"')
    return 'unknown'


def get_git_branch(project_dir):
    # Explicit CI override first: the workflow knows the branch it built
    # (actions/checkout pins GITHUB_SHA, so the checkout itself has none).
    env_value = ''.join(c for c in os.environ.get('CROSSPOINT_GIT_BRANCH', '')
                        if c not in '"\\')
    if env_value.strip():
        return env_value.strip()
    # symbolic-ref is the stable detached-HEAD check: it exits non-zero with
    # no output when HEAD is detached, while `rev-parse --abbrev-ref` prints
    # version-dependent text ("HEAD" on older git, "(HEAD detached at ...)"
    # on 2.53+).
    try:
        proc = subprocess.run(
            ['git', 'symbolic-ref', '-q', '--short', 'HEAD'],
            text=True, capture_output=True, cwd=project_dir
        )
    except OSError:
        warn('git not found on PATH; branch suffix will be "unknown"')
        return 'unknown'
    if proc.returncode == 0:
        branch = ''.join(c for c in proc.stdout.strip() if c not in '"\\')
        return branch if branch else 'unknown'
    # Detached HEAD: resolve the branch from the commit instead of stamping
    # a "detached" label.
    return resolve_branch_from_commit(project_dir)


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
    # commit SHA, optionally with an explicit branch override) so CI can
    # capture it with a subshell. Reuses the exact derivation the build
    # middleware uses — no second copy exists.
    if len(sys.argv) > 1 and sys.argv[1] == '--print-dev-version':
        # Usage: --print-dev-version [commit-sha] [--branch NAME]. --branch
        # replaces the checkout's branch in the derivation (the PR publish
        # lane checks out develop but stamps a PR head branch); without it
        # the behavior is unchanged.
        _sha_arg = None
        _branch_arg = None
        _rest = sys.argv[2:]
        _i = 0
        while _i < len(_rest):
            if _rest[_i] == '--branch':
                if _branch_arg is not None or _i + 1 >= len(_rest) or not _rest[_i + 1]:
                    warn('usage: git_branch.py --print-dev-version [commit-sha] [--branch NAME]')
                    sys.exit(1)
                _branch_arg = _rest[_i + 1]
                _i += 2
            elif _sha_arg is None:
                _sha_arg = _rest[_i]
                _i += 1
            else:
                warn('usage: git_branch.py --print-dev-version [commit-sha] [--branch NAME]')
                sys.exit(1)
        _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        if _sha_arg is not None:
            # ^{object} forces an existence check: a plain 40-hex argument is
            # echoed back abbreviated even when the object is absent (git
            # 2.53), which would stamp a fake identity.
            _short_sha = run_git_value(
                _project_dir, ['rev-parse', '--short', '--verify',
                               _sha_arg + '^{object}'],
                f'SHA {_sha_arg}'
            )
        else:
            _short_sha = get_git_short_sha(_project_dir)
        if _branch_arg is not None:
            # An explicit override is the caller's own value; only the
            # git-derived branch can turn out "unknown".
            _branch = _branch_arg
        else:
            _branch = get_git_branch(_project_dir)
        if 'unknown' in _short_sha or (_branch_arg is None and 'unknown' in _branch):
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
