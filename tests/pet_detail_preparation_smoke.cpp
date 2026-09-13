#include "../src/domain/pet_detail_preparation.h"
#include "../src/domain/asset_derivation.h"
#include "../src/domain/pet_analysis_facts.h"
#include "../src/domain/pet_metadata_view.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <cstdio>

namespace {
bool check(bool valid, const char* message) { if (!valid) std::fprintf(stderr,"FAIL: %s\n",message); return valid; }
FrozenDetailInputs fixture(QJsonObject extra = {}, QString era = QStringLiteral("灵初")) {
  auto metadata = std::make_shared<PetDetailCatalogSnapshot>(); metadata->revision = 1;
  metadata->root = {{QStringLiteral("pets"),QJsonObject{{QStringLiteral("7001"),QJsonObject{{QStringLiteral("name"),QStringLiteral("[%1]测试精灵").arg(era)}}}}},
      {QStringLiteral("sacredStarPlans"),QJsonObject{{QStringLiteral("2"),QJsonObject{{QStringLiteral("maxLevel"),10}}}}},
      {QStringLiteral("sacredStagePlans"),QJsonObject{{QStringLiteral("6"),QJsonObject{{QStringLiteral("maxLevel"),7}}}}},
      {QStringLiteral("stargods"),QJsonObject{{QStringLiteral("80"),QJsonObject{{QStringLiteral("name"),QStringLiteral("红色星神")},{QStringLiteral("quality"),6},{QStringLiteral("changeable"),false}}}}}};
  QJsonArray stars; for (int i = 0; i < 140; ++i) stars.append(80); stars[70] = QJsonObject{{QStringLiteral("unsupported"),80}};
  QJsonObject object{{QStringLiteral("id"),QStringLiteral("101")},{QStringLiteral("r"),7001},{QStringLiteral("n"),QStringLiteral("选中精灵")},
      {QStringLiteral("lv"),100},{QStringLiteral("zdl"),7},{QStringLiteral("gt"),6},{QStringLiteral("ip"),QStringLiteral("100##300")},
      {QStringLiteral("gps"),QStringLiteral("2#1#3")},{QStringLiteral("shenjue"),QStringLiteral("1034#2#6|0:0")},
      {QStringLiteral("badge"),QStringLiteral("101:5#201:0")},{QStringLiteral("astrolabe"),QStringLiteral("350:0:1#351:1:0")},
      {QStringLiteral("sgs"),QStringLiteral("-2:6:80#80:6#0:0")},{QStringLiteral("sgsp"),stars}};
  for (auto it = extra.begin(); it != extra.end(); ++it) object.insert(it.key(),it.value());
  auto payload = std::make_shared<RawPetRecordPayload>(); payload->object = object; payload->chargedBytes = 8192;
  auto raw = std::make_shared<RawPetRecord>(); raw->key = {QStringLiteral("A"),1,101,1}; raw->payload = payload; raw->complete = raw->sourceKnown = true; raw->brief = AssetDerivation::identityFields(object);
  auto facts = std::make_shared<PetDerivedFactsRecord>(); facts->key = {raw->key,1,{}};
  facts->facts.asset.raceId = object.value(QStringLiteral("r")).toInt(); facts->facts.asset.detailAvailable = facts->facts.asset.observationVerified = true;
  facts->facts.battlePower.current = 424242; facts->facts.battlePower.hasCurrent = true;
  FrozenDetailInputs input; input.raw = raw; input.facts = facts; input.brief = raw->brief; input.summaryRevision = 1; input.metadata = metadata; input.sourceVerified = true; return input;
}
std::unique_ptr<PreparedPetDetail> finish(PetDetailPreparation& task, int* maximumBatch = nullptr) {
  std::atomic_bool cancelled{false};
  for (int count = 0; count < 10000; ++count) {
    const auto step = task.step(cancelled);
    if (step == DetailStep::Complete) return task.takeResult();
    if (step == DetailStep::Failed || step == DetailStep::Cancelled) { std::fprintf(stderr,"prepare error: %s\n",qPrintable(task.error())); return {}; }
    if (step == DetailStep::NeedSummaries) {
      const auto ids = task.requestedSummaries(); if (maximumBatch) *maximumBatch = qMax(*maximumBatch,int(ids.size()));
      QVector<DetailRelatedSummary> summaries;
      for (auto id : ids) summaries.append({id,5,true,{{QStringLiteral("id"),QString::number(id)},{QStringLiteral("n"),QStringLiteral("摘要%1").arg(id)},{QStringLiteral("lv"),88},{QStringLiteral("zdl"),QStringLiteral("bad")}}});
      if (!task.provideSummaries(summaries)) return {};
    }
  }
  return {};
}
QString joined(const QVector<DetailField>& fields) { QString text; for (const auto& f : fields) text += f.label + QLatin1Char('=') + f.text + QLatin1Char(';'); return text; }
}
int main(int argc, char** argv) {
  QCoreApplication app(argc,argv); bool ok = true;
  PetDetailPreparation overview(fixture()); const auto model = finish(overview);
  ok &= check(model && model->pages.size() == 6 && model->battlePower.current == 424242 && model->sourceVerified,"overview did not reuse the supplied facts or lost sections");
  if (model) {
    const auto talent = joined(model->talent), sacred = joined(model->sacred);
    ok &= check(talent.contains(QStringLiteral("单星能=生命 100")) && !talent.contains(QStringLiteral("物防=300")) && !talent.contains(QStringLiteral("物攻=")) &&
        talent.contains(QStringLiteral("速度 待确认")) && talent.contains(QStringLiteral("天赋战斗力/满天赋战斗力=")),"compact talent shifted original property positions or exposed hidden legacy properties");
    ok &= check(sacred.contains(QStringLiteral("0/10 星（未满星）")) && sacred.contains(QStringLiteral("0/7 阶（未满阶）")),"zero sacred levels became maximum levels");
    ok &= check(model->pages[1].entries[0].name == QStringLiteral("星轮状态") && model->pages[1].entries[0].fields[0].state == DetailKnowledge::Unknown &&
        model->pages[1].entries[1].selected && !model->pages[1].entries[1].activated && model->pages[1].entries[2].activated &&
        model->pages[1].entries[0].fields[1].text == QStringLiteral("1"),"astrolabe selected/activated or independent breakthrough known state was lost");
    ok &= check(model->pages[2].entries.size() == 3 && model->pages[2].entries[0].changeable,"special changeable/empty equipped slots were discarded");
    ok &= check(model->pages[3].totalItems == 140 && model->pages[3].entries.size() == 64 && model->pages[3].hasNext(),"backpack overview is not naturally paged");
  }
  for (const auto& era : {QStringLiteral("神运"),QStringLiteral("星迹"),QStringLiteral("启元")}) {
    PetDetailPreparation earlier(fixture({{QStringLiteral("astrolabebr"),true},
        {QStringLiteral("astrolabe"),QStringLiteral("350:1:1#351:1:0")},
        {QStringLiteral("n"),QStringLiteral("[灵初]误导的皮肤名字")}},era),DetailSection::Astrolabe);
    const auto earlierDetail = finish(earlier);
    ok &= check(earlierDetail && earlierDetail->pages[0].entries.size() == 3 &&
        earlierDetail->pages[0].entries[0].fields.size() == 2 &&
        !joined(earlierDetail->pages[0].entries[0].fields).contains(QStringLiteral("突破")) &&
        joined(earlierDetail->pages[0].entries[0].fields).contains(QStringLiteral("点亮数量=2")) &&
        joined(earlierDetail->pages[0].entries[0].fields).contains(QStringLiteral("选中数量=1")),
        "non-Lingchu details exposed breakthrough or lost node counters after omitting its status field");
  }
  PetDetailPreparation lingchuAstrolabe(fixture({{QStringLiteral("astrolabebr"),false}}),DetailSection::Astrolabe);
  const auto lingchuDetail = finish(lingchuAstrolabe);
  ok &= check(lingchuDetail && joined(lingchuDetail->pages[0].entries[0].fields).contains(QStringLiteral("突破=未突破")),
      "Lingchu detail lost its applicable breakthrough state");
  PetDetailPreparation unknownEra(fixture({{QStringLiteral("r"),7002},{QStringLiteral("astrolabebr"),false}}),DetailSection::Astrolabe);
  const auto unknownEraDetail = finish(unknownEra);
  ok &= check(unknownEraDetail && !joined(unknownEraDetail->pages[0].entries[0].fields).contains(QStringLiteral("突破")) &&
      unknownEraDetail->pages[0].entries.size() == 3,
      "unknown-era detail claimed an unbroken astrolabe or hid its existing nodes");
  PetDetailPreparation talentTask(fixture({{QStringLiteral("ip"),QStringLiteral("101#202#303#404#505#606#707#808#909#1010#1111#1212")},
      {QStringLiteral("gps"),QStringLiteral("2#3#2#3#2#3#2#3#2#3#2#3")},
      {QStringLiteral("czdlv"),QJsonObject{{QStringLiteral("iv"),111}}},{QStringLiteral("mzdlv"),QJsonObject{{QStringLiteral("iv"),222}}}}));
  const auto compactTalent = finish(talentTask);
  const auto talentText = compactTalent ? joined(compactTalent->talent) : QString();
  ok &= check(talentText.contains(QStringLiteral("单星能=生命 101　超物攻 909　超魔攻 1111")) &&
      talentText.contains(QStringLiteral("双星能=速度 808　超物防 1010　超魔防 1212")) &&
      talentText.contains(QStringLiteral("天赋战斗力/满天赋战斗力=111/222")),"six retained talent properties or combined power pair were incorrect");
  PetDetailPreparation unpoweredTalent(fixture({{QStringLiteral("r"),7529},
      {QStringLiteral("n"),QStringLiteral("[灵初]轮转·命运之轮")},{QStringLiteral("gt"),0},
      {QStringLiteral("ip"),QStringLiteral("10#1#1#1#1#1#1#20#30#40#50#60")},{QStringLiteral("gps"),QString()}}));
  const auto unpowered = finish(unpoweredTalent);
  const auto unpoweredText = unpowered ? joined(unpowered->talent) : QString();
  ok &= check(unpoweredText.contains(QStringLiteral("评价=一无是处（0级）")) &&
      unpoweredText.contains(QStringLiteral("无星能=生命 10　速度 20　超物攻 30　超物防 40　超魔攻 50　超魔防 60")) &&
      !unpoweredText.contains(QStringLiteral("星能待确认")),"official explicit empty gps became unknown energy or guessed trained talent");
  PetDetailPreparation emptySlots(fixture({{QStringLiteral("sgs"),QStringLiteral("80:6:0:99#0#-1#-2")}}),DetailSection::EquippedStargods);
  const auto slotDetails = finish(emptySlots);
  ok &= check(slotDetails && slotDetails->pages[0].entries.size() == 4 && slotDetails->pages[0].entries[0].imageDefineId == 80 &&
      !slotDetails->pages[0].entries[0].emptySlot && slotDetails->pages[0].entries[1].emptySlot && slotDetails->pages[0].entries[2].emptySlot &&
      slotDetails->pages[0].entries[3].emptySlot && slotDetails->pages[0].entries[3].changeable,"official bare empty/disabled/changeable slots or four-part encoding were lost");
  int total = 0, invalid = 0;
  for (int page = 0; page < 3; ++page) {
    PetDetailPreparation task(fixture(),DetailSection::StargodBackpack,page); const auto value = finish(task);
    ok &= check(value && value->pages.size() == 1,"backpack page failed"); if (!value) continue;
    total += int(value->pages[0].entries.size()); for (const auto& e : value->pages[0].entries) invalid += e.state == DetailKnowledge::Invalid;
  }
  ok &= check(total == 140 && invalid == 1,"paging dropped an invalid original entry or lost later valid entries");
  auto missing = fixture(); auto missingRaw = std::make_shared<RawPetRecord>(*missing.raw); auto missingPayload = std::make_shared<RawPetRecordPayload>(*missingRaw->payload);
  for (const auto& key : {"gt","lv","badge","shenjue","astrolabe"}) missingPayload->object.remove(QString::fromLatin1(key));
  missingRaw->payload = missingPayload; missingRaw->brief.remove(QStringLiteral("lv")); missing.raw = missingRaw; missing.brief.remove(QStringLiteral("lv")); missing.sourceVerified = false;
  PetDetailPreparation missingTask(missing); const auto unknown = finish(missingTask);
  ok &= check(unknown && !unknown->sourceVerified && unknown->identity.level.state == DetailKnowledge::Unknown &&
      unknown->talent[0].state == DetailKnowledge::Unknown && unknown->sacred[0].state == DetailKnowledge::Unknown && unknown->pages[0].state == DetailKnowledge::Unknown,"missing values became explicit zero/unequipped/current source");
  PetDetailPreparation missingLevels(fixture({{QStringLiteral("shenjue"),QStringLiteral("1034#2#6")}})); const auto sacredUnknown = finish(missingLevels);
  ok &= check(sacredUnknown && sacredUnknown->sacred[1].state == DetailKnowledge::Unknown && sacredUnknown->sacred[2].state == DetailKnowledge::Unknown,"missing sacred current levels became full");
  auto future = fixture({{QStringLiteral("shenjue"),QStringLiteral("1034#99#99|11:8")}});
  auto futureMetadata = std::make_shared<PetDetailCatalogSnapshot>(*future.metadata);
  futureMetadata->root.insert(QStringLiteral("sacredStarPlans"),QJsonObject{{QStringLiteral("99"),QJsonObject{{QStringLiteral("maxLevel"),12}}}});
  futureMetadata->root.insert(QStringLiteral("sacredStagePlans"),QJsonObject{{QStringLiteral("99"),QJsonObject{{QStringLiteral("maxLevel"),9}}}});
  future.metadata = futureMetadata;
  const PetMetadataView futureView(futureMetadata);
  ShopPetMetadataSnapshot futurePlans{futureView.stargodDefinitions(),futureView.astrolabeDefinitions(),futureView.petDefinitions(),
      futureView.sacredStarPlans(),futureView.sacredStagePlans()};
  PetDetailPreparation futureTask(future); const auto futureDetail = finish(futureTask);
  const auto futureCultivation = deriveShopPet(future.raw->object(),true,futurePlans);
  ok &= check(futureDetail && joined(futureDetail->sacred).contains(QStringLiteral("11/12 星（未满星）")) &&
      joined(futureDetail->sacred).contains(QStringLiteral("8/9 阶（未满阶）")) &&
      futureCultivation.components[7] == ShopCultivationState::Useful && futureCultivation.components[8] == ShopCultivationState::Useful,
      "future official plan 99 did not update detail and shop together");
  auto powerPet = future.raw->object(); QJsonObject completeParts;
  for (const auto* key : {"lv","iv","pl","sgv","ep","gsv","lav","lsv","bsv","asv","sjv"})
    completeParts.insert(QString::fromLatin1(key),QByteArray(key) == "lv" ? 100 : 0);
  powerPet.insert(QStringLiteral("czdlv"),completeParts); powerPet.insert(QStringLiteral("mzdlv"),completeParts);
  powerPet.insert(QStringLiteral("sgs"),QString()); powerPet.insert(QStringLiteral("sgsp"),QJsonArray{});
  powerPet.insert(QStringLiteral("badge"),QString());
  powerPet.insert(QStringLiteral("astrolabe"),QString()); powerPet.insert(QStringLiteral("astrolabebr"),false);
  PetAssetRecord source; source.instanceId = 101; source.raceId = 7001; source.detailAvailable = true; source.pet = powerPet;
  const auto futureFacts = derivePetAnalysisFacts(source,futurePlans);
  ok &= check(futureFacts && futureFacts->battlePower.isHighest && futureFacts->asset.currentPowerKnown &&
      futureFacts->asset.sacredMissing && !futureFacts->asset.fullyCultivated && futureFacts->asset.improvable &&
      futureFacts->asset.gapKeys.contains(QStringLiteral("sacred_star")) && futureFacts->asset.gapKeys.contains(QStringLiteral("sacred_stage")) &&
      petAnalysisFactsFromJson(petAnalysisFactsToJson(*futureFacts)).has_value(),
      "asset progress relied only on an old maximum-power observation or lost its dynamic plan facts on disk");
  auto earlierPlans = futurePlans;
  earlierPlans.sacredStarPlans = QJsonObject{{QStringLiteral("99"),QJsonObject{{QStringLiteral("maxLevel"),11}}}};
  earlierPlans.sacredStagePlans = QJsonObject{{QStringLiteral("99"),QJsonObject{{QStringLiteral("maxLevel"),8}}}};
  const auto earlierFacts = derivePetAnalysisFacts(source,earlierPlans);
  ok &= check(earlierFacts && earlierFacts->asset.fullyCultivated && !earlierFacts->asset.sacredMissing &&
      earlierFacts->eligibility.components[7] == ShopCultivationState::Full && earlierFacts->eligibility.components[8] == ShopCultivationState::Full,
      "the same raw pet did not follow an independently frozen official plan maximum");
  futurePlans.sacredStarPlans = {}; futurePlans.sacredStagePlans = {};
  const auto unknownFacts = derivePetAnalysisFacts(source,futurePlans);
  ok &= check(unknownFacts && unknownFacts->battlePower.hasCurrent && unknownFacts->battlePower.hasHighest &&
      !unknownFacts->asset.cultivationKnown && !unknownFacts->asset.fullyCultivated && !unknownFacts->asset.sacredMissing &&
      unknownFacts->eligibility.components[7] == ShopCultivationState::Unknown &&
      petAnalysisFactsFromJson(petAnalysisFactsToJson(*unknownFacts)).has_value(),
      "unknown plan erased known numeric power or was treated as fully cultivated");
  PetDetailPreparation unknownPlan(fixture({{QStringLiteral("shenjue"),QStringLiteral("1034#99#99|11:8")}}));
  const auto unknownPlanDetail = finish(unknownPlan);
  ok &= check(unknownPlanDetail && unknownPlanDetail->sacred[1].state == DetailKnowledge::Unknown &&
      unknownPlanDetail->sacred[2].state == DetailKnowledge::Unknown &&
      !joined(unknownPlanDetail->sacred).contains(QStringLiteral("满星")),"unknown plan was guessed from a known-era cap");
  ok &= check(PetMetadataView::sacredPlanMaximum({{QStringLiteral("99"),QJsonObject{{QStringLiteral("maxLevel"),12.5}}}},99) == 0 &&
      PetMetadataView::sacredPlanMaximum({{QStringLiteral("99"),QJsonObject{{QStringLiteral("maxLevel"),0}}}},99) == 0,
      "invalid official plan maximum was truncated or treated as full");
  QJsonArray relations; for (int i = 0; i < 70; ++i) relations.append(QString::number(1000 + i)); relations.append(QStringLiteral("1000")); relations.append(1.5); relations.append(9e20);
  QJsonArray carryRelations; for (int i = 0; i < 140; ++i) carryRelations.append(QString::number(2000 + i));
  carryRelations.append(QStringLiteral("2000")); carryRelations.append(1.5); carryRelations.append(9e20);
  PetDetailPreparation foldedCarry(fixture({{QStringLiteral("cepi"),QStringLiteral("9001")},{QStringLiteral("acps"),carryRelations}}));
  const auto folded = finish(foldedCarry);
  ok &= check(folded && folded->pages.last().hasCarryCandidates && !folded->pages.last().carryCandidatesExpanded &&
      folded->pages.last().entries.size() == 1 && folded->pages.last().entries[0].group == QStringLiteral("正在被携带") &&
      folded->version.related.size() == 1,"collapsed carry candidates were resolved eagerly or hid the current carried pet");
  int carryBatch = 0;
  PetDetailPreparation expandedCarry(fixture({{QStringLiteral("cepi"),QStringLiteral("9001")},{QStringLiteral("acps"),carryRelations}}),DetailSection::CarryRelations);
  const auto expanded = finish(expandedCarry,&carryBatch);
  ok &= check(expanded && expanded->pages[0].carryCandidatesExpanded && expanded->pages[0].hasCarryCandidates &&
      expanded->pages[0].entries.size() == 143 && expanded->pages[0].totalItems == 143 &&
      !expanded->pages[0].hasNext() && !expanded->pages[0].hasPrevious() &&
      expanded->pages[0].entries[140].name == QStringLiteral("摘要2139") && expanded->pages[0].entries[140].fields.isEmpty() &&
      carryBatch > 1 && carryBatch <= 32,"carry expansion did not include all names in one bounded, background-prepared view");
  DetailPreparationLimits carryLimit; carryLimit.maximumExpandedCarryItems = 130;
  PetDetailPreparation limitedCarry(fixture({{QStringLiteral("acps"),carryRelations}}),DetailSection::CarryRelations,0,carryLimit);
  ok &= check(!finish(limitedCarry) && limitedCarry.error().contains(QStringLiteral("完整展开数量")),"oversized carry expansion silently truncated its result");
  int batch = 0; PetDetailPreparation related(fixture({{QStringLiteral("asps"),relations}}),DetailSection::SummonRelations,0); const auto r = finish(related,&batch);
  ok &= check(r && r->pages[0].entries.size() == 64 && r->pages[0].totalItems == 72 && batch <= 32 && batch > 1 &&
      r->version.related.size() == 64 && r->pages[0].entries[0].name == QStringLiteral("摘要1000") && r->pages[0].entries[0].fields[3].state == DetailKnowledge::Invalid,"relation batching, strict ID/dedup or per-field unknown semantics failed");
  PetDetailPreparation tail(fixture({{QStringLiteral("asps"),relations}}),DetailSection::SummonRelations,1); const auto t = finish(tail);
  ok &= check(t && t->pages[0].entries.size() == 8 && t->pages[0].state == DetailKnowledge::Invalid &&
      t->pages[0].entries[0].name == QStringLiteral("摘要1064") && t->pages[0].entries[6].state == DetailKnowledge::Invalid,"invalid relationship IDs were resolved, reordered, or later relation page was lost");
  PetDetailPreparation zeroRelation(fixture({{QStringLiteral("sepi"),0}}),DetailSection::SummonRelations); const auto none = finish(zeroRelation);
  ok &= check(none && none->pages[0].state == DetailKnowledge::Known && none->pages[0].totalItems == 0,"explicit no-relation sentinel became an invalid identity or missing field");
  PetDetailPreparation fallbackRelation(fixture({{QStringLiteral("sepi"),0},{QStringLiteral("sdpi"),QStringLiteral("8001")}}),DetailSection::SummonRelations); const auto fallback = finish(fallbackRelation);
  ok &= check(fallback && fallback->pages[0].totalItems == 1 && fallback->version.related[0].instanceId == 8001,"secondary summon instance fallback was lost");
  PetDetailPreparation conflictingRelation(fixture({{QStringLiteral("sepi"),QStringLiteral("8001")},{QStringLiteral("sppl"),QJsonObject{{QStringLiteral("id"),QStringLiteral("8002")},{QStringLiteral("n"),QStringLiteral("conflicting embedded")}}}}),DetailSection::SummonRelations); const auto conflict = finish(conflictingRelation);
  ok &= check(conflict && conflict->version.related.isEmpty() && conflict->pages[0].entries[0].state == DetailKnowledge::Invalid,"contradictory embedded/reference IDs consulted an unrelated summary");
  std::atomic_bool cancel{true}; PetDetailPreparation cancelled(fixture()); ok &= check(cancelled.step(cancel) == DetailStep::Cancelled && !cancelled.takeResult(),"cancelled pure preparation published");
  auto stale = fixture(); auto changed = std::make_shared<PetDetailCatalogSnapshot>(*stale.metadata); changed->revision = 2; stale.metadata = changed;
  PetDetailPreparation staleTask(stale); std::atomic_bool noCancel{false}; ok &= check(staleTask.step(noCancel) == DetailStep::Failed,"mismatched metadata facts were accepted");
  if (ok) std::puts("PASS: frozen detail facts, independent unknown values, original-position talent, sacred zero, all pages and strict related summaries");
  return ok ? 0 : 1;
}
