#include "target_profile_registry.h"

const std::vector<TargetProfile>& TargetProfileRegistry::profiles() {
  return kqpet::compatibility::registeredProfiles();
}

const TargetProfile* TargetProfileRegistry::find(const std::wstring& executableName,
                                                const std::wstring& version) {
  return kqpet::compatibility::findProfile(executableName, version);
}
