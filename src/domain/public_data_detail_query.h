#pragma once
#include "contracts/public_data_detail.h"

namespace PublicDataDetailQuery {
PublicDataDetail resolve(const PublicDataDetailContext& context, const PublicRecordKey& key);
QString title(const PublicDataDetailContext& context, const PublicRecordKey& key);
QString kindName(const PublicRecordKey& key);
}
