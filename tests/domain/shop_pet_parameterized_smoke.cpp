#include "domain/shop_pet_eligibility.h"
#include "domain/pet_analysis_facts.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <cstdio>

namespace {
bool check(bool value, const char* message) { if (!value) std::fprintf(stderr,"FAIL: %s\n",message); return value; }
ShopPetMetadataSnapshot metadata() {
  const auto star = [](int type,int quality,bool changeable) {
    QJsonObject levels; for (int level = 1; level <= 8; ++level) levels.insert(QString::number(level),level * (quality == 6 ? 80 : 50));
    return QJsonObject{{QStringLiteral("type"),type},{QStringLiteral("quality"),quality},
        {QStringLiteral("changeable"),changeable},{QStringLiteral("limitJobs"),QJsonArray{}},{QStringLiteral("battlePower"),levels}};
  };
  ShopPetMetadataSnapshot value;
  value.stargods = {{QStringLiteral("10"),star(1,6,false)},{QStringLiteral("11"),star(2,6,false)},
      {QStringLiteral("12"),star(2,5,false)},{QStringLiteral("79"),star(24,5,true)},{QStringLiteral("80"),star(25,6,true)}};
  value.pets = {{QStringLiteral("100"),QJsonObject{{QStringLiteral("stargodSlotMaxLevel"),8},{QStringLiteral("sign"),QStringLiteral("启元,星迹,神运,灵初")},{QStringLiteral("astrolabeBreakCosts"),QStringLiteral("8:51:100")}}}};
  value.badges = {{QStringLiteral("101"),QJsonObject{{QStringLiteral("maxLevel"),6}}},
      {QStringLiteral("102"),QJsonObject{{QStringLiteral("maxLevel"),7}}}};
  const auto trace = [](bool exclusive, bool tbd) { return QJsonObject{{QStringLiteral("exclusive"),exclusive},
      {QStringLiteral("isTBD"),tbd},{QStringLiteral("battlePower"),tbd ? 0 : 100},{QStringLiteral("locatedType"),2}}; };
  value.astrolabe = {{QStringLiteral("1"),trace(false,false)},{QStringLiteral("2"),trace(false,false)},
      {QStringLiteral("3"),trace(true,false)},{QStringLiteral("4"),trace(false,true)}};
  value.sacredStarPlans = {{QStringLiteral("99"),QJsonObject{{QStringLiteral("maxLevel"),12}}}};
  value.sacredStagePlans = {{QStringLiteral("99"),QJsonObject{{QStringLiteral("maxLevel"),9},
      {QStringLiteral("levels"),QJsonObject{{QStringLiteral("2"),QJsonObject{{QStringLiteral("equipmentCount"),0}}},
          {QStringLiteral("3"),QJsonObject{{QStringLiteral("equipmentCount"),2}}}}}}}};
  return value;
}
QJsonObject pet() {
  QJsonObject parts;
  for (const auto* key : {"lv","iv","pl","sgv","ep","gsv","lav","lsv","bsv","asv","sjv"}) parts.insert(QString::fromLatin1(key),0);
  parts.insert(QStringLiteral("lv"),100);
  return {{QStringLiteral("id"),123},{QStringLiteral("r"),100},{QStringLiteral("sgs"),QStringLiteral("10:8#0:8#-2:2:79")},
      {QStringLiteral("sgsp"),QJsonArray{}},{QStringLiteral("czdlv"),parts},{QStringLiteral("mzdlv"),parts},
      {QStringLiteral("astrolabe"),QString()},{QStringLiteral("astrolabebr"),false}};
}
ShopPetEligibilityState state(const QString& code,const QJsonObject& pet,const ShopPetMetadataSnapshot& metadata) {
  return evaluateShopPetRule(compileShopPetRule(code),deriveShopPet(pet,true,metadata)).state;
}
}
int main(int argc, char** argv) {
  QCoreApplication app(argc,argv); bool ok = true; auto m = metadata(); auto p = pet();
  using S = ShopPetEligibilityState;
  for (const auto& expression : QStringList{QStringLiteral("11"),QStringLiteral("11-62-31-41-42-86-91-94"),
       QStringLiteral("11-62-31-91"),QStringLiteral("11-62-31-91-41"),QStringLiteral("11-62-31-91-41-85"),
       QStringLiteral("11-62-31-91-94-41-42-86"),QStringLiteral("11-62-32-91-94-41-42-86-89"),
       QStringLiteral("11-91-31-62-41"),QStringLiteral("31"),QStringLiteral("33"),QStringLiteral("33$1"),
       QStringLiteral("34"),QStringLiteral("39"),QStringLiteral("39$1"),QStringLiteral("39$3"),QStringLiteral("41"),QStringLiteral("41-42"),
       QStringLiteral("43"),QStringLiteral("44"),QStringLiteral("62"),QStringLiteral("84"),QStringLiteral("85"),
       QStringLiteral("89"),QStringLiteral("89$1"),QStringLiteral("91"),QStringLiteral("91-94"),QStringLiteral("92"),QStringLiteral("95")}) {
    for (const auto& component : compileShopPetRule(expression).components)
      ok &= check(component.componentIndex >= 0,"a current official offer expression still has an unsupported component");
  }
  const auto parsed = compileShopPetRule(QStringLiteral("33$5-39$1"));
  ok &= check(parsed.components.size() == 2 && parsed.components[0].code == QStringLiteral("33$5") &&
      parsed.components[0].targetLevel == 5 && parsed.components[1].code == QStringLiteral("39$1"),
      "official parameter strings were stripped or target level was discarded");
  ok &= check(state(QStringLiteral("33"),p,m) == S::Usable && state(QStringLiteral("33$1"),p,m) == S::Usable &&
      state(QStringLiteral("39"),p,m) == S::Usable && state(QStringLiteral("39$1"),p,m) == S::Usable &&
      state(QStringLiteral("39$3"),p,m) == S::Usable,
      "default/level-one red and changeable offers did not recognize real missing resources");
  p.insert(QStringLiteral("sgsp"),QJsonArray{11,80});
  ok &= check(state(QStringLiteral("39$1"),p,m) == S::NotUsable && state(QStringLiteral("33$1"),p,m) == S::NotUsable &&
      state(QStringLiteral("33$2"),p,m) == S::NotUsable && state(QStringLiteral("33$5"),p,m) == S::Usable,
      "owned backpack reds were ignored or parameterized minimum-slot-level upgrade was lost");
  p.insert(QStringLiteral("sgs"),QStringLiteral("10:8#11:8#10:5:79"));
  ok &= check(state(QStringLiteral("33$5"),p,m) == S::NotUsable && state(QStringLiteral("33$6"),p,m) == S::Usable,
      "mapped changeable slot level was confused with its appearance or source");
  p.remove(QStringLiteral("sgsp"));
  ok &= check(state(QStringLiteral("33$1"),p,m) == S::Unknown && state(QStringLiteral("39$1"),p,m) == S::Unknown,
      "missing backpack became empty or fully owned");
  p = pet(); p.insert(QStringLiteral("sgs"),QStringLiteral("malformed"));
  ok &= check(state(QStringLiteral("33$1"),p,m) == S::Unknown,"unknown changeable slot/level was treated as an upgrade opportunity");
  p = pet(); p.insert(QStringLiteral("sgs"),QStringLiteral("10:8#11:8"));
  ok &= check(state(QStringLiteral("33$5"),p,m) == S::NotUsable,"a pet without a changeable slot was recommended changeable stars");
  for (const auto* raw : {"33$0","33$-1","33$1:2","33$","33$2147483648","39$2","39$1:2"})
    ok &= check(state(QString::fromLatin1(raw),pet(),m) == S::Unknown,"unsupported or malformed parameter was guessed");

  p = pet(); p.insert(QStringLiteral("sgsp"),QJsonArray{11,80});
  ok &= check(state(QStringLiteral("32"),p,m) == S::Usable && state(QStringLiteral("31"),p,m) == S::Usable,
      "full-star offers ignored remaining slot levels when all stars were already owned");
  p.insert(QStringLiteral("sgs"),QStringLiteral("10:8#11:8#10:8:79"));
  ok &= check(state(QStringLiteral("32"),p,m) == S::NotUsable && state(QStringLiteral("31"),p,m) == S::NotUsable,
      "fully trained backpack-owned reds triggered a duplicate full-star purchase");
  p.insert(QStringLiteral("sgsp"),QJsonArray{});
  ok &= check(state(QStringLiteral("32"),p,m) == S::Usable,"full red-star set failed to include the missing red changeable star");

  p = pet(); p.insert(QStringLiteral("badge"),QStringLiteral("101:6#201:0|102:7#202:1"));
  ok &= check(state(QStringLiteral("41"),p,m) == S::NotUsable && state(QStringLiteral("43"),p,m) == S::NotUsable &&
      state(QStringLiteral("42"),p,m) == S::Usable && state(QStringLiteral("44"),p,m) == S::Usable,
      "badge level and exclusive activation were conflated through the combined battle-power field");
  p.insert(QStringLiteral("badge"),QStringLiteral("101:5#201:1|102:7#0:0"));
  ok &= check(state(QStringLiteral("43"),p,m) == S::Usable && state(QStringLiteral("41"),p,m) == S::Usable &&
      state(QStringLiteral("42"),p,m) == S::NotUsable,"ordinary badge level deficit or explicit absence of an exclusive badge was misread");
  p.insert(QStringLiteral("badge"),QStringLiteral("101:6"));
  ok &= check(state(QStringLiteral("42"),p,m) == S::NotUsable,"official omitted-exclusive encoding was treated as unawakened");
  p.insert(QStringLiteral("badge"),QStringLiteral("999:6#201:1"));
  ok &= check(state(QStringLiteral("43"),p,m) == S::Unknown,"unknown future badge maximum was guessed");

  p = pet(); p.insert(QStringLiteral("astrolabe"),QStringLiteral("1:0:0#2:1:0#3:0:0#4:0:0"));
  ok &= check(state(QStringLiteral("85"),p,m) == S::Usable && state(QStringLiteral("86"),p,m) == S::Usable &&
      state(QStringLiteral("89"),p,m) == S::NotUsable && state(QStringLiteral("85-89"),p,m) == S::Usable,
      "ordinary light-up or breakthrough prerequisite supplied by the same package was misclassified");
  p.insert(QStringLiteral("astrolabe"),QStringLiteral("1:1:0#2:1:0#3:0:0#4:0:0"));
  ok &= check(state(QStringLiteral("85"),p,m) == S::NotUsable && state(QStringLiteral("86"),p,m) == S::Usable &&
      state(QStringLiteral("89"),p,m) == S::Usable,"exclusive/TBD traces were included in the ordinary light-up requirement");
  p.insert(QStringLiteral("astrolabe"),QStringLiteral("1:1:0#2:1:0#3:1:0#4:0:0"));
  ok &= check(state(QStringLiteral("84"),p,m) == S::NotUsable && state(QStringLiteral("86"),p,m) == S::NotUsable,
      "unreleased TBD trace was recommended for purchase");
  p.insert(QStringLiteral("astrolabebr"),true);
  ok &= check(state(QStringLiteral("89"),p,m) == S::NotUsable,"already unlocked breakthrough with fewer selected stars was recommended again");
  auto unknownAstrolabe = m; auto node = unknownAstrolabe.astrolabe.value(QStringLiteral("1")).toObject(); node.remove(QStringLiteral("isTBD"));
  unknownAstrolabe.astrolabe.insert(QStringLiteral("1"),node);
  p = pet(); p.insert(QStringLiteral("astrolabe"),QStringLiteral("1:0:0"));
  ok &= check(state(QStringLiteral("85"),p,unknownAstrolabe) == S::Unknown && state(QStringLiteral("86"),p,unknownAstrolabe) == S::Unknown,
      "unknown trace release state was guessed as available");
  auto noBreak = m; auto species = noBreak.pets.value(QStringLiteral("100")).toObject(); species.insert(QStringLiteral("astrolabeBreakCosts"),QString());
  noBreak.pets.insert(QStringLiteral("100"),species);
  ok &= check(state(QStringLiteral("89"),p,noBreak) == S::NotUsable,"a species with no official breakthrough feature was offered one");
  species.remove(QStringLiteral("astrolabeBreakCosts")); noBreak.pets.insert(QStringLiteral("100"),species);
  ok &= check(state(QStringLiteral("89"),p,noBreak) == S::Unknown,"missing breakthrough capability data was guessed");

  p = pet(); p.insert(QStringLiteral("shenjue"),QStringLiteral("1034#99#99|11:2"));
  ok &= check(state(QStringLiteral("95"),p,m) == S::Usable,"known next stage with zero source-beast material was not eligible for type 95");
  ok &= check(state(QStringLiteral("94"),p,m) == S::Usable,"full-stage offer ignored a dynamic plan deficit");
  p.insert(QStringLiteral("shenjue"),QStringLiteral("1034#99#99|11:3"));
  const auto excluded = describeShopPetRule(compileShopPetRule(QStringLiteral("95")),deriveShopPet(p,true,m));
  ok &= check(excluded.state == S::NotUsable && excluded.reason.contains(QStringLiteral("当前升阶需要源兽材料")),
      "source-beast-consuming stage was allowed through the inexpensive branch or described as fully cultivated");
  p.insert(QStringLiteral("shenjue"),QStringLiteral("1034#99#99|11:9"));
  ok &= check(state(QStringLiteral("95"),p,m) == S::NotUsable,"already full sacred stage was eligible for another stage");
  ok &= check(state(QStringLiteral("94"),p,m) == S::NotUsable,"full-stage offer remained useful after reaching the dynamic maximum");
  p.insert(QStringLiteral("shenjue"),QStringLiteral("1034#99#99|11:4"));
  ok &= check(state(QStringLiteral("95"),p,m) == S::Unknown,"missing stage material data was guessed as zero");
  p.insert(QStringLiteral("shenjue"),QStringLiteral("1034#999#999|11:2"));
  ok &= check(state(QStringLiteral("95"),p,m) == S::Unknown,"unknown future plan acquired a material-free upgrade");

  p = pet(); p.insert(QStringLiteral("sgsp"),QJsonArray{11,80}); p.insert(QStringLiteral("shenjue"),QStringLiteral("1034#99#99|11:2"));
  PetAssetRecord seed; seed.instanceId = 123; seed.raceId = 100; seed.detailAvailable = true; seed.pet = p;
  const auto facts = derivePetAnalysisFacts(seed,m);
  ok &= check(bool(facts),"parameterized offer fact fixture failed");
  if (facts) {
    const auto serialized = petAnalysisFactsToJson(*facts); const auto restored = petAnalysisFactsFromJson(serialized);
    ok &= check(restored && restored->eligibility.components.size() == kShopCultivationComponentCount &&
        restored->eligibility.changeableLevelKnown && restored->eligibility.changeableLevel == 2 &&
        evaluateShopPetRule(compileShopPetRule(QStringLiteral("33$5")),restored->eligibility).state == S::Usable &&
        evaluateShopPetRule(compileShopPetRule(QStringLiteral("33$1")),restored->eligibility).state == S::NotUsable &&
        evaluateShopPetRule(compileShopPetRule(QStringLiteral("95")),restored->eligibility).state == S::Usable,
        "disk facts lost target-level interpretation or the inexpensive-stage component");
    auto broken = serialized; auto entry = broken.value(QStringLiteral("eligibility")).toObject();
    auto components = entry.value(QStringLiteral("components")).toArray(); components.removeLast();
    entry.insert(QStringLiteral("components"),components); broken.insert(QStringLiteral("eligibility"),entry);
    ok &= check(!petAnalysisFactsFromJson(broken),"old shorter component layout was accepted as the current facts schema");
  }
  p = pet(); p.insert(QStringLiteral("sgs"),QString()); p.insert(QStringLiteral("sgsp"),QJsonArray{});
  p.insert(QStringLiteral("badge"),QStringLiteral("101:5#201:1")); seed.pet = p;
  auto earlierBadge = m; earlierBadge.badges.insert(QStringLiteral("101"),QJsonObject{{QStringLiteral("maxLevel"),5}});
  const auto oldBadge = derivePetAnalysisFacts(seed,earlierBadge), newBadge = derivePetAnalysisFacts(seed,m);
  ok &= check(oldBadge && oldBadge->asset.fullyCultivated && newBadge && newBadge->battlePower.isHighest &&
      newBadge->asset.currentPowerKnown && newBadge->asset.highestPowerKnown && !newBadge->asset.fullyCultivated &&
      newBadge->asset.soulMissing && newBadge->asset.improvable && newBadge->asset.gapKeys.contains(QStringLiteral("badge_level")) &&
      evaluateShopPetRule(compileShopPetRule(QStringLiteral("43")),newBadge->eligibility).state == S::Usable &&
      petAnalysisFactsFromJson(petAnalysisFactsToJson(*newBadge)).has_value(),
      "new official badge maximum was ignored by assets because the old numeric battle power was full");
  p.insert(QStringLiteral("badge"),QString()); p.insert(QStringLiteral("astrolabe"),QStringLiteral("1:1:0#2:0:0")); seed.pet = p;
  auto futureAstrolabe = m;
  futureAstrolabe.pets.insert(QStringLiteral("100"),QJsonObject{{QStringLiteral("stargodSlotMaxLevel"),8},{QStringLiteral("sign"),QStringLiteral("灵初")},{QStringLiteral("astrolabeBreakCosts"),QString()}});
  const QJsonObject zeroPowerTrace{{QStringLiteral("exclusive"),false},{QStringLiteral("isTBD"),false},
      {QStringLiteral("battlePower"),0},{QStringLiteral("locatedType"),1}};
  futureAstrolabe.astrolabe.insert(QStringLiteral("1"),zeroPowerTrace);
  futureAstrolabe.astrolabe.insert(QStringLiteral("2"),zeroPowerTrace);
  auto oldAstrolabeMetadata = futureAstrolabe; auto unreleased = zeroPowerTrace; unreleased.insert(QStringLiteral("isTBD"),true);
  oldAstrolabeMetadata.astrolabe.insert(QStringLiteral("2"),unreleased);
  const auto oldAstrolabe = derivePetAnalysisFacts(seed,oldAstrolabeMetadata), newAstrolabe = derivePetAnalysisFacts(seed,futureAstrolabe);
  ok &= check(oldAstrolabe && oldAstrolabe->asset.fullyCultivated && newAstrolabe && newAstrolabe->battlePower.isHighest &&
      newAstrolabe->asset.currentPowerKnown && newAstrolabe->asset.highestPowerKnown && !newAstrolabe->asset.fullyCultivated &&
      newAstrolabe->asset.astrolabeMissing && newAstrolabe->asset.improvable && newAstrolabe->asset.gapKeys.contains(QStringLiteral("astrolabe_light")) &&
      evaluateShopPetRule(compileShopPetRule(QStringLiteral("85")),newAstrolabe->eligibility).state == S::Usable &&
      petAnalysisFactsFromJson(petAnalysisFactsToJson(*newAstrolabe)).has_value(),
      "newly available official trace was hidden from assets and shop by unchanged numeric battle power");
  p.insert(QStringLiteral("astrolabe"),QStringLiteral("1:1:0#2:1:0")); p.remove(QStringLiteral("astrolabebr"));
  for (const auto& era : {QStringLiteral("神运"),QStringLiteral("星迹"),QStringLiteral("启元"),QStringLiteral("其他")}) {
    auto eraMetadata = futureAstrolabe;
    auto species = eraMetadata.pets.value(QStringLiteral("100")).toObject();
    species.insert(QStringLiteral("era"),era); species.insert(QStringLiteral("astrolabeBreakCosts"),QStringLiteral("8:51:100"));
    eraMetadata.pets.insert(QStringLiteral("100"),species); seed.pet = p;
    const auto eraFacts = derivePetAnalysisFacts(seed,eraMetadata);
    ok &= check(eraFacts && eraFacts->asset.fullyCultivated && !eraFacts->asset.astrolabeMissing &&
        !eraFacts->asset.gapKeys.contains(QStringLiteral("astrolabe_breakthrough")) &&
        evaluateShopPetRule(compileShopPetRule(QStringLiteral("89")),eraFacts->eligibility).state == S::NotUsable &&
        eraFacts->battlePower.astrolabeBonus == 0 && eraFacts->battlePower.astrolabeTargetBonus == 0 &&
        petAnalysisFactsFromJson(petAnalysisFactsToJson(*eraFacts)).has_value(),
        "non-Lingchu era gained a breakthrough shop recommendation, asset gap or required response field");
  }
  auto unknownEraMetadata = futureAstrolabe;
  auto unknownSpecies = unknownEraMetadata.pets.value(QStringLiteral("100")).toObject();
  unknownSpecies.insert(QStringLiteral("era"),QStringLiteral("未来未知时代"));
  unknownEraMetadata.pets.insert(QStringLiteral("100"),unknownSpecies); seed.pet = p;
  const auto unknownEraFacts = derivePetAnalysisFacts(seed,unknownEraMetadata);
  ok &= check(unknownEraFacts && !unknownEraFacts->asset.highestPowerKnown && !unknownEraFacts->asset.fullyCultivated &&
      !unknownEraFacts->asset.gapKeys.contains(QStringLiteral("astrolabe_breakthrough")) &&
      evaluateShopPetRule(compileShopPetRule(QStringLiteral("89")),unknownEraFacts->eligibility).state == S::Unknown,
      "unknown era was promoted to Lingchu or shown as definitely missing breakthrough");
  // New branches use actual official effects; existing assertions above remain.
  for (const auto& code : {QStringLiteral("34$1"),QStringLiteral("34$7")}) {
    auto missing=pet(), owned=pet(); owned.insert(QStringLiteral("sgsp"),QJsonArray{11});
    ok &= check(state(code,missing,m)==state(QStringLiteral("34"),missing,m) &&
        state(code,owned,m)==state(QStringLiteral("34"),owned,m),"parameterized red star diverged from existing acquisition judgment");
  }
  p=pet(); p.insert(QStringLiteral("gt"),5);
  ok &= check(state(QStringLiteral("61"),p,m)==S::Usable,"gift level below Chaofan was not useful");
  p.insert(QStringLiteral("gt"),6);
  ok &= check(state(QStringLiteral("61"),p,m)==S::NotUsable,"completed Chaofan gift was offered again");
  p.remove(QStringLiteral("gt"));
  ok &= check(state(QStringLiteral("61"),p,m)==S::Unknown,"missing gift grade was invented");
  p=pet(); p.insert(QStringLiteral("shenjue"),QStringLiteral("1034#99#99|11:8"));
  for(const auto& code:{QStringLiteral("92$1"),QStringLiteral("92$3")})
    ok &= check(state(code,p,m)==S::Usable,"stage increment was confused with an absolute target stage");
  p.insert(QStringLiteral("shenjue"),QStringLiteral("1034#99#99|11:9"));
  ok &= check(state(QStringLiteral("92$1"),p,m)==S::NotUsable,"stage increment exceeded the official plan maximum");

  p=pet();
  ok &= check(state(QStringLiteral("35$10"),p,m)==S::NotUsable && state(QStringLiteral("35$11"),p,m)==S::Usable,
      "specified red star ownership was replaced by a generic missing-red count");
  p.insert(QStringLiteral("sgsp"),QJsonArray{11});
  ok &= check(state(QStringLiteral("35$11"),p,m)==S::NotUsable,"specified red star in this pet's backpack was ignored");
  p.insert(QStringLiteral("sgsp"),QJsonArray{QStringLiteral("11")});
  ok &= check(state(QStringLiteral("35$11"),p,m)==S::NotUsable,"numeric-string backpack identity was treated as an unowned star");
  p=pet(); p.remove(QStringLiteral("sgsp"));
  ok &= check(state(QStringLiteral("35$11"),p,m)==S::Unknown && state(QStringLiteral("35$10"),p,m)==S::NotUsable,
      "unknown backpack was treated as empty or equipped ownership was lost");
  auto restricted=m; auto star=restricted.stargods.value(QStringLiteral("11")).toObject();
  star.insert(QStringLiteral("limitJobs"),QJsonArray{9}); restricted.stargods.insert(QStringLiteral("11"),star);
  auto restrictedPet=restricted.pets.value(QStringLiteral("100")).toObject();
  restrictedPet.insert(QStringLiteral("jobs"),QJsonArray{9}); restricted.pets.insert(QStringLiteral("100"),restrictedPet);
  p=pet();
  ok &= check(state(QStringLiteral("35$11"),p,restricted)==S::NotUsable &&
      describeShopPetRule(compileShopPetRule(QStringLiteral("35$11")),deriveShopPet(p,true,restricted)).reason.contains(QStringLiteral("职业")),
      "specified star ignored or misdescribed the official forbidden-job list");
  p=pet(); p.insert(QStringLiteral("sgs"),QStringLiteral("-2:1"));
  ok &= check(state(QStringLiteral("35$11"),p,m)==S::NotUsable,"ordinary specified star was assigned to a changeable-only pet");

  auto traces=m; auto inner=traces.astrolabe.value(QStringLiteral("1")).toObject();
  inner.insert(QStringLiteral("locatedType"),1); traces.astrolabe.insert(QStringLiteral("1"),inner);
  p=pet(); p.insert(QStringLiteral("astrolabe"),QStringLiteral("1:0:0#2:0:0|3:0:0|4:0:0"));
  auto ast=deriveShopPet(p,true,traces);
  ok &= check(state(QStringLiteral("81"),p,traces)==S::Usable && state(QStringLiteral("82"),p,traces)==S::NotUsable &&
      state(QStringLiteral("83"),p,traces)==S::Usable && describeShopPetRule(compileShopPetRule(QStringLiteral("82")),ast).reason.contains(QStringLiteral("前置")),
      "single inner/outer/exclusive node scope or predecessor condition was conflated");
  p.insert(QStringLiteral("astrolabe"),QStringLiteral("1:1:0#2:0:0|3:1:0|4:0:0"));
  ok &= check(state(QStringLiteral("81"),p,traces)==S::NotUsable && state(QStringLiteral("82"),p,traces)==S::Usable &&
      state(QStringLiteral("83"),p,traces)==S::NotUsable,"lit/exclusive/TBD trace was counted in the wrong reward");
  p.insert(QStringLiteral("astrolabe"),QStringLiteral("4:0:0"));
  for(const auto& code:{QStringLiteral("81"),QStringLiteral("82"),QStringLiteral("83")})
    ok &= check(state(code,p,traces)==S::NotUsable,"unreleased trace was offered for single-node activation");
  p.insert(QStringLiteral("astrolabe"),QStringLiteral("99999:0:0"));
  ok &= check(state(QStringLiteral("81"),p,traces)==S::Unknown,"unknown node definition was classified as completed");
  for(const auto* raw:{"34$0","34$1:2","34$2147483648","35$0","35$12","35$99999","92$0","92$1:2"})
    ok &= check(state(QString::fromLatin1(raw),pet(),m)==S::Unknown,"invalid or unknown parameter acquired an automatic judgment");
  p=pet(); p.insert(QStringLiteral("gt"),5); p.insert(QStringLiteral("astrolabe"),QStringLiteral("1:0:0#2:0:0|3:0:0"));
  seed.pet=p; const auto addedFacts=derivePetAnalysisFacts(seed,traces);
  const auto addedRestored=addedFacts ? petAnalysisFactsFromJson(petAnalysisFactsToJson(*addedFacts)) : std::nullopt;
  ok &= check(addedRestored && evaluateShopPetRule(compileShopPetRule(QStringLiteral("35$11")),addedRestored->eligibility).state==S::Usable &&
      evaluateShopPetRule(compileShopPetRule(QStringLiteral("61")),addedRestored->eligibility).state==S::Usable &&
      describeShopPetRule(compileShopPetRule(QStringLiteral("82")),addedRestored->eligibility).reason.contains(QStringLiteral("前置")),
      "persisted facts dropped new ownership, gift or predecessor judgments");
  if (addedFacts) {
    auto malformed=petAnalysisFactsToJson(*addedFacts); auto details=malformed.value(QStringLiteral("eligibility")).toObject();
    details.insert(QStringLiteral("usefulSpecifiedStars"),QJsonArray{11,11}); malformed.insert(QStringLiteral("eligibility"),details);
    ok &= check(!petAnalysisFactsFromJson(malformed),"duplicate specified-star facts survived cache validation");
  }
  p=pet();
  for (const auto& code : QStringList{"35$11:1","35$11:0:6"}) {
    const auto rule=compileShopPetRule(code);
    ok &= check(state(code,p,m)==state(QStringLiteral("35$11"),p,m) &&
        rule.components[0].code==code && rule.components[0].targetId==11,
        "specified-star trailing numeric fields changed the first-ID meaning or lost the raw expression");
  }
  for (const auto& code : QStringList{"35$11:","35$11:x","25$0","53$0","25$2147483648","37$"})
    ok &= check(state(code,p,m)==S::Unknown,"invalid generic cultivation parameter acquired a judgment");

  auto legacy=m; auto legacyDefinition=legacy.pets.value(QStringLiteral("100")).toObject();
  legacyDefinition.insert(QStringLiteral("sign"),QStringLiteral("神职,传说,天启,启元,星迹"));
  legacy.pets.insert(QStringLiteral("100"),legacyDefinition);
  p=pet(); p.insert(QStringLiteral("eps"),QStringLiteral("0,-1,0"));
  ok &= check(state(QStringLiteral("25$6"),p,legacy)==S::Usable,"empty source-beast slots did not accept common equipment");
  p.insert(QStringLiteral("eps"),QStringLiteral("1|0|6|0|,43|0|5|0|,6|0|6|0|"));
  ok &= check(state(QStringLiteral("25$6"),p,legacy)==S::Usable && state(QStringLiteral("25$5"),p,legacy)==S::NotUsable,
      "common equipment target level was ignored or the current exclusive upgrade was lost");
  p.insert(QStringLiteral("eps"),QStringLiteral("1|0|6|0|,43|0|6|0|,6|0|6|0|"));
  ok &= check(state(QStringLiteral("25"),p,legacy)==S::NotUsable,"full source-beast slots were offered another level-six equipment");
  p.remove(QStringLiteral("eps"));
  ok &= check(state(QStringLiteral("25$6"),p,legacy)==S::Unknown,"missing equipment detail was treated as empty");
  p.insert(QStringLiteral("eps"),QStringLiteral("0,0"));
  ok &= check(state(QStringLiteral("25$6"),p,legacy)==S::Unknown,"truncated equipment slots were silently completed");
  p.insert(QStringLiteral("eps"),QStringLiteral("0,0,0"));
  ok &= check(state(QStringLiteral("25$6"),p,m)==S::NotUsable,"Shenyun/Lingchu pet accepted obsolete common equipment");

  p.insert(QStringLiteral("lss"),QStringLiteral("-1:0:0:0,-1:0:0:0,-1:0:0:0,-1:0:0:0,-1:0:0:0"));
  ok &= check(state(QStringLiteral("53$5"),p,legacy)==S::Usable,"empty legend-stone slots did not accept a single stone");
  p.insert(QStringLiteral("lss"),QStringLiteral("1:2:5:0,2:2:5:0,3:2:4:0,4:2:5:0,5:2:5:0"));
  ok &= check(state(QStringLiteral("53$5"),p,legacy)==S::Usable && state(QStringLiteral("53$4"),p,legacy)==S::NotUsable,
      "legend-stone target level was replaced with a generic power gap");
  p.insert(QStringLiteral("lss"),QStringLiteral("1:2,2:2,3:2,4:2,5:2"));
  ok &= check(state(QStringLiteral("53"),p,legacy)==S::NotUsable,"official omitted stone-level default five was lost");
  p.remove(QStringLiteral("lss"));
  ok &= check(state(QStringLiteral("53$5"),p,legacy)==S::Unknown,"missing legend-stone detail was treated as empty");
  p.insert(QStringLiteral("lss"),QStringLiteral("1:2:5,2:2:5,3:2:5"));
  ok &= check(state(QStringLiteral("53$5"),p,legacy)==S::Unknown,"truncated legend-stone slots were silently completed");
  ok &= check(state(QStringLiteral("53$5"),p,m)==S::NotUsable,"Shenyun/Lingchu pet accepted obsolete legend stones");
  auto unknownEra=legacy; unknownEra.pets={};
  ok &= check(state(QStringLiteral("25$6"),p,unknownEra)==S::Unknown && state(QStringLiteral("53$5"),p,unknownEra)==S::Unknown,
      "unresolved era was guessed eligible for obsolete cultivation");

  p=pet(); p.insert(QStringLiteral("sgs"),QStringLiteral("10:8#0:8#-2:8:80"));
  ok &= check(state(QStringLiteral("37$6"),p,m)==S::Usable,"missing ordinary gold star did not yield usefulness");
  p.insert(QStringLiteral("sgsp"),QJsonArray{12});
  ok &= check(state(QStringLiteral("37$6"),p,m)==S::NotUsable,"an owned backpack gold star was counted as missing");
  p.insert(QStringLiteral("sgsp"),QJsonArray{}); p.insert(QStringLiteral("sgs"),QStringLiteral("10:8#12:4#-2:8:80"));
  ok &= check(state(QStringLiteral("37$6"),p,m)==S::Usable,"underlevel ordinary gold slot did not accept the level-six reward");
  p.insert(QStringLiteral("sgs"),QStringLiteral("10:8#12:6#-2:1:80"));
  ok &= check(state(QStringLiteral("37"),p,m)==S::NotUsable && state(QStringLiteral("37$9"),p,m)==S::NotUsable,
      "single-gold suffix was invented as a level or the changeable slot was treated as ordinary");
  p.insert(QStringLiteral("sgs"),QStringLiteral("10:1#11:1#-2:1:80"));
  ok &= check(state(QStringLiteral("37$6"),p,m)==S::NotUsable,"a gold reward was recommended as a downgrade of owned red stars");
  p.remove(QStringLiteral("sgsp"));
  ok &= check(state(QStringLiteral("37$6"),p,m)==S::Unknown,"missing star backpack was guessed empty for a gold offer");

  p=pet(); p.insert(QStringLiteral("eps"),QStringLiteral("1|0|6,43|0|4,6|0|6"));
  p.insert(QStringLiteral("lss"),QStringLiteral("1:2:5,2:2:3,3:2:5,4:2:5,5:2:5")); seed.pet=p;
  const auto newcomerFacts=derivePetAnalysisFacts(seed,legacy);
  const auto newcomerRestored=newcomerFacts ? petAnalysisFactsFromJson(petAnalysisFactsToJson(*newcomerFacts)) : std::nullopt;
  ok &= check(newcomerRestored && newcomerRestored->eligibility.minimumEquipmentLevel==4 &&
      newcomerRestored->eligibility.minimumLegendStoneLevel==3 &&
      evaluateShopPetRule(compileShopPetRule(QStringLiteral("25$4")),newcomerRestored->eligibility).state==S::NotUsable &&
      evaluateShopPetRule(compileShopPetRule(QStringLiteral("53$5")),newcomerRestored->eligibility).state==S::Usable,
      "persisted facts lost target-specific source-beast or legend-stone judgments");
  if (newcomerFacts) {
    auto broken=petAnalysisFactsToJson(*newcomerFacts); auto details=broken.value(QStringLiteral("eligibility")).toObject();
    details.insert(QStringLiteral("equipmentLevelsKnown"),false); broken.insert(QStringLiteral("eligibility"),details);
    ok &= check(!petAnalysisFactsFromJson(broken),"contradictory source-beast facts survived validation");
  }
  if (ok) std::puts("PASS: existing cultivation judgments and audited parameterized, specified-star, single-trace and gift rules");
  return ok ? 0 : 1;
}
