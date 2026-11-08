#include "OtaInstallRouting.h"

#include <cstring>

namespace ota_install_routing {
namespace {
// The exact CI asset grammar the CI lanes emit. `xerif-dev-` and `xerif-pr-`
// name the dev slot; every other .bin names the stable slot. Prefix match is
// case-sensitive: the CI lanes emit lowercase names.
constexpr char kDevPrefix[] = "xerif-dev-";
constexpr char kPrPrefix[] = "xerif-pr-";
constexpr size_t kDevPrefixLen = sizeof(kDevPrefix) - 1;
constexpr size_t kPrPrefixLen = sizeof(kPrPrefix) - 1;
}  // namespace

SdFileClass classifySdFile(const char* name) {
  if (!name) return SdFileClass::Stable;
  // Dev/pr prefixes are checked FIRST (xerif-dev- also matches xerif-).
  if (strncmp(name, kDevPrefix, kDevPrefixLen) == 0) return SdFileClass::Dev;
  if (strncmp(name, kPrPrefix, kPrPrefixLen) == 0) return SdFileClass::Pr;
  return SdFileClass::Stable;
}

InstallTarget targetForSdFile(const SdFileClass fileClass) {
  return fileClass == SdFileClass::Stable ? InstallTarget::Stable : InstallTarget::Dev;
}

InstallRoute routeForInstall(const bool runningIsStable, const bool targetIsStable,
                             const bool workerAvailable) {
  // The running slot is never a write target (§3): the direct write targets
  // the OTHER slot, the dance targets the running slot.
  if (runningIsStable != targetIsStable) return InstallRoute::DirectWrite;
  // The dance: the swap worker (the other slot) completes the write of the
  // target slot. When the worker is unavailable (empty or lacks the resume
  // marker), refuse — never fall back to writing the wrong-role slot.
  if (!workerAvailable) return InstallRoute::RefuseNoWorker;
  return InstallRoute::Dance;
}

}  // namespace ota_install_routing
