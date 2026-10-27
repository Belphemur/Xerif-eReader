import re
from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
MARKER = "/* CrossPoint wolfSSL compatibility overrides */"
# Only NO_DH / FFDHE remain here. The Ed25519 OTA-verify + FP_MAX_BITS knobs now
# live in platformio.ini [base] build_flags (they are order-independent and reach
# wolfSSL's own sources), so they no longer depend on this patch landing.
#
# NO_DH is defined by Arduino-wolfSSL's user_settings.h, but command-line -D cannot
# undefine a header macro, so we must strip it from the downloaded file. FFDHE_2048
# lets the TLS client offer/accept standard DH groups.
OVERRIDES = f"""

{MARKER}
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif
/* Arduino-wolfSSL always defines DEBUG_WOLFSSL, but SecureClient turns the
   trace on only in FREEINK_WOLFSSL_DEBUG builds. Drop it otherwise so the trace
   code and strings are not linked. Set the flag for all sources (build_flags),
   not just SecureNet, to keep the trace. */
#ifndef FREEINK_WOLFSSL_DEBUG
#undef DEBUG_WOLFSSL
#endif
"""

# wolfSSL 5.8.x emits three diagnostics in every translation unit that pulls in
# settings.h, drowning real warnings in build logs. None can be silenced with a
# macro from user_settings.h:
#  1. "#warning wolfssl/options.h included in compiled wolfssl library object."
#     wolfcrypt/settings.h raises it when BUILDING_WOLFSSL (defined in
#     libwolfssl_sources.h, so only wolfSSL's own sources see it) meets
#     WOLFSSL_OPTIONS_H from our command line — and the check runs BEFORE
#     settings.h includes user_settings.h. It only targets builds configured via
#     wolfssl/options.h, which Arduino-wolfSSL does not use.
#  2. '"WOLFSSL_TLS13" redefined' — user_settings.h defines it with an empty body
#     while -DWOLFSSL_TLS13 on the command line defines it as 1. Guarding the
#     header defines keeps the command-line value; TLS 1.3 stays enabled.
#  3. Inside #if defined(__xtensa__) (ESP32-S3): "Contact wolfSSL support for a
#     fast implementation that is constant time" — an unconditional vendor nag
#     with no escape macro in 5.8.4. Only the #warning is dropped; the
#     CURVE25519/ED25519/CURVE448/ED448 _SMALL fallbacks it follows stay active.
# Other settings.h #warnings do not fire in this configuration, so they are left
# untouched.
VENDOR_WARNINGS = (
    "wolfssl/options.h included in compiled wolfssl library object",
    "Contact wolfSSL support",
)
TLS13_DEFINE_RE = re.compile(r"^(\s*)#define\s+WOLFSSL_TLS13\s*$")
TLS13_UNDEF_RE = re.compile(r"^\s*#undef\s+WOLFSSL_TLS13\s*$")
TLS13_GUARD_RE = re.compile(r"^\s*#ifndef\s+WOLFSSL_TLS13\s*$")


def guard_tls13_defines(text: str) -> str:
    """Wrap bare #define WOLFSSL_TLS13 in #ifndef so the command-line -D wins."""
    out = []
    guarded = 0
    for line in text.splitlines(keepends=True):
        body = line.rstrip("\r\n")
        # Skip the define right after #undef (already safe) and skip our own
        # guard on re-runs (idempotency).
        handled_by = out[-1].rstrip("\r\n") if out else ""
        if TLS13_UNDEF_RE.match(handled_by) or TLS13_GUARD_RE.match(handled_by):
            out.append(line)
            continue
        match = TLS13_DEFINE_RE.match(body)
        if match:
            out.append(f"{match.group(1)}#ifndef WOLFSSL_TLS13\n")
            out.append(line)
            out.append(f"{match.group(1)}#endif\n")
            guarded += 1
        else:
            out.append(line)
    if guarded:
        print(f"Guarded {guarded} WOLFSSL_TLS13 define(s)")
    return "".join(out)


def strip_vendor_warnings(text: str) -> str:
    """Drop the two vendor #warning directives (and any '\\' continuations)."""
    out = []
    removed = 0
    in_continuation = False
    for line in text.splitlines(keepends=True):
        if in_continuation:
            in_continuation = line.rstrip("\r\n").endswith("\\")
            removed += 1
            continue
        stripped = line.strip()
        if stripped.startswith("#warning") and any(w in stripped for w in VENDOR_WARNINGS):
            removed += 1
            in_continuation = line.rstrip("\r\n").endswith("\\")
            continue
        out.append(line)
    if removed:
        print(f"Removed {removed} vendor #warning line(s)")
    return "".join(out)


def patch_user_settings(path: Path) -> None:
    text = path.read_text()
    if MARKER in text:
        text = text.split(MARKER, 1)[0].rstrip()
    text = guard_tls13_defines(text)
    path.write_text(text + OVERRIDES + "\n")
    print(f"Patched wolfSSL settings: {path.relative_to(PROJECT_DIR)}")


def patch_settings_h(path: Path) -> None:
    text = path.read_text()
    stripped = strip_vendor_warnings(text)
    if stripped != text:
        path.write_text(stripped)
        print(f"Silenced vendor #warning(s): {path.relative_to(PROJECT_DIR)}")


# wolfSSL resolves two ways here, and the installed folder name differs:
#  * registry pin (wolfssl/Arduino-wolfSSL @ 5.7.2) -> .pio/libdeps/<env>/Arduino-wolfSSL/
#  * GitHub tag (https://github.com/wolfSSL/Arduino-wolfSSL.git#5.8.4) -> .pio/libdeps/<env>/wolfssl/
# The library's internal name flipped to "wolfssl" at 5.8.x.
#
# pathlib.Path.glob does NOT support brace expansion ({a,b}), so we enumerate
# both folder names explicitly.
#
# Runs as a `post:` script so it executes AFTER PlatformIO has installed the
# wolfSSL dependency (a `pre:` script would run before install on a clean/CI
# checkout, find no user_settings.h, and silently skip the patch).
WOLFSSL_FOLDERS = ("Arduino-wolfSSL", "wolfssl")
patched = 0
for folder in WOLFSSL_FOLDERS:
    for settings in PROJECT_DIR.glob(f".pio/libdeps/*/{folder}/src/user_settings.h"):
        patch_user_settings(settings)
        patched += 1
    for settings_h in PROJECT_DIR.glob(f".pio/libdeps/*/{folder}/src/wolfssl/wolfcrypt/settings.h"):
        patch_settings_h(settings_h)
if patched == 0:
    print(f"Warning: no wolfSSL user_settings.h found under {PROJECT_DIR}/.pio/libdeps/*/ "
          "(have you run a build yet?)")
