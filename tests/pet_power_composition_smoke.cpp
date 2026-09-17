#include "domain/pet_power_calculator.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <iostream>

namespace {
bool require(bool ok, const char* message) { if (!ok) std::cerr << message << '\n'; return ok; }
QJsonObject star(int type,int quality,bool changeable=false,QJsonArray limits={}) {
  QJsonObject levels;
  for (int lv=1;lv<=8;++lv) levels.insert(QString::number(lv),quality==6 ? (lv==8 ? 650 : lv==7 ? 570 : 70+lv*70) : lv*50+50);
  return {{"name",QStringLiteral("星神%1").arg(type)},{"type",type},{"quality",quality},{"changeable",changeable},
      {"limitJobs",limits},{"battlePower",levels}};
}
PetPowerMetadata metadata() {
  return {{{"10",star(1,6)},{"11",star(1,5)},{"12",star(1,6)},{"20",star(2,6)},
    {"30",star(3,6,false,{7})},{"79",star(24,5,true)},{"80",star(25,6,true)},
    {"90",QJsonObject{{"type",97},{"quality",1},{"changeable",false},{"limitJobs",QJsonArray{}},{"battlePower",QJsonObject{}}}}},8,
    {{"1",QJsonObject{{"locatedType",2},{"battlePower",100}}},{"2",QJsonObject{{"locatedType",2},{"battlePower",100}}},
     {"3",QJsonObject{{"locatedType",2},{"battlePower",100}}}},{7}};
}
QJsonObject pet() {
  QJsonObject value{{"id",123},{"r",456},{"_metaEra",QStringLiteral("灵初")},{"lv",120},{"zdl",7777},{"xzdl",3000},{"sgs","11:1#0:8#79:1:79"},
    {"sgsp",QJsonArray{10,20,80}},{"astrolabe","1:1:1|2:1:1|3:1:1"},{"astrolabebr",true},
    {"czdlv",QJsonObject{{"lv",1000},{"iv",200},{"sgv",1},{"asv",450}}},
    {"mzdlv",QJsonObject{{"lv",1000},{"iv",200},{"sgv",1350},{"asv",300}}}};
  for(const auto* field:{"czdlv","mzdlv"}) {
    auto parts=value.value(QLatin1String(field)).toObject();
    for(const auto* key:{"lv","iv","pl","sgv","ep","gsv","lav","lsv","bsv","asv","sjv"})
      if(!parts.contains(QLatin1String(key))) parts.insert(QLatin1String(key),0);
    value.insert(QLatin1String(field),parts);
  }
  return value;
}
}
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv); bool ok=true; auto m=metadata(); auto p=pet(); auto v=calculatePetBattlePower(p,m);
  ok &= require(v.hasServerCurrent && v.serverCurrent==7777 && v.extreme==3000 && v.hasHighest && v.highest==3600,
      "official totals must remain distinct from reconstructed supreme");
  ok &= require(v.currentLocallyCalculated && v.current==2580 && v.currentStargodPower==930 &&
      v.equippedStargodPower==200 && v.equippedCurrent==1850 && v.adjustmentGain==730,
      "owned backpack stars must contribute at actual slot levels, including empty slots");
  ok &= require(v.ownedMaxStargodPowerKnown && v.ownedMaxStargodPower==1950 && v.stargodUpgradeGap==1020 &&
      v.stargodAcquisitionKnown && v.stargodAcquisitionGap==0 && v.missingRedStars==0 &&
      v.changeableOwnedRed && !v.changeableRed && !v.isHighest && v.stargodLevelMissingSlots==2,
      "slot upgrades, missing purchases and unused owned red changeable must be distinct");
  ok &= require(v.components.size()==6 && v.stargodDetails.size()==3 && v.stargodDetails[0].slot==1 &&
      v.stargodDetails[1].slot==2 && v.stargodDetails[2].changeable,"detail rows must preserve actual slot positions");
  p.insert("sgs","11:8#20:8#80:8:80"); v=calculatePetBattlePower(p,m);
  ok &= require(v.current==3600 && v.isHighest && v.adjustmentGain>0 && v.redStargodFull,
      "fully trained owned red stars can be complete while an equipment adjustment remains");
  p=pet(); p.insert("sgsp",QJsonArray{10,12,12,30}); v=calculatePetBattlePower(p,m);
  ok &= require(v.stargodAcquisitionKnown && v.redStars==1 && v.missingRedStars==1 && v.availableStars==1 &&
      !v.redStargodFull && v.stargodAcquisitionGap>0,
      "duplicate types and job-incompatible red stars cannot satisfy missing slots");
  p=pet(); p.insert("astrolabe","1:1:1|2:1:1|3:1:0"); v=calculatePetBattlePower(p,m);
  ok &= require(v.astrolabeBonus==0 && v.astrolabeTargetBonus==150 && v.astrolabeCurrent==300 && v.highestGap==1170,
      "breakthrough boolean alone cannot grant the three-selected-node bonus");
  p=pet(); p.insert("astrolabe",""); p.insert("astrolabebr",false);
  auto c=p.value("czdlv").toObject(),x=p.value("mzdlv").toObject(); c.insert("asv",0);x.insert("asv",0);
  p.insert("czdlv",c);p.insert("mzdlv",x);v=calculatePetBattlePower(p,m);
  ok &= require(v.hasHighest && v.highest==3150 && !v.astrolabeApplicable && v.astrolabeTargetBonus==0,
      "pets without astrolabe cannot receive a fabricated 150 supreme bonus");
  p=pet();p.remove("sgsp");v=calculatePetBattlePower(p,m);
  ok &= require(!v.currentLocallyCalculated && !v.stargodAcquisitionKnown && !v.isHighest &&
      v.equippedCurrentKnown && !v.unknownReasons.isEmpty(),"missing backpack must stay unknown, retaining observed/equipped values");
  p=pet();p.remove("zdl");v=calculatePetBattlePower(p,m);
  ok &= require(!v.hasServerCurrent && v.hasCurrent && v.currentLocallyCalculated && v.current==2580,
      "valid local calculation must not depend on server total");
  p=pet();for(const auto* field:{"czdlv","mzdlv"}){auto parts=p.value(QLatin1String(field)).toObject();parts.remove("pl");p.insert(QLatin1String(field),parts);}
  v=calculatePetBattlePower(p,m);ok &= require(!v.hasCurrent && !v.hasHighest && !v.completionKnown && v.hasServerCurrent,
      "matching sparse components must not prove a complete total or fully cultivated pet");
  p=pet();m.slotMaxLevel=0;v=calculatePetBattlePower(p,m);
  ok &= require(v.currentLocallyCalculated && !v.hasHighest && !v.ownedMaxStargodPowerKnown && !v.isHighest,
      "missing maximum slot level must not erase known current or invent supreme");
  m=metadata();p=pet();auto row=m.astrolabe.value("3").toObject();row.remove("battlePower");m.astrolabe.insert("3",row);
  v=calculatePetBattlePower(p,m);ok &= require(!v.hasHighest && !v.astrolabePowerKnown && !v.completionKnown,
      "unknown node power must not be interpreted as zero/full");
  m=metadata();p=pet();auto invalid=p.value("czdlv").toObject();invalid.insert("iv",1.5);p.insert("czdlv",invalid);
  v=calculatePetBattlePower(p,m);ok &= require(!v.currentLocallyCalculated && !v.hasHighest,
      "fractional/invalid components must reject aggregate conclusions");
  p=pet();p.insert("r",456);p.insert("_metaRaceId",999);p.insert("_metaJobs","88");p.insert("rt",77);
  auto resolved=petPowerMetadataFromCatalog(p,0,m.stargods,m.astrolabe,
      {{"456",QJsonObject{{"jobs","7#8-9"},{"stargodSlotMaxLevel",8}}}});
  ok &= require(resolved.slotMaxLevel==8 && resolved.jobs==QJsonArray{7,8,9},"metadata must use exact race jobs, never runtime rt");
  resolved=petPowerMetadataFromCatalog(p,0,m.stargods,m.astrolabe,{});
  ok &= require(resolved.jobs.isEmpty(),"mismatching cached race metadata must not contaminate job constraints");
  for (const auto& era : {QStringLiteral("灵初"),QStringLiteral("神运"),QStringLiteral("星迹"),QStringLiteral("启元"),QStringLiteral("其他")}) {
    p=pet(); p.insert("sgs","11:8#20:8#80:8:80");
    const auto eraMetadata=petPowerMetadataFromCatalog(p,8,m.stargods,metadata().astrolabe,
        {{"456",QJsonObject{{"era",era},{"jobs","7"},{"stargodSlotMaxLevel",8}}}});
    const bool lingchu=era==QStringLiteral("灵初");
    const bool hasAstrolabe=era==QStringLiteral("灵初")||era==QStringLiteral("神运")||era==QStringLiteral("星迹");
    v=calculatePetBattlePower(p,eraMetadata);
    const int expectedHighest = lingchu ? 3600 : hasAstrolabe ? 3450 : 3150;
    QStringList keys; for (const auto& row : v.components) keys.append(row.key);
    QStringList expectedKeys;
    for (const auto& key : {QStringLiteral("lv"),QStringLiteral("iv"),QStringLiteral("pl"),QStringLiteral("sgv"),
                            QStringLiteral("ep"),QStringLiteral("gsv"),QStringLiteral("lav"),QStringLiteral("lsv"),
                            QStringLiteral("bsv"),QStringLiteral("asv"),QStringLiteral("sjv")})
      if (eraHasComponent(parsePetEraName(era), key)) expectedKeys.append(key);
    ok &= require(v.breakthroughApplicabilityKnown && v.breakthroughApplicable==lingchu &&
        v.astrolabeApplicable==hasAstrolabe &&
        v.astrolabeBonus==(lingchu?150:0) && v.astrolabeTargetBonus==(lingchu?150:0) &&
        v.hasHighest && v.highest==expectedHighest && v.current==v.highest && v.isHighest &&
        keys==expectedKeys,
        "era-gated astrolabe leaked into a generation that does not have the system");
    p.remove("astrolabebr"); v=calculatePetBattlePower(p,eraMetadata);
    ok &= require(lingchu ? !v.completionKnown : v.completionKnown && v.hasHighest && v.isHighest && !v.breakthroughKnown,
        "non-Lingchu cultivation required an inapplicable breakthrough response field");
    if (lingchu) { p.insert("astrolabebr",false); v=calculatePetBattlePower(p,eraMetadata);
      ok &= require(v.astrolabeBonus==0 && v.astrolabeTargetBonus==150 && v.current==3450 && !v.isHighest,
          "unbroken Lingchu pet gained current breakthrough bonus from inconsistent selected nodes"); }
  }
  p=pet(); p.insert("n",QStringLiteral("[灵初]误导显示名"));
  auto unknownEra=petPowerMetadataFromCatalog(p,8,metadata().stargods,metadata().astrolabe,
      {{"456",QJsonObject{{"era",QStringLiteral("未知时代")},{"stargodSlotMaxLevel",8}}}});
  v=calculatePetBattlePower(p,unknownEra);
  ok &= require(!v.breakthroughApplicable && v.astrolabeBonus==0 && v.astrolabeTargetBonus==0,
      "unknown authoritative era inherited Lingchu astrolabe");
  ok &= require(resolvePetEra({{"r",456},{"_metaEra",QStringLiteral("灵初")}},
      {{"456",QJsonObject{{"name",QStringLiteral("皮肤名称")},{"sign",QStringLiteral("启元,星迹,神运")}}}})==PetEra::ShenYun &&
      resolvePetEra({{"r",456},{"n",QStringLiteral("[灵初]皮肤")}},{{"456",QJsonObject{{"sign",QString()}}}})==PetEra::Other &&
      resolvePetEra({{"r",456}},{{"456",QJsonObject{{"name",QStringLiteral("皮肤名称")},{"sign",QStringLiteral("启元,星迹,神运,灵初,皮肤")}}}})==PetEra::LingChu,
      "official current-race sign failed to control skin era or explicit no-era metadata");
  ok &= require(resolvePetEra({{"r",456},{"n",QStringLiteral("[灵初")}})==PetEra::Unknown &&
      resolvePetEra({{"r",456},{"n",QStringLiteral("名字里有灵初")}})==PetEra::Unknown &&
      resolvePetEra({{"r",456},{"_metaRaceId",999},{"_metaEra",QStringLiteral("灵初")}})==PetEra::Unknown,
      "malformed name prefixes, substrings or another race's cached era granted breakthrough");
  ok &= require(resolvePetEra({{"r",1}},{{"1",QJsonObject{{"sign",QStringLiteral("神职,超神,神属,传说")}}}})==PetEra::ChuanShuo &&
      resolvePetEra({{"r",1}},{{"1",QJsonObject{{"sign",QStringLiteral("神属")}}}})==PetEra::ShenShu &&
      resolvePetEra({{"r",1}},{{"1",QJsonObject{{"sign",QStringLiteral("神职,超神,神属")}}}})==PetEra::ShenZhi &&
      petEraDisplayName(PetEra::ChuanShuo)==QStringLiteral("传说"),
      "cumulative sign must take the latest chronological era, not string order");
  p=pet(); p.insert("ip","100#0#0#0#0#0#0#120#100#120#100#100"); p.insert("gps","2#1#1#1#1#1#1#3#2#3#2#2");
  p.insert("lv",120);
  auto gifted=p.value("czdlv").toObject(); gifted.insert("iv",972); p.insert("czdlv",gifted);
  auto giftedMeta=petPowerMetadataFromCatalog(p,8,m.stargods,m.astrolabe,
      {{"456",QJsonObject{{"jobs","22"},{"sign",QStringLiteral("神职")},{"maxLevel",120},{"stargodSlotMaxLevel",8}}}});
  v=calculatePetBattlePower(p,giftedMeta);
  {
    QStringList keys; bool ivChecked=false, plNamed=false, hasGsv=false, hasAsv=false;
    for (const auto& row : v.components) {
      keys.append(row.key);
      if (row.key==QStringLiteral("iv")) {
        ivChecked = row.explanation.contains(QStringLiteral("本地复算 972")) &&
            row.explanation.contains(QStringLiteral("与回包一致"));
      }
      if (row.key==QStringLiteral("pl")) plNamed = row.label==QStringLiteral("潜能");
      if (row.key==QStringLiteral("gsv")) hasGsv = true;
      if (row.key==QStringLiteral("asv")) hasAsv = true;
    }
    ok &= require(ivChecked && plNamed && !hasGsv && !hasAsv &&
            keys==QStringList({QStringLiteral("lv"),QStringLiteral("iv"),QStringLiteral("pl"),
                               QStringLiteral("sgv"),QStringLiteral("ep")}),
        "official gift formula must verify czdlv.iv and hide systems the era does not have");
  }
  p.insert("cps","12:6");
  auto plMeta=giftedMeta; plMeta.maxLevel=120;
  v=calculatePetBattlePower(p,plMeta);
  {
    bool plChecked=false;
    for (const auto& row : v.components) if (row.key==QStringLiteral("pl"))
      plChecked = row.explanation.contains(QStringLiteral("本地复算 240"));
    ok &= require(plChecked, "proficient formula trunc(level/maxLevel*40*pLevel) must reconstruct 240");
  }
  if (argc>1) {
    QFile f(QString::fromLocal8Bit(argv[1]));
    if (f.open(QIODevice::ReadOnly)) {
      const auto catalog=QJsonDocument::fromJson(f.readAll()).object();
      // Real catalog is optional for standalone runs, mandatory in the CTest target.
      auto md=petPowerMetadataFromCatalog({{"r",7529}},0,catalog.value("stargods").toObject(),
          catalog.value("astrolabe").toObject(),catalog.value("pets").toObject());
      ok &= require(!md.jobs.isEmpty() && md.slotMaxLevel>0,"latest pet metadata should be available without hardcoded IDs");
      auto real=pet();real.insert("r",7529);real.insert("astrolabe","");real.insert("sgsp",QJsonArray{});
      real.insert("sgs","66:8#67:8#68:8#70:8#72:8#73:8#74:8#80:8:80");
      auto a=real.value("czdlv").toObject(),b=real.value("mzdlv").toObject();a.insert("asv",0);b.insert("asv",0);
      real.insert("czdlv",a);real.insert("mzdlv",b);auto actual=calculatePetBattlePower(real,md);
      ok &= require(actual.hasHighest && actual.highest==6400 && actual.isHighest && actual.current==6400,
          "real complete catalog must ignore fragments/experience materials and yield a legal supreme target");
      if(argc>2) {
        QFile observed(QString::fromLocal8Bit(argv[2]));
        if(observed.open(QIODevice::ReadOnly)) {
          const auto value=QJsonDocument::fromJson(observed.readAll()).object().value("pet").toObject();
          const auto frozen=petPowerMetadataFromCatalog(value,0,catalog.value("stargods").toObject(),
              catalog.value("astrolabe").toObject(),catalog.value("pets").toObject());
          const auto calculated=calculatePetBattlePower(value,frozen);
          std::cout<<"local sample: observed="<<calculated.serverCurrent<<", local="<<calculated.current
              <<", official="<<calculated.extreme<<", supreme="<<calculated.highest
              <<", currentKnown="<<calculated.hasCurrent<<", supremeKnown="<<calculated.hasHighest<<'\n';
          for(const auto& why:calculated.unknownReasons) std::cout<<why.toUtf8().constData()<<'\n';
          ok &= require(calculated.currentLocallyCalculated && calculated.hasHighest,"complete persisted sample should have calculated power");
        } else ok &= require(false,"optional local sample could not be opened");
      }
    } else ok &= require(false,"catalog fixture could not be opened");
  }
  if(ok) std::cout<<"pet power composition checks passed\n";
  return ok?0:1;
}
