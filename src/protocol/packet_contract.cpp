#include "packet_contract.h"
#include "domain/checked_json_numbers.h"
#include "domain/pet_move_policy.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>

#include <cmath>

namespace {
bool appendObjects(const QJsonValue& value, QList<QJsonObject>* pets, int depth) {
  if (depth > 16) return false;
  if (value.isObject()) { pets->append(value.toObject()); return true; }
  if (!value.isArray()) return false;
  for (const QJsonValue& item : value.toArray())
    if (!appendObjects(item, pets, depth + 1)) return false;
  return true;
}
}  // namespace

namespace PacketContracts {
const QList<PacketContract>& all() {
  static const QList<PacketContract> contracts = {
      {QStringLiteral("21_1"), {}, {}, false, PacketAccess::Passive,
       PacketSnapshotSemantics::None, PacketCorrelationStrength::PassiveObserved,
       QStringLiteral("info.n: nonempty string or checked integer"), QStringLiteral("tests/fixtures/login")},
      {QStringLiteral("2_1_10"), QStringLiteral("PJXExtension"), QStringLiteral("{}"), true, PacketAccess::Read,
       PacketSnapshotSemantics::Complete, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("pl: object/list; pps: optional sequence array/string; ppc: optional capacity"),
       QStringLiteral("tests/fixtures/backpack/real_v1_sanitized.json")},
      {QStringLiteral("2_1_S"), QStringLiteral("PJXExtension"), QStringLiteral("null"), true, PacketAccess::Read,
       PacketSnapshotSemantics::PresentGroupsOnly, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("ns/rb/es: each present group must be a valid object/list; omission retains old group"),
       QStringLiteral("tests/fixtures/warehouse/real_v1_sanitized.json")},
      {QStringLiteral("2_1_R"), QStringLiteral("PJXExtension"), QStringLiteral("{pi: positive integer}"), true, PacketAccess::Read,
       PacketSnapshotSemantics::None, PacketCorrelationStrength::EntityCorrelated,
       QStringLiteral("p: object with positive id, r/ri and checked nonnegative lv"),
       QStringLiteral("tests/fixtures/detail")},
      {QStringLiteral("2_1_11"), QStringLiteral("PJXExtension"), QStringLiteral("{pps: sequence, ppt: pack type}"), true, PacketAccess::Write,
       PacketSnapshotSemantics::None, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("r: explicit checked status, or valid complete pl+pps observation"),
       QStringLiteral("tests/move_controller_smoke.cpp; no server correlation token proven")},
      {QStringLiteral("2_2_10"), QStringLiteral("PJXExtension"), QStringLiteral("{}"), true, PacketAccess::Read,
       PacketSnapshotSemantics::Complete, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("fis.cfid/fl plus optional planN.cpid/fl"), QStringLiteral("tests/fixtures/formation/real_v1_sanitized.json")},
      {QStringLiteral("1008_20260313_es_0"), QStringLiteral("TimelinessActExtension"), QStringLiteral("{}"), true, PacketAccess::Read,
       PacketSnapshotSemantics::PresentGroupsOnly, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("activity count fields validated by ShopExchangeController"), QStringLiteral("tests/fixtures/shop/real_v1_sanitized.json")},
      {QStringLiteral("1008_20260313_es_2"), QStringLiteral("TimelinessActExtension"), QStringLiteral("{i: positive integer}"), true, PacketAccess::Read,
       PacketSnapshotSemantics::Complete, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("activity-local bi counters and optional cn tokens; serialize shops sharing this command"), QStringLiteral("docs/activity-exchange-observations.md")},
      {QStringLiteral("1019_0"), QStringLiteral("SimpleActExtension"), QStringLiteral("{ai: positive integer}"), true, PacketAccess::Read,
       PacketSnapshotSemantics::Complete, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("ClientSA get-info; response ai must match request ai"), QStringLiteral("docs/activity-exchange-observations.md")},
      {QStringLiteral("1008_20251231_cvgv2_0"), QStringLiteral("TimelinessActExtension"), QStringLiteral("null"), true, PacketAccess::Read,
       PacketSnapshotSemantics::Complete, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("li item counters and c V-coin balance"), QStringLiteral("docs/activity-exchange-observations.md")},
      {QStringLiteral("1039_3_0"), QStringLiteral("null"), QStringLiteral("null"), true, PacketAccess::Read,
       PacketSnapshotSemantics::Complete, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("return-player lv configuration tier"), QStringLiteral("docs/activity-exchange-observations.md")},
      {QStringLiteral("1039_4_0"), QStringLiteral("null"), QStringLiteral("null"), true, PacketAccess::Read,
       PacketSnapshotSemantics::Complete, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("return shop bt item counters"), QStringLiteral("docs/activity-exchange-observations.md")},
      {QStringLiteral("1008_20250627_hdc_0"), QStringLiteral("TimelinessActExtension"), QStringLiteral("null"), true, PacketAccess::Read,
       PacketSnapshotSemantics::Complete, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("p array: item i and used counter l"), QStringLiteral("docs/activity-exchange-observations.md")},
      {QStringLiteral("3_11"), QStringLiteral("MaterialExtension"), QStringLiteral("{}"), true, PacketAccess::Read,
       PacketSnapshotSemantics::PresentGroupsOnly, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("numeric material-group keys: list of checked i/n items"), QStringLiteral("tests/fixtures/shop/material_real_v1_sanitized.json")},
      {QStringLiteral("2_32_0"), QStringLiteral("PJXExtension"), QStringLiteral("null"), true, PacketAccess::Read,
       PacketSnapshotSemantics::Complete, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("eps: warehouse packs {dpi,exp,lvl,eqn}; excludes equipped pet slots"),
       QStringLiteral("docs/source-beast-inventory-protocol.md")},
      {QStringLiteral("1015_2A"), QStringLiteral("LeagueExtension"), {}, false, PacketAccess::Passive,
       PacketSnapshotSemantics::None, PacketCorrelationStrength::PassiveObserved,
       QStringLiteral("infos.UnionMemberInfo.lCToken: checked count"), QStringLiteral("docs/AI工程交接文档.md")},
      {QStringLiteral("1008_20170623_dt_0"), QStringLiteral("TimelinessActExtension"), QStringLiteral("null"), true, PacketAccess::Read,
       PacketSnapshotSemantics::None, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("daily activity fields validated by RoutineOverviewController"), QStringLiteral("tests/fixtures/routine/real_v1_sanitized.json")},
      {QStringLiteral("1037_0"), QStringLiteral("null"), QStringLiteral("{ids: lights}"), true, PacketAccess::Read,
       PacketSnapshotSemantics::Complete, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("red-point fields validated by RoutineOverviewController"), QStringLiteral("tests/fixtures/routine/real_v1_sanitized.json")},
      {QStringLiteral("1008_20220603_swa_0_0"), QStringLiteral("TimelinessActExtension"), QStringLiteral("null"), true, PacketAccess::Read,
       PacketSnapshotSemantics::None, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("opportunity fields validated by RoutineOverviewController"), QStringLiteral("tests/fixtures/routine/real_v1_sanitized.json")},
      {QStringLiteral("1008_20190531_gbt_1"), QStringLiteral("TimelinessActExtension"), QStringLiteral("null"), true, PacketAccess::Read,
       PacketSnapshotSemantics::None, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("opportunity fields validated by RoutineOverviewController"), QStringLiteral("docs/AI工程交接文档.md")},
      {QStringLiteral("2_36_1"), QStringLiteral("PJXExtension"), QStringLiteral("null"), true, PacketAccess::Read,
       PacketSnapshotSemantics::None, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("opportunity fields validated by RoutineOverviewController"), QStringLiteral("docs/AI工程交接文档.md")},
      {QStringLiteral("110_123_0"), QStringLiteral("XiaoMoEvolveExtension"), QStringLiteral("null"), true, PacketAccess::Read,
       PacketSnapshotSemantics::None, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("opportunity fields validated by RoutineOverviewController"), QStringLiteral("docs/AI工程交接文档.md")},
      {QStringLiteral("1008_20260522_nf_0"), QStringLiteral("TimelinessActExtension"), QStringLiteral("{un: -1}"), true, PacketAccess::Read,
       PacketSnapshotSemantics::None, PacketCorrelationStrength::CommandObserved,
       QStringLiteral("opportunity fields validated by RoutineOverviewController"), QStringLiteral("docs/AI工程交接文档.md")},
      {QStringLiteral("16_24_A"), {}, {}, false, PacketAccess::Passive,
       PacketSnapshotSemantics::None, PacketCorrelationStrength::PassiveObserved,
       QStringLiteral("arena packet; never actively requested"), QStringLiteral("docs/AI工程交接文档.md")}};
  return contracts;
}

const PacketContract* find(const QString& command) {
  for (const PacketContract& contract : all()) if (contract.command == command) return &contract;
  return nullptr;
}

DecodedSourceBeastInventory decodeSourceBeastInventory(const QJsonObject& packet) {
  DecodedSourceBeastInventory result;
  if (!packet.contains(QStringLiteral("eps"))) {
    result.error = QStringLiteral("源兽仓库列表未返回，保留旧数量");
    return result;
  }
  const auto raw = packet.value(QStringLiteral("eps"));
  result.state = PacketFieldState::Invalid;
  result.error = QStringLiteral("源兽仓库列表无效，保留旧数量");
  if (!raw.isArray() || raw.toArray().size() > 100000) return result;
  QSet<QString> seen;
  QHash<int, qint64> candidate;
  for (const auto& value : raw.toArray()) {
    if (!value.isObject()) return result;
    const auto pack = value.toObject();
    qint64 id = 0, level = 0, experience = 0, quantity = 0;
    if (!checkedInteger(pack.value(QStringLiteral("dpi")), &id, 1, std::numeric_limits<int>::max()) ||
        !checkedInteger(pack.value(QStringLiteral("lvl")), &level, 1, std::numeric_limits<int>::max()) ||
        !checkedInteger(pack.value(QStringLiteral("exp")), &experience, 0, std::numeric_limits<int>::max()) ||
        !checkedInteger(pack.value(QStringLiteral("eqn")), &quantity, 0)) return result;
    // The official warehouse sends packs, not equipment instance ids. Keep
    // level/experience groups distinct without expanding each pack into items.
    const auto key = QStringLiteral("%1:%2:%3").arg(id).arg(level).arg(experience);
    if (seen.contains(key)) return result;
    seen.insert(key);
    qint64 total = 0;
    if (!checkedAdd(candidate.value(int(id)), quantity, &total)) return result;
    candidate.insert(int(id), total);
  }
  result.quantities = std::move(candidate);
  result.state = raw.toArray().isEmpty() ? PacketFieldState::Empty : PacketFieldState::Value;
  result.error.clear();
  return result;
}

bool validateFlash(const QString& method, const QString& argument, QString* error) {
  const auto fail = [error](const QString& reason) {
    if (error) *error = reason;
    return false;
  };
  if (method != QStringLiteral("batchpet"))
    return fail(QStringLiteral("Flash method is not registered"));
  const QStringList entries = argument.split(QLatin1Char('#'), Qt::KeepEmptyParts);
  if (entries.isEmpty() || entries.size() > PetMovePolicy::kMaxSequenceInstances)
    return fail(QStringLiteral("pet sequence must contain 1 to %1 instances").arg(PetMovePolicy::kMaxSequenceInstances));
  QSet<qint64> ids;
  for (const QString& entry : entries) {
    qint64 id = 0;
    if (!checkedInteger(entry, &id, 1) || QString::number(id) != entry || ids.contains(id))
      return fail(QStringLiteral("pet sequence contains an empty, invalid or duplicate instance"));
    ids.insert(id);
  }
  if (error) error->clear();
  return true;
}

bool validateOutbound(const QString& extension, const QString& command,
                      const QString& json, QString* error) {
  const auto fail = [error](const QString& reason) {
    if (error) *error = reason;
    return false;
  };
  const PacketContract* contract = find(command);
  if (!contract || !contract->activeSendAllowed || extension != contract->extension)
    return fail(QStringLiteral("command/extension pair is not authorized for active sending"));
  const QString trimmed = json.trimmed();
  if (contract->parameterShape == QStringLiteral("null")) {
    if (trimmed != QStringLiteral("null"))
      return fail(QStringLiteral("command requires JSON null"));
    if (error) error->clear();
    return true;
  }
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject())
    return fail(QStringLiteral("command requires a complete JSON object"));
  const QJsonObject parameters = document.object();
  bool valid = false;
  if (contract->parameterShape == QStringLiteral("{}")) valid = parameters.isEmpty();
  else if (command == QStringLiteral("2_1_R")) {
    qint64 instance = 0;
    valid = parameters.size() == 1 && parameters.value(QStringLiteral("pi")).isDouble() &&
            checkedInteger(parameters.value(QStringLiteral("pi")), &instance, 1);
  } else if (command == QStringLiteral("2_1_11")) {
    qint64 pack = -1;
    valid = parameters.size() == 2 && parameters.value(QStringLiteral("pps")).isString() &&
            parameters.value(QStringLiteral("ppt")).isDouble() &&
            checkedInteger(parameters.value(QStringLiteral("ppt")), &pack, 0, 0) &&
            validateFlash(QStringLiteral("batchpet"), parameters.value(QStringLiteral("pps")).toString());
  } else if (command == QStringLiteral("1037_0")) {
    valid = parameters.size() == 1 && parameters.value(QStringLiteral("ids")).isString() &&
            parameters.value(QStringLiteral("ids")).toString() == QStringLiteral("lights");
  } else if (command == QStringLiteral("1008_20260313_es_2") || command == QStringLiteral("1019_0")) {
    const QString key = command == QStringLiteral("1019_0") ? QStringLiteral("ai") : QStringLiteral("i");
    qint64 identifier = 0;
    valid = parameters.size() == 1 && parameters.value(key).isDouble() && checkedInteger(parameters.value(key),&identifier,1,std::numeric_limits<int>::max());
  } else if (command == QStringLiteral("1008_20260522_nf_0")) {
    qint64 target = 0;
    valid = parameters.size() == 1 && parameters.value(QStringLiteral("un")).isDouble() &&
            checkedInteger(parameters.value(QStringLiteral("un")), &target, -1, -1);
  }
  if (!valid) return fail(QStringLiteral("command parameters have unapproved fields, types or values"));
  if (error) error->clear();
  return true;
}

