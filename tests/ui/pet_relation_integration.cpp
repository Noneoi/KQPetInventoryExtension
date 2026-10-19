#include "support/preview_inventory_support.h"
#include "support/relation_detail_support.h"
#include "ui/detail/prepared_pet_detail_renderer.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QTextDocument>

namespace {
bool check(bool value, const char* reason) {
  if (!value) std::fprintf(stderr,"FAIL: %s\n",reason);
  return value;
}
QString plain(const QString& html) { QTextDocument document; document.setHtml(html); return document.toPlainText(); }
PreparedPetDetailHandle prepared(FrozenDetailInputs input, DetailSection section, int index = 0) {
  PetDetailPreparation task(std::move(input),section,index);
  return PreparedPetDetailHandle(RelationTest::finish(task).release());
}
bool hasCandidate(const PreparedPetDetailHandle& detail, const QString& name, DetailOwnership ownership) {
  const auto* page = RelationTest::page(detail.get(),DetailSection::SummonRelations);
  if (page) for (const auto& entry : page->entries)
    if (entry.group == DetailRelationGroup::summonList() && entry.name == name && entry.ownership == ownership) return true;
  return false;
}
std::shared_ptr<PetSkillCatalogSnapshot> skillsFor(const PetSkillCatalogSnapshot& previous,
                                                quint64 revision, const QByteArray& digest,
                                                const QString& name, int target = 7201) {
  auto result = std::make_shared<PetSkillCatalogSnapshot>(previous);
  result->revision = revision; result->contentDigest = digest;
  result->root.insert("relationRules",QJsonObject{{"schema",1},
      {"summon",RelationTest::group({RelationTest::row(target,target,name)},7396)},
      {"carry",QJsonObject{{"available",true},{"bySourceRaceId",QJsonObject{}}}}});
  return result;
}
}
int main(int argc, char** argv) {
  QApplication app(argc,argv); bool ok = true;
  auto input = RelationTest::inputs({{"sepi",1001},{"sppl",QJsonObject{{"id",1001},{"r",201}}},
      {"cepi",1001},{"cppl",QJsonObject{{"id",1001},{"r",201}}},{"crpis",QJsonArray{2001}}});
  const auto summonModel = prepared(input,DetailSection::SummonRelations);
  const auto carryModel = prepared(input,DetailSection::CarryRelations);
  const QString summonHtml = PreparedPetDetailRenderer::render(summonModel,{}), summonText = plain(summonHtml);
  const QString carryHtml = PreparedPetDetailRenderer::render(carryModel,{}), carryText = plain(carryHtml);
  ok &= check(summonText.contains(QStringLiteral("已召唤精灵")) && summonText.contains(QStringLiteral("可召唤对象")) &&
              !summonText.contains(QStringLiteral("待召唤")) && !summonText.contains(QStringLiteral("昵称不应出现")) &&
              !summonText.contains(QStringLiteral("自定义错误名称")),"summon renderer conflated current/candidate or showed aliases");
  ok &= check(summonHtml.contains(QStringLiteral("color:#15803d'>原始甲")) &&
              summonHtml.contains(QStringLiteral("color:#233044'>原始甲")),
              "current relation was not green, or candidate inherited its current-state color");
  ok &= check(carryText.contains(QStringLiteral("携同关系")) && carryText.contains(QStringLiteral("当前携同精灵")) &&
              carryText.contains(QStringLiteral("吞噬本精灵的精灵")) && carryText.contains(QStringLiteral("神使")) &&
              carryText.contains(QStringLiteral("召唤兽")) && carryText.contains(QStringLiteral("不朽书灵")),
              "source carrying mechanism, reverse owner, or target role was mislabelled");
  ok &= check(carryText.contains(QStringLiteral("仓库（分组未记录）")) &&
              carryText.contains(QStringLiteral("当前名单未记录")) && carryText.contains(QStringLiteral("不等同于未拥有")) &&
              !carryText.contains(QStringLiteral("不在本账号")),"unknown warehouse or partial-roster absence was overstated");
  for (const auto& setting : {qMakePair(QStringLiteral("吞噬技"),QStringLiteral("当前吞噬对象")),
                              qMakePair(QStringLiteral("天觉者"),QStringLiteral("当前天觉携带精灵"))}) {
    auto typed = input; auto metadata = std::make_shared<PetDetailCatalogSnapshot>(*input.metadata);
    auto pets = metadata->root.value("pets").toObject(), source = pets.value("100").toObject();
    source.insert("sign",setting.first); pets.insert("100",source); metadata->root.insert("pets",pets); typed.metadata = metadata;
    auto typedSkills = std::make_shared<PetSkillCatalogSnapshot>(*input.skills);
    pets = typedSkills->root.value("pets").toObject(); source = pets.value("100").toObject();
    source.insert("signs",setting.first); pets.insert("100",source); typedSkills->root.insert("pets",pets); typed.skills = typedSkills;
    const auto result = prepared(typed,DetailSection::CarryRelations);
    ok &= check(result && plain(PreparedPetDetailRenderer::render(result,{})).contains(setting.second),
                "official exact sign did not determine the source carrying label");
  }
  QJsonArray many; for (int race = 1000; race < 1130; ++race) many.append(RelationTest::row(race,race,QStringLiteral("后页候选%1").arg(race)));
  auto paged = input; paged.skills = RelationTest::skills(many,{});
  const auto second = prepared(paged,DetailSection::SummonRelations,1);
  const QString later = plain(PreparedPetDetailRenderer::render(second,{}));
  ok &= check(second && later.contains(QStringLiteral("后页候选")) && !later.contains(QStringLiteral("当前关联未记录")) &&
              !later.contains(QStringLiteral("已召唤精灵")),"later summon page falsely claimed the current relation was absent");

  QTemporaryDir temporary;
  PreviewInventory::Fixture fixture(temporary.path(),QStringLiteral("relation-cache-fixture"));
  QJsonObject selected{{"id",42},{"r",7396},{"lv",100},{"n",QStringLiteral("关系缓存样例")},
                       {"sepi",-1},{"srpi",-1},{"cepi",-1}};
  QJsonObject target{{"id",52},{"r",7201},{"lv",100},{"n",QStringLiteral("账号自定义名称")}};
  ok &= check(fixture.initialized && fixture.publishLists({selected},{},{}) && fixture.factsReady({42}),
              "real publisher fixture did not initialize");
  const auto baseSkills = fixture.inventory.skillSnapshot();
  if (!baseSkills) return 1;
  auto firstSkills = skillsFor(*baseSkills,baseSkills->revision + 1,QByteArrayLiteral("relation-first"),QStringLiteral("官方候选甲"));
  fixture.publisher.skillMetadataUpdated(firstSkills);
  fixture.inventory.watchDetail(0,42);
  ok &= check(PreviewInventory::until([&] { return hasCandidate(fixture.inventory.preparedDetail(0,42),
      QStringLiteral("官方候选甲"),DetailOwnership::Missing); }),"missing mutable array did not receive public candidates through the publisher");
  const auto initial = fixture.inventory.preparedDetail(0,42);
  const auto rawVersion = fixture.repository.recordVersion(42).key;
  const auto rawObject = fixture.repository.rawRecordHandle(42)->object();
  ok &= check(initial && initial->version.skillRevision == firstSkills->revision && initial->version.relationRosterRevision > 0,
              "publisher omitted the frozen skill/roster dependency versions");

  // Refresh only the warehouse part, leaving the selected backpack detail's
  // observation version untouched. Replaying a complete backpack packet is a
  // new observation even when its contents are equal, so it cannot test this.
  quint64 warehouseGeneration = 100;
  const auto warehouseOnly = [&](const QJsonArray& normal) {
    fixture.repository.expectListPart(QStringLiteral("2_1_S"),++warehouseGeneration,
        fixture.repository.accountKey(),fixture.repository.sessionGeneration());
    deliverVerifiedFixture(&fixture.repository,{{"_cmd","2_1_S"},{"ns",normal},{"es",QJsonArray{}},{"rb",QJsonArray{}}});
    return waitForRepositoryIdle(&fixture.repository,30000);
  };
  const bool warehouseAdded = warehouseOnly({PreviewInventory::brief(target)}) && PreviewInventory::until([&] {
      return hasCandidate(fixture.inventory.preparedDetail(0,42),QStringLiteral("官方候选甲"),DetailOwnership::WarehouseNormal);
    });
  if (!warehouseAdded) {
    const auto current = fixture.inventory.preparedDetail(0,42);
    const auto* currentPage = RelationTest::page(current.get(),DetailSection::SummonRelations);
    std::fprintf(stderr,"relation roster diagnostic: %s selectedRawSame=%d roster=%llu initialRoster=%llu target=%s\n",
        qPrintable(fixture.diagnostics()),fixture.repository.recordVersion(42).key == rawVersion,
        static_cast<unsigned long long>(current ? current->version.relationRosterRevision : 0),
        static_cast<unsigned long long>(initial ? initial->version.relationRosterRevision : 0),
        QJsonDocument(fixture.repository.briefFor(52)).toJson(QJsonDocument::Compact).constData());
    if (currentPage) for (const auto& entry : currentPage->entries)
      std::fprintf(stderr,"candidate name=%s id=%lld ownership=%d\n",qPrintable(entry.name),
          static_cast<long long>(entry.relatedInstanceId),int(entry.ownership));
  }
  ok &= check(warehouseAdded && fixture.repository.recordVersion(42).key == rawVersion,
    "warehouse-only membership change did not invalidate candidates independently of selected raw data");
  ok &= check(warehouseOnly({}) && PreviewInventory::until([&] {
      return hasCandidate(fixture.inventory.preparedDetail(0,42),QStringLiteral("官方候选甲"),DetailOwnership::Missing);
    }) && fixture.repository.recordVersion(42).key == rawVersion,
    "warehouse-only removal left stale candidates or changed selected raw data");

  ok &= check(fixture.publishLists({selected,target},{},{}) && PreviewInventory::until([&] {
      return hasCandidate(fixture.inventory.preparedDetail(0,42),QStringLiteral("官方候选甲"),DetailOwnership::Backpack);
    }),"adding an eligible instance did not invalidate selected detail");
  const auto inBackpack = fixture.inventory.preparedDetail(0,42);
  ok &= check(initial && inBackpack && inBackpack->version.relationRosterRevision > initial->version.relationRosterRevision &&
              fixture.repository.rawRecordHandle(42)->object() == rawObject &&
              hasCandidate(initial,QStringLiteral("官方候选甲"),DetailOwnership::Missing),
              "roster refresh rewrote selected content or mutated an already published snapshot");
  ok &= check(fixture.publishLists({selected},{},{PreviewInventory::brief(target)}) && PreviewInventory::until([&] {
      return hasCandidate(fixture.inventory.preparedDetail(0,42),QStringLiteral("官方候选甲"),DetailOwnership::WarehouseElite);
    }),"moving a candidate to elite warehouse did not invalidate its location");

  // Same revision and different digest must also refresh; version equality is
  // a pair, as it is for the existing metadata and facts path.
  const auto beforeSkillUpdate = fixture.repository.recordVersion(42).key;
  auto secondSkills = skillsFor(*firstSkills,firstSkills->revision,QByteArrayLiteral("relation-second"),QStringLiteral("官方候选乙"),7202);
  fixture.publisher.skillMetadataUpdated(secondSkills);
  ok &= check(PreviewInventory::until([&] {
      const auto current = fixture.inventory.preparedDetail(0,42);
      return current && current->version.skillDigest == secondSkills->contentDigest &&
          hasCandidate(current,QStringLiteral("官方候选乙"),DetailOwnership::Missing) &&
          !hasCandidate(current,QStringLiteral("官方候选甲"),DetailOwnership::WarehouseElite);
    }),"changed skill digest left stale official relation candidates in the selected-detail cache");
  ok &= check(fixture.repository.recordVersion(42).key == beforeSkillUpdate,"changing public relation rules rewrote selected account data");

  auto thirdSkills = skillsFor(*secondSkills,secondSkills->revision + 1,QByteArrayLiteral("relation-third"),QStringLiteral("官方候选甲"));
  fixture.publisher.skillMetadataUpdated(thirdSkills);
  ok &= check(PreviewInventory::until([&] { return hasCandidate(fixture.inventory.preparedDetail(0,42),
      QStringLiteral("官方候选甲"),DetailOwnership::WarehouseElite); }),"restored public pairing did not use the current roster");
  ok &= check(fixture.publishLists({selected},{},{}) && PreviewInventory::until([&] {
      return hasCandidate(fixture.inventory.preparedDetail(0,42),QStringLiteral("官方候选甲"),DetailOwnership::Missing);
    }),"removing an instance from the current roster left stale ownership in a candidate");
  ok &= check(fixture.close(),"relation fixture did not shut down cleanly");
  return ok ? 0 : 1;
}
