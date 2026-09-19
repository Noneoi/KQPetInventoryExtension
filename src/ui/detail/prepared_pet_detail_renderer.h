#pragma once
#include "contracts/pet_detail_types.h"
#include <QUrl>

namespace PreparedPetDetailRenderer {
QString render(const PreparedPetDetailHandle& detail, const QString& imageUrl,
               bool compact = false, bool fetching = false);
QString waiting(const QString& name, const QString& error = {});
bool pageLink(const QUrl& url, DetailSection* section, int* pageIndex);
// A related pet's own instance, for opening its detail beside this one.
bool petLink(const QUrl& url, qint64* instanceId);
}
