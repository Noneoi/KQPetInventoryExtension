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
       QStringLiteral("34"),QStringLiteral("39"),QStringLiteral("39$1"),QStringLiteral("41"),QStringLiteral("41-42"),
       QStringLiteral("43"),QStringLiteral("44"),QStringLiteral("62"),QStringLiteral("84"),QStringLiteral("85"),
       QStringLiteral("89"),QStringLiteral("91"),QStringLiteral("91-94"),QStringLiteral("92"),QStringLiteral("95")}) {
    for (const auto& component : compileShopPetRule(expression).components)
      ok &= check(component.componentIndex >= 0,"a current official offer expression still has an unsupported component");
  }
  const auto parsed = compileShopPetRule(QStringLiteral("33$5-39$1"));
  ok &= check(parsed.components.size() == 2 && parsed.components[0].code == QStringLiteral("33$5") &&
      parsed.components[0].targetLevel == 5 && parsed.components[1].code == QStringLiteral("39$1"),
      "official parameter strings were stripped or target level was discarded");
  ok &= check(state(QStringLiteral("33"),p,m) == S::Usable && state(QStringLiteral("33$1"),p,m) == S::Usable &&
      state(QStringLiteral("39"),p,m) == S::Usable && state(QStringLiteral("39$1"),p,m) == S::Usable,
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
  if (ok) std::puts("PASS: all current official offer types, parameterized red stars, separate badge cultivation, normal/exclusive/TBD traces, breakthrough and dynamic source-beast stages");
  return ok ? 0 : 1;
}
