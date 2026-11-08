#pragma once

#include <cstdint>

// Install-time slot auto-selection (design §15/§16). The install SOURCE picks
// the target slot; the user is never asked. The routing decision points are
// pure functions so the host suite can pin the §16 matrix rows without
// device hardware.
//
// Slot roles are fixed by position (§3): app0 = stable, app1 = dev. The
// running slot is never a write target: the dance path stages the asset on SD
// (§16 swap worker), records a pending install, restarts into the OTHER slot
// and lets that boot hook complete the write of the TARGET slot.

namespace ota_install_routing {

// §15 source rule. The gh_release flow targets the stable slot; the dev
// channel targets the dev slot; a normal-mode SD install is classified by the
// asset grammar.
enum class InstallSource : uint8_t {
  GhRelease,   // /releases/latest manifest → stable slot
  DevChannel,  // dev prerelease manifest → dev slot
  SdFile,      // SD file → classified by name (§15)
};

// §15 SD classification: prefix-based on the exact asset grammar the CI lanes
// emit. `xerif-dev-` and `xerif-pr-` name the dev slot; every other .bin
// (including `xerif-<version>-<device>.bin` and unknown prefixes) names the
// stable slot. Dev/pr prefixes are checked FIRST (xerif-dev- also matches
// xerif-).
enum class SdFileClass : uint8_t { Dev, Pr, Stable };

// §15 target slot: app0 = stable, app1 = dev.
enum class InstallTarget : uint8_t { Stable, Dev };

// §16 routing matrix. The running slot is never a write target (§3): the
// direct write targets the OTHER slot, the dance targets the running slot.
// A dance requires the swap worker (the OTHER slot) to hold a marker-capable
// image; when it is empty or lacks the marker the install is REFUSED — never
// fall back to writing the wrong-role slot.
enum class InstallRoute : uint8_t {
  DirectWrite,     // target != running slot: write the target slot directly
  Dance,           // target == running slot: stage on SD, restart into the
                   // swap worker, let the boot hook complete the write
  RefuseNoWorker,  // target == running slot but the swap worker is unavailable
};

// Prefix-based SD classification (§15). The exact CI asset grammar:
// `xerif-dev-` → Dev, `xerif-pr-` → Pr, everything else → Stable.
SdFileClass classifySdFile(const char* name);

// The §15 source rule for a normal-mode SD install: dev/pr-classified names
// the dev slot; every other .bin names the stable slot.
InstallTarget targetForSdFile(SdFileClass fileClass);

// The §16 routing matrix: the running slot is never a write target.
//   runningIsStable == targetIsStable → Dance / RefuseNoWorker
//   runningIsStable != targetIsStable → DirectWrite
InstallRoute routeForInstall(bool runningIsStable, bool targetIsStable, bool workerAvailable);

}  // namespace ota_install_routing
