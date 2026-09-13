#pragma once

#include "asset_analysis_types.h"

class AssetSnapshotComparator final {
public:
  static AssetSnapshotDelta compare(const AccountAssetSnapshot& current,
                                    const AccountAssetSnapshot& previous);
};
