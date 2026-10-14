#include "domain/pet_relation_rules.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <cstdio>

namespace {
bool check(bool valid, const char* message) {
  if (!valid) std::fprintf(stderr, "FAIL: %s\n", message);
  return valid;
}
QJsonObject identity(const char* status, int base, const char* name) {
  return {{"identity", QJsonObject{{"status", status}, {"baseRaceId", QString::number(base)}, {"canonicalName", name}}}};
}
QJsonObject row(int index, QJsonArray targets, int type = 16) {
  return {{"rowId", index}, {"sourceRaceIds", QJsonArray{"100"}}, {"targetRaceIds", targets},
    {"type", type}, {"expansionStatus", "resolved"}, {"isShow", false}, {"table", "hide"}};
}
QJsonObject fixture() {
  const QJsonObject pets{{"201", identity("native", 201, "synthetic A")}, {"202", identity("skin", 201, "synthetic A")},
    {"203", QJsonObject{{"name", "same unverified name"}}}, {"204", QJsonObject{{"name", "same unverified name"}}},
    {"205", identity("native", 205, "synthetic B")}};
  auto publicRow = row(3, {"205"}); publicRow["isShow"] = true;
  const QJsonArray rows{row(0, {"201"}, 4), row(1, {"201", "202"}), row(2, {"203", "204"}, 8), publicRow};
  const QJsonObject group{{"rows", rows}, {"byPair", QJsonObject{{"100_201", 1}, {"100_202", 1},
    {"100_203", 2}, {"100_204", 2}, {"100_205", 3}}}, {"bySourceRaceId", QJsonObject{{"100", QJsonArray{0, 1, 2, 3}}}},
    {"hiddenSourceRaceIds", QJsonArray{"100"}}};
  const QJsonObject mechanics{{"201", QJsonObject{{"sign", "神职"}}}, {"202", QJsonObject{{"sign", "神职,皮肤"}}},
    {"204", QJsonObject{{"sign", "神职"}}}, {"205", QJsonObject{{"sign", "神职"}}}};
  return {{"pets", pets}, {"petMechanics", mechanics}, {"replaySupplements", QJsonObject{{"summon", group}, {"carry", group}}}};
}
int raceCount(const PetRelationOptions& options) {
  int result = 0; for (const auto& family : options.families) result += family.raceIds.size(); return result;
}
bool verifyActual(const QString& path) {
  QFile file(path);
  if (!check(file.open(QIODevice::ReadOnly), "actual decoded catalog unavailable")) return false;
  QJsonParseError error; const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  if (!check(error.error == QJsonParseError::NoError && document.isObject(), "actual decoded catalog is not JSON")) return false;
  const auto summon = petRelationOptions(document.object(), 7532, PetRelationKind::Summon);
  const auto carry = petRelationOptions(document.object(), 6769, PetRelationKind::Carry);
  const QVector<int> expectedSummon{5931, 5987, 6312, 6362, 6465, 6567, 6658, 6790, 6874, 6924, 7257, 7396, 7397};
  const QVector<int> expectedCarry{5683, 6761, 7346};
  QVector<int> actualSummon, actualCarry;
  for (const auto& family : summon.families) actualSummon.append(family.originalRaceId);
  for (const auto& family : carry.families) actualCarry.append(family.originalRaceId);
  if (!check(summon.available && summon.families.size() == 13 && raceCount(summon) == 66 && actualSummon == expectedSummon,
      "2026-10-14 source 7532 must expose 66 exact targets grouped into 13 verified originals") ||
      !check(carry.available && carry.families.size() == 3 && raceCount(carry) == 13 && actualCarry == expectedCarry,
      "2026-10-14 source 6769 must expose 13 exact targets grouped into 3 verified originals")) return false;
  for (const auto& family : carry.families)
    if (!check(family.carryType == 16 && !family.originalName.isEmpty() && family.listedWithoutOwnership,
        "actual carried family lost official type/name/non-owned visibility")) return false;
  for (const auto& family : summon.families)
    if (!check(family.listedWithoutOwnership, "actual non-owned summon family disappeared")) return false;
  const auto bytes = QJsonDocument(QJsonObject{{"passed", true}, {"summonFamilies", 13}, {"summonRaces", 66},
    {"carryFamilies", 3}, {"carryRaces", 13}}).toJson(QJsonDocument::Compact);
  std::printf("%s\n", bytes.constData()); return true;
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  const auto root = fixture();
  const auto carry = petRelationOptions(root, 100, PetRelationKind::Carry);
  const auto summon = petRelationOptions(root, 100, PetRelationKind::Summon);
  if (!check(carry.available && carry.families.size() == 4 && raceCount(carry) == 5,
      "complete exact target union missing") ||
      !check(carry.families[0].originalRaceId == 201 && carry.families[0].raceIds == QVector<int>{201, 202} &&
        carry.families[0].carryType == 16 && carry.families[0].originalName == QStringLiteral("synthetic A"),
        "final pair override or verified original grouping lost") ||
      !check(carry.families[1].originalRaceId == 203 && carry.families[2].originalRaceId == 204 &&
        carry.families[1].originalName.isEmpty() && carry.families[2].originalName.isEmpty(),
        "unverified same names merged or became official originals") ||
      !check(summon.available && raceCount(summon) == 5 && summon.families.size() == 4,
        "complete official pairs were removed because isShow=false or source is hidden") ||
      !check(!summon.families[0].listedWithoutOwnership && summon.families[3].listedWithoutOwnership &&
        carry.families[0].listedWithoutOwnership && !carry.families[1].listedWithoutOwnership,
        "non-owned selection ignored isShow or inferred non-skin from a name/missing sign") ||
      !check(petRelationOptions(root, 999, PetRelationKind::Carry).available &&
        petRelationOptions(root, 999, PetRelationKind::Carry).families.isEmpty(), "valid no-pair query became unavailable") ||
      !check(!petRelationOptions({}, 100, PetRelationKind::Carry).available &&
        !petRelationOptions(root, 0, PetRelationKind::Carry).available, "missing/invalid public inputs were declared complete")) return 1;

  auto corrupt = root; auto supplements = root["replaySupplements"].toObject(); auto group = supplements["carry"].toObject();
  auto pairs = group["byPair"].toObject(); pairs["100_201"] = 1.5; group["byPair"] = pairs;
  supplements["carry"] = group; corrupt["replaySupplements"] = supplements;
  if (!check(!petRelationOptions(corrupt, 100, PetRelationKind::Carry).available, "fractional final row index accepted")) return 2;
  pairs["100_201"] = 99; group["byPair"] = pairs; supplements["carry"] = group; corrupt["replaySupplements"] = supplements;
  if (!check(!petRelationOptions(corrupt, 100, PetRelationKind::Carry).available, "out-of-range final row index accepted")) return 3;
  pairs["100_201"] = 3; group["byPair"] = pairs; supplements["carry"] = group; corrupt["replaySupplements"] = supplements;
  if (!check(!petRelationOptions(corrupt, 100, PetRelationKind::Carry).available, "final pair and actual row targets disagree")) return 4;

  auto uncertain = root; auto pets = root["pets"].toObject(); pets["202"] = identity("unresolved", 201, "synthetic A");
  uncertain["pets"] = pets;
  const auto unresolved = petRelationOptions(uncertain, 100, PetRelationKind::Carry);
  if (!check(unresolved.available && unresolved.families.size() == 5 && raceCount(unresolved) == 5,
      "unresolved original identity discarded an eligible target or guessed its family")) return 5;
  pets["202"] = identity("skin", 201, "wrong canonical name"); uncertain["pets"] = pets;
  if (!check(petRelationOptions(uncertain, 100, PetRelationKind::Carry).families.size() == 5,
      "inconsistent skin identity merged into a native family")) return 6;

  auto skinOnly = root; auto skinSupplements = root["replaySupplements"].toObject(); auto skinGroup = skinSupplements["summon"].toObject();
  auto skinRows = skinGroup["rows"].toArray(); auto skinRow = skinRows[1].toObject(); skinRow["isShow"] = true; skinRows[1] = skinRow;
  skinGroup["rows"] = skinRows; skinGroup["byPair"] = QJsonObject{{"100_202", 1}};
  skinSupplements["summon"] = skinGroup; skinOnly["replaySupplements"] = skinSupplements;
  const auto skinOption = petRelationOptions(skinOnly, 100, PetRelationKind::Summon);
  if (!check(skinOption.available && skinOption.families.size() == 1 && skinOption.families[0].raceIds == QVector<int>{202} &&
      !skinOption.families[0].listedWithoutOwnership, "skin-only eligible pair invented an unpaired native candidate")) return 8;

  const auto arguments = app.arguments();
  if (arguments.contains(QStringLiteral("--catalog-json"))) {
    const int index = arguments.indexOf(QStringLiteral("--catalog-json"));
    if (index + 1 >= arguments.size() || !verifyActual(arguments[index + 1])) return 7;
  }
  return 0;
}
