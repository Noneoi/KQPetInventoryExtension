#pragma once
#include <QJsonObject>
#include <QString>

// Chronological highest-tag order, matching official sign accumulation.
enum class PetEra {
  Unknown,
  Other,
  ShenShu,
  ChaoShen,
  ShenZhi,
  ChuanShuo,
  TianQi,
  QiYuan,
  XingJi,
  ShenYun,
  LingChu
};

// Exact official race metadata wins over cached/live display names. No fuzzy
// species/name matching is used to grant era-specific cultivation features.
PetEra resolvePetEra(const QJsonObject& pet, const QJsonObject& pets = {});
QString petEraDisplayName(PetEra era);
PetEra parsePetEraName(const QString& name);
inline bool eraHasAstrolabeBreakthrough(PetEra era) { return era == PetEra::LingChu; }

// Official era × system matrix. Unknown keeps every system calculable so leftover
// packet data remains visible, but does not grant Lingchu-only breakthrough.
struct PetEraSystems {
  bool level = true;
  bool talent = true;
  bool proficient = false;
  bool stargod = true;
  bool equipment = false;
  bool guardStone = false;
  bool learnForce = false;
  bool legendStone = false;
  bool badge = false;
  bool astrolabe = false;
  bool sacred = false;
  bool sixTalentLanes = true;
};
PetEraSystems systemsForEra(PetEra era);
bool eraHasComponent(PetEra era, const QString& key);
