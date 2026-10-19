// Offline audit: the same pure rule and fact pipeline used by the workbench.
// Input snapshots remain local; output contains public offers and aggregate counts.
#include "domain/shop_pet_eligibility.h"
#include "domain/pet_metadata_view.h"
#include "domain/pet_analysis_facts.h"
#include "domain/pet_identity.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSaveFile>
#include <cstdio>

QJsonDocument read(const QString& path) {
  QFile file(path); if (!file.open(QIODevice::ReadOnly)) qFatal("Cannot read audit input");
  QJsonParseError error; const auto result = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError) qFatal("Invalid audit JSON");
  return result;
}
int main(int argc, char** argv) {
  QCoreApplication app(argc,argv);
  if (argc != 5) { std::fprintf(stderr,"cases.json metadata.json local-pets.json output.json\n"); return 2; }
  const auto cases = read(QString::fromLocal8Bit(argv[1])).array();
  const auto root = read(QString::fromLocal8Bit(argv[2])).object();
  const auto input = read(QString::fromLocal8Bit(argv[3])).object();
  auto snapshot = std::make_shared<PetDetailCatalogSnapshot>(); snapshot->root = root;
  PetMetadataView::indexPets(snapshot.get()); const PetMetadataView view(snapshot);
  const ShopPetMetadataSnapshot metadata{view.stargodDefinitions(),view.astrolabeDefinitions(),view.petDefinitions(),
      view.sacredStarPlans(),view.sacredStagePlans(),view.badgeDefinitions(),view.itemDefinitions(),view.sacredEquipmentDefinitions()};
  QList<ShopPetDerived> pets; QJsonObject regression;
  const QStringList oldCodes{"11","31","32","33","33$1","33$5","34","39","39$1","39$3","41","42","43","44","62","84","85","86","89","89$1","91","92","94","95"};
  QList<QByteArray> oldResults; oldResults.resize(oldCodes.size()); int incomplete = 0, factRoundTrips = 0, factFailures = 0;
  for (const auto& value : input.value("pets").toArray()) {
    const auto entry = value.toObject(); const auto pet = view.enrichMetadata(entry.value("pet").toObject());
    const bool complete = entry.value("complete").toBool(); incomplete += !complete;
    pets.append(deriveShopPet(pet,complete,metadata));
    for (int i=0;i<oldCodes.size();++i) {
      const auto state=evaluateShopPetRule(compileShopPetRule(oldCodes[i]),pets.back());
      oldResults[i] += QByteArray::number(int(state.state)) + ':' + QByteArray::number(state.usefulCount) + ':' + QByteArray::number(state.redStarUseful) + ';';
    }
    PetAssetRecord seed; seed.instanceId=petInstanceId(pet); seed.raceId=petRaceId(pet); seed.pet=pet; seed.detailAvailable=complete;
    const auto facts=derivePetAnalysisFacts(seed,metadata);
    if (facts) {
      const auto restored=petAnalysisFactsFromJson(petAnalysisFactsToJson(*facts));
      if (restored) ++factRoundTrips; else ++factFailures;
    } else ++factFailures;
  }
  for (int i=0;i<oldCodes.size();++i) regression.insert(oldCodes[i],QString::fromLatin1(QCryptographicHash::hash(oldResults[i],QCryptographicHash::Sha256).toHex()));
  QJsonArray rows; int boundaryPassed=0,boundaryFailed=0;
  for (const auto& value : cases) {
    const auto entry=value.toObject(), good=entry.value("good").toObject(); auto row=entry.value("notice").toObject();
    QSet<int> races; for (const auto& race : good.value("raceIds").toArray()) races.insert(race.toInt());
    const auto rule=compileShopPetRule(good.value("enhanceType").toString());
    int usable=0, full=0, unknown=0; QJsonObject reasons; QJsonArray missingRules;
    for (const auto& component : rule.components) if(component.componentIndex < 0) missingRules.append(component.code);
    for (const auto& pet : pets) {
      if (!races.contains(pet.raceId) && !races.contains(pet.metadataRaceId)) continue;
      const auto result=describeShopPetRule(rule,pet);
      if(result.state==ShopPetEligibilityState::Usable) ++usable;
      else if(result.state==ShopPetEligibilityState::NotUsable) ++full;
      else ++unknown;
      reasons.insert(result.reason,reasons.value(result.reason).toInt()+1);
    }
    row.insert("enhanceType",good.value("enhanceType")); row.insert("targetSpecies",good.value("raceIds"));
    row.insert("manualSelectionRequired",good.value("manualSelectionRequired")); row.insert("missingRules",missingRules);
    row.insert("localMatches",usable+full+unknown); row.insert("usable",usable); row.insert("notNeeded",full); row.insert("unknown",unknown);
    row.insert("reasons",reasons); rows.append(row);
    if (usable+full+unknown==0) {
      // No local instance: explicit synthetic packet states, never represented
      // as an observation from the user's account or a completed exchange.
      auto fixtureMetadata=metadata;
      const auto node=[](int layer,bool exclusive) { return QJsonObject{{"locatedType",layer},{"exclusive",exclusive},{"isTBD",false},{"battlePower",100}}; };
      fixtureMetadata.astrolabe.insert("200000001",node(1,false));
      fixtureMetadata.astrolabe.insert("200000002",node(2,false));
      fixtureMetadata.astrolabe.insert("200000003",node(2,true));
      QJsonObject fixture{{"id",1},{"r",good.value("raceIds").toArray().first().toInt()}};
      const QString effect=good.value("enhanceType").toString();
      const QString missing=effect=="81" ? "200000001:0:0#200000002:1:0|200000003:1:0" :
          effect=="82" ? "200000001:1:0#200000002:0:0|200000003:1:0" : "200000001:1:0#200000002:1:0|200000003:0:0";
      QJsonObject boundary; bool passed=true;
      for(int state=0;state<3;++state) {
        if(state==0) fixture.insert("astrolabe",missing);
        else if(state==1) fixture.insert("astrolabe","200000001:1:0#200000002:1:0|200000003:1:0");
        else fixture.remove("astrolabe");
        const auto actual=evaluateShopPetRule(rule,deriveShopPet(fixture,true,fixtureMetadata)).state;
        const auto expected=state==0 ? ShopPetEligibilityState::Usable : state==1 ? ShopPetEligibilityState::NotUsable : ShopPetEligibilityState::Unknown;
        const bool ok=actual==expected; passed &= ok; boundary.insert(state==0 ? "missing" : state==1 ? "completed" : "missingDetail",ok);
        if(ok) ++boundaryPassed; else ++boundaryFailed;
      }
      row.insert("syntheticBoundaryChecks",boundary); row.insert("verification",passed ? "synthetic-boundaries-passed" : "synthetic-boundaries-failed");
    } else row.insert("verification","local-detail-replay");
    rows.replace(rows.size()-1,row);
  }
  const QJsonObject output{{"cases",rows},{"petCount",pets.size()},{"incomplete",incomplete},
      {"snapshotSha256",input.value("snapshotSha256")},{"factRoundTrips",factRoundTrips},{"factFailures",factFailures},{"existingRuleFingerprints",regression},
      {"boundaryPassed",boundaryPassed},{"boundaryFailed",boundaryFailed}};
  QSaveFile file(QString::fromLocal8Bit(argv[4])); if(!file.open(QIODevice::WriteOnly)) return 3;
  file.write(QJsonDocument(output).toJson(QJsonDocument::Indented)); if(!file.commit()) return 3;
  std::printf("audited=%lld pets=%lld fact_round_trips=%d fact_failures=%d\n",rows.size(),pets.size(),factRoundTrips,factFailures);
  return boundaryFailed ? 4 : 0;
}