bool checkedInteger(const QJsonValue& value, qint64* result, qint64 minimum, qint64 maximum) {
  return DomainNumeric::checkedInteger(value, result, minimum, maximum);
}
bool checkedAdd(qint64 left, qint64 right, qint64* result) {
  return DomainNumeric::checkedAdd(left, right, result);
}
bool checkedMultiply(qint64 left, qint64 right, qint64* result) {
  return DomainNumeric::checkedMultiply(left, right, result);
}
bool normalizePet(QJsonObject* pet, bool requireDetail, QString* error) {
  qint64 id = 0;
  if (!checkedInteger(pet->value(QStringLiteral("id")), &id, 1)) {
    if (error) *error = QStringLiteral("invalid instance id"); return false;
  }
  pet->insert(QStringLiteral("id"), QString::number(id));
  for (const QString& key : {QStringLiteral("r"), QStringLiteral("ri"), QStringLiteral("lv")}) {
    if (!pet->contains(key)) continue;
    qint64 value = 0;
    if (!checkedInteger(pet->value(key), &value, key == QStringLiteral("lv") ? 0 : 1,
                        std::numeric_limits<int>::max())) {
      if (error) *error = QStringLiteral("invalid %1").arg(key); return false;
    }
    pet->insert(key, static_cast<int>(value));
  }
  if (requireDetail && ((!pet->contains(QStringLiteral("r")) && !pet->contains(QStringLiteral("ri"))) ||
                        !pet->contains(QStringLiteral("lv")))) {
    if (error) *error = QStringLiteral("detail requires race and level"); return false;
  }
  return true;
}

