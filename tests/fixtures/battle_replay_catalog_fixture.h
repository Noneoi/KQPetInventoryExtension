#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

// TEST ONLY. This small synthetic catalog exercises storage/publication, not
// game semantics. Source records below are copied from the bundled catalog's
// provenance; they describe its original packages, NOT these synthetic rows.
// Never ship this fixture as official game data.
inline QJsonObject battleReplayCatalogFixture(const QString& marker) {
  QJsonObject root{{QStringLiteral("format"), 3},
                   {QStringLiteral("testOnlyMarker"), marker}};
  for (const auto* key : {"decompiledSourceSha256", "skillLastEffects", "buffPowerFullMap",
       "relationsBySkill", "relationsByBuff", "officialEntries", "officialEntryMentions",
       "entryMentionsByBuff", "entryMentionsBySkill", "petMechanics", "mechanismRules",
       "legendMechanics", "huanMechanics", "petMechanicsSummary", "referenceConsumers",
       "petIdentitySummary", "petDragons", "replaySupplements", "replayCultivation",
       "tableJoinSummary", "transformSkills", "referenceCoverage"})
    root.insert(QLatin1String(key), QJsonObject{});
  root.insert(QStringLiteral("tableRelations"), QJsonArray{});
  root.insert(QStringLiteral("combos"), QJsonArray{});
  root.insert(QStringLiteral("clientPresentation"), QJsonObject{{QStringLiteral("schemaVersion"), 1}});
  root.insert(QStringLiteral("pets"), QJsonObject{{QStringLiteral("900000001"), QJsonObject{
      {QStringLiteral("name"), QStringLiteral("SYNTHETIC TEST PET ") + marker},
      {QStringLiteral("identity"), QJsonObject{{QStringLiteral("status"), QStringLiteral("native")},
          {QStringLiteral("canonicalName"), QStringLiteral("SYNTHETIC TEST PET ") + marker}}},
      {QStringLiteral("nativeAttributes"), QJsonArray{QStringLiteral("900000004")}},
      {QStringLiteral("maxPower"), 100}}}});
  root.insert(QStringLiteral("skills"), QJsonObject{{QStringLiteral("900000002"), QJsonObject{
      {QStringLiteral("id"), QStringLiteral("900000002")},
      {QStringLiteral("name"), QStringLiteral("SYNTHETIC TEST SKILL ") + marker},
      {QStringLiteral("description"), QStringLiteral("Test-only description")},
      {QStringLiteral("configResolution"), QStringLiteral("direct")},
      {QStringLiteral("textSource"), QJsonObject{}}, {QStringLiteral("playbackAvailable"), false}}}});
  root.insert(QStringLiteral("buffs"), QJsonObject{{QStringLiteral("900000003"), QJsonObject{
      {QStringLiteral("id"), QStringLiteral("900000003")}, {QStringLiteral("did"), QStringLiteral("0")},
      {QStringLiteral("nm"), QStringLiteral("SYNTHETIC TEST BUFF")}, {QStringLiteral("df"), QStringLiteral("0")},
      {QStringLiteral("once"), QStringLiteral("0")}, {QStringLiteral("pos"), QStringLiteral("0")},
      {QStringLiteral("beffect"), QString{}}, {QStringLiteral("bhs"), QString{}}, {QStringLiteral("mess"), QString{}},
      {QStringLiteral("nameStatus"), QStringLiteral("named")}, {QStringLiteral("configSource"), QStringLiteral("test-only")}}}});
  root.insert(QStringLiteral("attributes"), QJsonObject{{QStringLiteral("900000004"), QJsonObject{
      {QStringLiteral("id"), QStringLiteral("900000004")}, {QStringLiteral("nm"), QStringLiteral("SYNTHETIC TEST ATTRIBUTE")},
      {QStringLiteral("pri"), QStringLiteral("1")}, {QStringLiteral("rcs"), QStringLiteral("1")}}}});
  root.insert(QStringLiteral("execution"), QJsonObject{{QStringLiteral("900000005"), QJsonObject{
      {QStringLiteral("code"), QStringLiteral("900000005")}, {QStringLiteral("nm"), QStringLiteral("SYNTHETIC TEST EXECUTION")},
      {QStringLiteral("eid"), QStringLiteral("-1")},
      {QStringLiteral("type"), QStringLiteral("1")}}}});
  root.insert(QStringLiteral("bufferExecution"), QJsonObject{{QStringLiteral("900000006"), QJsonObject{
      {QStringLiteral("bufferId"), QStringLiteral("900000006")}, {QStringLiteral("actionResId"), QStringLiteral("0")},
      {QStringLiteral("skillId"), QStringLiteral("0")}, {QStringLiteral("lightedSkillId"), QStringLiteral("0")},
      {QStringLiteral("desc"), QStringLiteral("SYNTHETIC TEST BUFFER EXECUTION")}}}});
  root.insert(QStringLiteral("petMechanicsSummary"), QJsonObject{{QStringLiteral("pets"), 1}});
  root.insert(QStringLiteral("tableJoinSummary"), QJsonObject{
      {QStringLiteral("skills"), QJsonObject{{QStringLiteral("skills"), 1}}},
      {QStringLiteral("buffs"), QJsonObject{{QStringLiteral("canonicalBuffIds"), 1},
          {QStringLiteral("flashBufferExecuteRows"), 1}, {QStringLiteral("flashComboRows"), 0}}}});
  root.insert(QStringLiteral("provenance"), QJsonDocument::fromJson(R"JSON({
    "manifestSha256":"dd1aed75066367c6383cd682533e241957f02344b2c1a8bc10a0e3a46c435d45",
    "versiondata":{"version":"20260930468207480"},
    "packages":[
      {"resource":"library/interfaces","version":"2026093021560874",
       "url":"https://aoqi.100bt.com/play/library/interfaces~2026093021560874.swf",
       "sha256":"2a7b34e20b998b40e38fea17050fde581c10ae22b6f1750632709a1dc8ad6872"},
      {"resource":"battle/battle","version":"260930623185041",
       "url":"https://aoqi.100bt.com/play/battle/battle~260930623185041.swf",
       "sha256":"e429b57fae82a8af9dd92f0d6ec7f6ff1612f743a790f9036987cd1c4ea73a75"},
      {"resource":"battleconfig/battleconfig","version":"260930372598462",
       "url":"https://aoqi.100bt.com/play/battleconfig/battleconfig~260930372598462.swf",
       "sha256":"8e912e2bb1a92ba89c5c2ee9a6abc0c0543a8092d2da84618e4800c9a086a16c"}
    ]})JSON").object());
  return root;
}

inline QByteArray battleReplayCatalogFixtureBytes(const QString& marker) {
  return QJsonDocument(battleReplayCatalogFixture(marker)).toJson(QJsonDocument::Compact);
}
