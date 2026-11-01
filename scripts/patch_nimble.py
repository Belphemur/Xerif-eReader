"""
PlatformIO pre-build script: make NimBLE-Arduino's HCI event-pool counts
sdkconfig-respecting.

Copilot review finding on PR #275 (verified with a toolchain preprocessor probe):
NimBLE-Arduino 2.5.1's src/nimconfig.h defines CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT
and CONFIG_BT_NIMBLE_TRANSPORT_EVT_DISCARD_COUNT unconditionally (30 / 8) AFTER
including sdkconfig.h, so transport.c allocates 30 + 8 event-pool blocks whatever
the rebuilt core sdkconfig holds. The CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT=8 and
EVT_DISCARD_COUNT=4 knobs in platformio.ini [ble_page_turner] never reached the
pools, and the 1,872 B of claimed savings did not exist.

The splice wraps both definitions in #ifndef guards so a generated sdkconfig
value wins when present (every CrossPoint build includes sdkconfig.h — the same
guard NimBLE-Arduino already uses for CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT
twelve lines above); builds with no sdkconfig keep NimBLE's 30/8 defaults.

Idempotent by content, fail-closed on an unexpected upstream shape (the build
aborts rather than silently compiling the untrimmed pools), mirroring
patch_wolfssl/patch_arduino_ble_controller.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os

OLD = """\
/** @brief Number of high priority HCI event buffers */
#define CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT 30

/** @brief Number of low priority HCI event buffers */
#define CONFIG_BT_NIMBLE_TRANSPORT_EVT_DISCARD_COUNT 8
"""

NEW = """\
/** @brief Number of high priority HCI event buffers
 *  (guarded: the generated sdkconfig.h included above owns these knobs,
 *  untrimmed upstream blocks the CONFIG_BT_NIMBLE_TRANSPORT_EVT_* settings) */
#ifndef CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT
#define CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT 30
#endif

/** @brief Number of low priority HCI event buffers */
#ifndef CONFIG_BT_NIMBLE_TRANSPORT_EVT_DISCARD_COUNT
#define CONFIG_BT_NIMBLE_TRANSPORT_EVT_DISCARD_COUNT 8
#endif
"""


def patch_nimble(_env):
    libdeps_dir = os.path.join(_env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    patched_any = False
    for env_dir in sorted(os.listdir(libdeps_dir)):
        nimconfig = os.path.join(
            libdeps_dir, env_dir, "NimBLE-Arduino", "src", "nimconfig.h")
        if not os.path.isfile(nimconfig):
            continue
        text = open(nimconfig, encoding="utf-8", newline="").read()
        if "#ifndef CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT" in text:
            continue  # already patched
        if OLD not in text:
            raise RuntimeError(
                "NimBLE-Arduino nimconfig.h does not match the expected "
                "TRANSPORT_EVT_COUNT shape (upstream may have changed); "
                "patch_nimble.py refuses to guess -- aborting build (" +
                nimconfig + ")")
        open(nimconfig, "w", encoding="utf-8", newline="").write(
            text.replace(OLD, NEW, 1))
        print(f"NimBLE event-pool counts made sdkconfig-respecting: {env_dir}")
    # A build whose libdeps are not installed yet (fresh checkout) also skips
    # silently, like patch_jpegdec; the patched file is on the libdep itself,
    # so once installed, the next build patches it and the compile gate in
    # platformio.ini values only sees the patched header.


try:
    Import("env")  # noqa: F821
except NameError:
    pass
else:
    patch_nimble(env)
