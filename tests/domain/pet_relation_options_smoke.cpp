#include "../support/relation_detail_support.h"
#include "domain/pet_relation_options.h"
#include <QCoreApplication>

namespace {
bool check(bool value, const char* message) {
  if (!value) std::fprintf(stderr,"FAIL: %s\n",message);
  return value;
}
bool has(const QVector<const DetailEntry*>& entries, qint64 id, DetailOwnership ownership) {
  for (const auto* value : entries) if (value->relatedInstanceId == id && value->ownership == ownership) return true;
  return false;
}
}
int main(int argc, char** argv) {
  QCoreApplication app(argc,argv); bool ok = true;
  auto input = RelationTest::inputs({{"sepi",1001},{"sppl",QJsonObject{{"id",1001},{"r",201},
      {"n",QStringLiteral("皮肤/自定义名字")},{"customName",QStringLiteral("昵称不应出现")}}},
      {"asps",QJsonArray{1001,9999}}});
  const auto options = petRelationCandidateOptions(input.skills->root,100,false);
  ok &= check(options.available && options.families.size() == 5 && options.families[0].targetRaceIds == QVector<int>{200,201},
              "explicit base mapping was lost, or same-name distinct species were merged");
  ok &= check(petRelationCandidateOptions(input.skills->root,999,false).available &&
              petRelationCandidateOptions(input.skills->root,999,false).families.isEmpty(),
              "complete public catalog without a source was treated as unavailable");
  PetDetailPreparation task(input,DetailSection::SummonRelations);
  const auto model = RelationTest::finish(task);
  const auto* page = RelationTest::page(model.get(),DetailSection::SummonRelations);
  ok &= check(page && page->officialRelationCandidates && page->summonCandidateCount == 4,
              "official candidates did not replace the account's mutable candidate array");
  if (page) {
    const auto current = RelationTest::entries(*page,DetailRelationGroup::summoned());
    const auto candidates = RelationTest::entries(*page,DetailRelationGroup::summonList());
    ok &= check(current.size() == 1 && current[0]->selected && current[0]->relatedInstanceId == 1001 &&
                current[0]->name == QStringLiteral("原始甲"),"current summoned instance or official original name was lost");
    ok &= check(candidates.size() == 4 && !has(candidates,1001,DetailOwnership::Backpack) &&
                has(candidates,1002,DetailOwnership::WarehouseUnknown) && has(candidates,1003,DetailOwnership::WarehouseElite),
                "selected/owned skin/hidden owned targets or unknown warehouse classification are incorrect");
    int absent = 0; bool hiddenAbsent = false, leakedRaw = false;
    for (const auto* item : candidates) {
      absent += item->ownership == DetailOwnership::Missing;
      hiddenAbsent |= item->name == QStringLiteral("隐藏未记录"); leakedRaw |= item->relatedInstanceId == 9999;
    }
    ok &= check(absent == 2 && !hiddenAbsent && !leakedRaw,"public visibility or roster-absence semantics changed");
  }
  ok &= check(model && model->version.skillRevision == 7 && model->version.skillDigest == QByteArrayLiteral("relation-skill-test") &&
              model->version.relationRosterRevision == 11,"detail did not retain frozen relation source versions");

  PetDetailPreparation noAccountCandidates(RelationTest::inputs(),DetailSection::SummonRelations);
  const auto withoutArrays = RelationTest::finish(noAccountCandidates);
  const auto* withoutPage = RelationTest::page(withoutArrays.get(),DetailSection::SummonRelations);
  ok &= check(withoutPage && withoutPage->summonCandidateCount == 5,"missing account candidate field erased public candidates");
  auto noRoster = RelationTest::inputs(); noRoster.relationRoster.reset();
  PetDetailPreparation noRosterTask(noRoster,DetailSection::SummonRelations);
  const auto unlocated = RelationTest::finish(noRosterTask);
  const auto* unlocatedPage = RelationTest::page(unlocated.get(),DetailSection::SummonRelations);
  ok &= check(unlocatedPage && unlocatedPage->summonCandidateCount == 3,"hidden unowned targets became visible without a roster");
  if (unlocatedPage) for (const auto& entry : unlocatedPage->entries)
    ok &= check(entry.ownership == DetailOwnership::Unknown,"missing roster was presented as definite absence");
  auto currentOutsideRoster = input; auto emptyRoster = RelationTest::roster(); emptyRoster->byRace.clear();
  currentOutsideRoster.relationRoster = emptyRoster;
  PetDetailPreparation outsideTask(currentOutsideRoster,DetailSection::SummonRelations);
  const auto outsideModel = RelationTest::finish(outsideTask,{});
  const auto* outsidePage = RelationTest::page(outsideModel.get(),DetailSection::SummonRelations);
  ok &= check(outsidePage && outsidePage->summonCandidateCount == 2 &&
              RelationTest::entries(*outsidePage,DetailRelationGroup::summoned()).size() == 1,
              "current association outside the roster was offered again as an unlocated candidate family");

  auto carryInput = RelationTest::inputs({{"cepi",1001},{"cppl",QJsonObject{{"id",1001},{"r",201}}},
                                         {"crpis",QJsonArray{2001}}});
  PetDetailPreparation carryTask(carryInput,DetailSection::CarryRelations);
  const auto carryModel = RelationTest::finish(carryTask);
  const auto* carryPage = RelationTest::page(carryModel.get(),DetailSection::CarryRelations);
  ok &= check(carryPage && carryPage->carryKind == DetailCarryKind::Companion && carryPage->carryCandidateCount == 4 &&
              carryPage->carryCandidatesExpanded && carryPage->contractCarrierRole && carryPage->contractEnvoyRole,
              "carrier mechanism, independent reverse relation, or full candidate expansion was lost");
  if (carryPage) {
    const auto owner = RelationTest::entries(*carryPage,DetailRelationGroup::carrierOwner());
    ok &= check(owner.size() == 1 && owner[0]->carryKind == DetailCarryKind::Swallow &&
                owner[0]->ownership == DetailOwnership::WarehouseUnknown,"reverse owner role or unknown warehouse was fabricated");
    QSet<int> types; for (const auto* item : RelationTest::entries(*carryPage,DetailRelationGroup::carryCandidate())) types.insert(item->relationType);
    ok &= check(types == QSet<int>{4,8,16},"target roles were conflated with the source carrying mechanism");
  }
  PetDetailPreparation overviewTask(carryInput);
  const auto overview = RelationTest::finish(overviewTask);
  const auto* collapsed = RelationTest::page(overview.get(),DetailSection::CarryRelations);
  ok &= check(collapsed && !collapsed->carryCandidatesExpanded && collapsed->carryCandidateCount == 4 &&
              RelationTest::entries(*collapsed,DetailRelationGroup::carryCandidate()).isEmpty(),
              "overview retained an unbounded full carry list instead of a correct count");
  for (const auto& item : QVector<QPair<QJsonValue,DetailCarryKind>>{
      {QStringLiteral("吞噬技"),DetailCarryKind::Swallow},
      {QStringLiteral("天觉者"),DetailCarryKind::Almighty},
      {QString(),DetailCarryKind::Unknown},
      {QStringLiteral("携同技能"),DetailCarryKind::Unknown},
      {QJsonValue(QJsonValue::Undefined),DetailCarryKind::Companion},
      {QJsonValue(123),DetailCarryKind::Companion}}) {
    auto changedInput = carryInput; auto changedSkills = std::make_shared<PetSkillCatalogSnapshot>(*carryInput.skills);
    auto pets = changedSkills->root.value("pets").toObject(), source = pets.value("100").toObject();
    source.insert("signs",item.first); pets.insert("100",source); changedSkills->root.insert("pets",pets); changedInput.skills = changedSkills;
    PetDetailPreparation changedTask(changedInput,DetailSection::CarryRelations);
    const auto changed = RelationTest::finish(changedTask);
    const auto* changedPage = RelationTest::page(changed.get(),DetailSection::CarryRelations);
    ok &= check(changedPage && changedPage->carryKind == item.second,
                "same-snapshot skill signs did not override stale metadata, or missing signs lost the metadata fallback");
  }

  auto unavailable = input; auto unavailableSkills = std::make_shared<PetSkillCatalogSnapshot>(*input.skills);
  auto rules = unavailableSkills->root.value("relationRules").toObject();
  rules.insert("summon",RelationTest::group({},100,false)); unavailableSkills->root.insert("relationRules",rules);
  unavailable.skills = unavailableSkills;
  PetDetailPreparation unavailableTask(unavailable,DetailSection::SummonRelations);
  const auto retained = RelationTest::finish(unavailableTask);
  const auto* retainedPage = RelationTest::page(retained.get(),DetailSection::SummonRelations);
  ok &= check(retainedPage && !retainedPage->officialRelationCandidates && !retainedPage->relationCatalogProblem.isEmpty() &&
              RelationTest::entries(*retainedPage,DetailRelationGroup::summoned()).size() == 1 &&
              RelationTest::entries(*retainedPage,DetailRelationGroup::summonList()).size() == 1,
              "unavailable public data erased observed current/candidates or pretended to be a confirmed empty catalog");
  auto malformed = input.skills->root; auto badRules = malformed.value("relationRules").toObject();
  auto duplicate = RelationTest::rows(); duplicate.append(duplicate[0]);
  badRules.insert("summon",RelationTest::group(duplicate)); malformed.insert("relationRules",badRules);
  ok &= check(!petRelationCandidateOptions(malformed,100,false).available,"duplicate final pair was accepted as a complete public projection");
  auto wrongSession = input; auto foreignRoster = std::make_shared<DetailRelationRoster>(*input.relationRoster);
  foreignRoster->epoch = 2; wrongSession.relationRoster = foreignRoster;
  PetDetailPreparation foreignTask(wrongSession,DetailSection::SummonRelations);
  ok &= check(!RelationTest::finish(foreignTask),"relation roster from another session contaminated the selected pet");

  QJsonArray many; for (int race = 1000; race < 1130; ++race) many.append(RelationTest::row(race,race,QStringLiteral("候选%1").arg(race)));
  auto pagedInput = input; pagedInput.skills = RelationTest::skills(many,{});
  PetDetailPreparation firstTask(pagedInput,DetailSection::SummonRelations,0), secondTask(pagedInput,DetailSection::SummonRelations,1);
  const auto first = RelationTest::finish(firstTask), second = RelationTest::finish(secondTask);
  const auto* firstPage = RelationTest::page(first.get(),DetailSection::SummonRelations);
  const auto* secondPage = RelationTest::page(second.get(),DetailSection::SummonRelations);
  ok &= check(firstPage && secondPage && firstPage->totalItems == 131 && secondPage->totalItems == 131 &&
              firstPage->entries.size() == 64 && secondPage->entries.size() == 64 &&
              RelationTest::entries(*firstPage,DetailRelationGroup::summoned()).size() == 1 &&
              RelationTest::entries(*secondPage,DetailRelationGroup::summoned()).isEmpty(),
              "official summon pagination duplicated/dropped the current relation or candidates");
  return ok ? 0 : 1;
}
