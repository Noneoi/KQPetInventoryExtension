#pragma once
#include "domain/pet_detail_preparation.h"
#include "domain/asset_derivation.h"
#include <QJsonArray>
#include <cstdio>

namespace RelationTest {
inline QJsonObject row(int target, int base, const QString& name, int type = 0,
                       bool listed = true, bool shown = true) {
  return {{"targetRaceId",target},{"baseRaceId",base},{"originalName",name},
          {"type",type},{"isShow",shown},{"listedWithoutOwnership",listed}};
}
inline QJsonArray rows(bool carry = false) {
  return {row(200,200,QStringLiteral("原始甲"),carry ? 4 : 0),
          row(201,200,QStringLiteral("原始甲"),carry ? 4 : 0,false),
          row(300,300,QStringLiteral("隐藏已拥有"),carry ? 8 : 0,false,false),
          row(400,400,QStringLiteral("未记录乙"),carry ? 16 : 0),
          row(401,401,QStringLiteral("隐藏未记录"),carry ? 4 : 0,false,false),
          row(500,500,QStringLiteral("原始甲"),carry ? 4 : 0)};
}
inline QJsonObject group(const QJsonArray& entries, int source = 100, bool available = true) {
  return {{"available",available},{"bySourceRaceId",QJsonObject{{QString::number(source),entries}}}};
}
inline std::shared_ptr<PetSkillCatalogSnapshot> skills(QJsonArray summon = rows(), QJsonArray carry = rows(true)) {
  auto value = std::make_shared<PetSkillCatalogSnapshot>();
  value->revision = 7; value->contentDigest = QByteArrayLiteral("relation-skill-test");
  value->root = {{"relationRules",QJsonObject{{"schema",1},{"summon",group(summon)},{"carry",group(carry)}}},
                {"pets",QJsonObject{{"100",QJsonObject{{"signs",QStringLiteral("携同技")},
                    {"summons",QJsonArray{QJsonObject{{"role","summoner"}}}}}},
                    {"600",QJsonObject{{"signs",QStringLiteral("吞噬技")}}}}}};
  return value;
}
inline std::shared_ptr<DetailRelationRoster> roster() {
  auto value = std::make_shared<DetailRelationRoster>();
  value->account = QStringLiteral("A"); value->epoch = 1; value->revision = 11;
  value->byRace[201] = {{1001,201,DetailOwnership::Backpack},{1002,201,DetailOwnership::WarehouseUnknown}};
  value->byRace[300] = {{1003,300,DetailOwnership::WarehouseElite}};
  return value;
}
inline FrozenDetailInputs inputs(QJsonObject extra = {}) {
  auto metadata = std::make_shared<PetDetailCatalogSnapshot>(); metadata->revision = 1;
  metadata->root = {{"pets",QJsonObject{
      {"100",QJsonObject{{"name",QStringLiteral("来源精灵")},{"sign",QStringLiteral("携同技")}}},
      {"200",QJsonObject{{"name",QStringLiteral("原始甲")}}},
      {"201",QJsonObject{{"name",QStringLiteral("皮肤甲")},{"groupRaceId",200}}},
      {"300",QJsonObject{{"name",QStringLiteral("隐藏已拥有")}}},
      {"400",QJsonObject{{"name",QStringLiteral("未记录乙")}}},
      {"401",QJsonObject{{"name",QStringLiteral("隐藏未记录")}}},
      {"500",QJsonObject{{"name",QStringLiteral("原始甲")}}},
      {"600",QJsonObject{{"name",QStringLiteral("吞噬方原名")},{"sign",QStringLiteral("吞噬技")}}}}}};
  QJsonObject object{{"id",101},{"r",100},{"n",QStringLiteral("来源精灵")},{"lv",100},{"zdl",1234}};
  for (auto it = extra.begin(); it != extra.end(); ++it) object.insert(it.key(),it.value());
  auto payload = std::make_shared<RawPetRecordPayload>(); payload->object = object; payload->chargedBytes = 4096;
  auto raw = std::make_shared<RawPetRecord>(); raw->key = {QStringLiteral("A"),1,101,1};
  raw->payload = payload; raw->brief = AssetDerivation::identityFields(object); raw->complete = raw->sourceKnown = true;
  auto facts = std::make_shared<PetDerivedFactsRecord>(); facts->key = {raw->key,1,{}};
  facts->facts.asset.raceId = 100; facts->facts.asset.detailAvailable = facts->facts.asset.observationVerified = true;
  FrozenDetailInputs result; result.raw = raw; result.facts = facts; result.metadata = metadata;
  result.skills = skills(); result.relationRoster = roster(); result.brief = raw->brief;
  result.summaryRevision = 1; result.sourceVerified = true;
  return result;
}
inline QHash<qint64,QJsonObject> summaries() {
  return {{1001,{{"id",1001},{"r",201},{"n",QStringLiteral("自定义错误名称")},{"customName",QStringLiteral("昵称不应出现")},
                  {"lv",88},{"zdl",5678},{"xzdl",9000},{"_location","backpack"}}},
          {2001,{{"id",2001},{"r",600},{"n",QStringLiteral("吞噬方昵称")},{"lv",100},
                  {"_location","warehouse"},{"_warehouseGroup","unexpected-group"}}}};
}
inline std::unique_ptr<PreparedPetDetail> finish(PetDetailPreparation& task,
    const QHash<qint64,QJsonObject>& observations = summaries()) {
  std::atomic_bool cancelled{false};
  for (int pass = 0; pass < 10000; ++pass) {
    const auto state = task.step(cancelled);
    if (state == DetailStep::Complete) return task.takeResult();
    if (state == DetailStep::Failed || state == DetailStep::Cancelled) return {};
    if (state == DetailStep::NeedSummaries) {
      QVector<DetailRelatedSummary> result;
      for (qint64 id : task.requestedSummaries()) {
        const auto found = observations.constFind(id);
        result.append({id,found == observations.cend() ? 0u : 1u,found != observations.cend(),
                       found == observations.cend() ? QJsonObject{} : *found});
      }
      if (!task.provideSummaries(result)) return {};
    }
  }
  return {};
}
inline const DetailPage* page(const PreparedPetDetail* detail, DetailSection section) {
  if (detail) for (const auto& value : detail->pages) if (value.section == section) return &value;
  return nullptr;
}
inline QVector<const DetailEntry*> entries(const DetailPage& page, const QString& group) {
  QVector<const DetailEntry*> result;
  for (const auto& value : page.entries) if (value.group == group) result.append(&value);
  return result;
}
}
