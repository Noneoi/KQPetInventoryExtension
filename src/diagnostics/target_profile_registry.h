#pragma once

#include "target_profile.h"

#include <string>
#include <vector>

class TargetProfileRegistry final {
public:
  static const TargetProfile* find(const std::wstring& executableName,
                                   const std::wstring& version);
  static const std::vector<TargetProfile>& profiles();
};