DecodedPetList decodePetList(const QJsonValue& value) {
  DecodedPetList decoded;
  if (value.isUndefined()) return decoded;
  decoded.state = PacketFieldState::Invalid;
  if (!appendObjects(value, &decoded.pets, 0)) {
    decoded.error = QStringLiteral("list contains a non-object or invalid nesting"); return decoded;
  }
  QSet<qint64> ids;
  for (QJsonObject& pet : decoded.pets) {
    if (!normalizePet(&pet, false, &decoded.error)) return decoded;
    const qint64 id = pet.value(QStringLiteral("id")).toString().toLongLong();
    if (ids.contains(id)) { decoded.error = QStringLiteral("duplicate instance id"); return decoded; }
    ids.insert(id);
  }
  decoded.state = decoded.pets.isEmpty() ? PacketFieldState::Empty : PacketFieldState::Value;
  return decoded;
}

DecodedBackpack decodeBackpack(const QJsonObject& packet) {
  DecodedBackpack result;
  result.list = decodePetList(packet.value(QStringLiteral("pl")));
  if (!result.list.valid()) { result.error = QStringLiteral("invalid or absent pl: %1").arg(result.list.error); return result; }
  QSet<qint64> petIds;
  for (const QJsonObject& pet : result.list.pets)
    petIds.insert(pet.value(QStringLiteral("id")).toString().toLongLong());
  const QJsonValue sequence = packet.value(QStringLiteral("pps"));
  result.sequencesPresent = !sequence.isUndefined();
  if (result.sequencesPresent) {
    QJsonArray sequences;
    if (sequence.isArray()) sequences = sequence.toArray();
    else if (sequence.isString()) sequences.append(sequence);
    else { result.error = QStringLiteral("invalid pps type"); return result; }
    QSet<qint64> positioned;
    for (int pack = 0; pack < sequences.size(); ++pack) {
      const QJsonValue entry = sequences.at(pack);
      // The real fixture has ["900001", null]; null is an unavailable pack,
      // not a malformed populated sequence and not evidence of capacity zero.
      if (entry.isNull()) continue;
      if (!entry.isString()) { result.error = QStringLiteral("invalid pack sequence"); return result; }
      QStringList ids;
      const QString encoded = entry.toString();
      if (!encoded.isEmpty()) for (const QString& text : encoded.split(QLatin1Char('#'), Qt::KeepEmptyParts)) {
        qint64 id = 0;
        if (!checkedInteger(text, &id, 1) || !petIds.contains(id) || positioned.contains(id)) {
          result.error = QStringLiteral("invalid, duplicate, or absent sequence instance"); return result;
        }
        positioned.insert(id); ids.append(QString::number(id));
      }
      result.sequences.insert(pack, ids);
    }
    if (positioned != petIds) { result.error = QStringLiteral("sequence does not cover pet set"); return result; }
  }
  const QJsonValue capacity = packet.value(QStringLiteral("ppc"));
  result.capacitiesPresent = !capacity.isUndefined();
  const auto addCapacity = [&result](int pack, const QJsonValue& value) {
    qint64 count = 0;
    if (!checkedInteger(value, &count, 0, std::numeric_limits<int>::max()) ||
        result.sequences.value(pack).size() > count) return false;
    result.capacities.insert(pack, static_cast<int>(count)); return true;
  };
  if (result.capacitiesPresent) {
    if (capacity.isArray()) {
      const QJsonArray entries = capacity.toArray();
      for (int pack = 0; pack < entries.size(); ++pack)
        if (!addCapacity(pack, entries.at(pack))) { result.error = QStringLiteral("invalid pack capacity"); return result; }
    } else if (capacity.isObject()) {
      const QJsonObject entries = capacity.toObject();
      for (auto it = entries.begin(); it != entries.end(); ++it) {
        qint64 pack = 0;
        if (!checkedInteger(it.key(), &pack, 0, std::numeric_limits<int>::max()) ||
            result.capacities.contains(static_cast<int>(pack)) || !addCapacity(static_cast<int>(pack), it.value())) {
          result.error = QStringLiteral("invalid pack capacity map"); return result;
        }
      }
    } else if (!addCapacity(0, capacity)) { result.error = QStringLiteral("invalid pack capacity"); return result; }
  }
  result.valid = true;
  return result;
}

bool decodeObject(const QString& method, const QString& payload, QJsonObject* packet, QString* error) {
  if (method != QStringLiteral("recivedata")) return false;
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    if (error) *error = QStringLiteral("invalid JSON object"); return false;
  }
  *packet = document.object();
  return true;
}
}  // namespace PacketContracts
