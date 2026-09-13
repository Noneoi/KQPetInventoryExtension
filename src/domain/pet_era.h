#pragma once
#include <QJsonObject>

enum class PetEra { Unknown, Other, QiYuan, XingJi, ShenYun, LingChu };

// Exact official race metadata wins over cached/live display names. No fuzzy
// species/name matching is used to grant era-specific cultivation features.
PetEra resolvePetEra(const QJsonObject& pet, const QJsonObject& pets = {});
inline bool eraHasAstrolabeBreakthrough(PetEra era) { return era == PetEra::LingChu; }
