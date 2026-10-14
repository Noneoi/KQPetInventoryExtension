#pragma once

#include <QJsonObject>
#include <QString>

// Validates the supported generated-catalog contract, not the correctness of
// undocumented game mechanics or the authenticity of arbitrary local files.
QString validateBattleReplayCatalog(const QJsonObject& root);
