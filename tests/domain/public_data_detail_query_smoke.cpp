#include "domain/public_data_detail_query.h"
#include "fixtures/public_data_detail_fixture.h"
#include <QCoreApplication>
#include <cstdio>

int main(int argc,char** argv) {
  QCoreApplication app(argc,argv); bool ok=true;
  const auto require=[&](bool pass,const char* why) { if (!pass) { std::fprintf(stderr,"FAIL: %s\n",why); ok=false; } };
  const auto first=publicDetailFixture(); const PublicDataDetailContext context{first,{}};
  const auto get=[&](PublicDataViewKind view,const QString& table,const QString& id) { return PublicDataDetailQuery::resolve(context,{view,table,id}); };
  const auto skill=get(PublicDataViewKind::PetSkills,"skills","17");
  require(skill.title==QStringLiteral("first · H5技能") && skill.description.contains(QStringLiteral("官方配置")),"H5 skill did not resolve exact record");
  require(get(PublicDataViewKind::BattleReplay,"skills","17").title==QStringLiteral("Flash技能") &&
      get(PublicDataViewKind::PetSkills,"buffs","17").title==QStringLiteral("同号H5效果") &&
      get(PublicDataViewKind::BattleReplay,"buffs","17").title==QStringLiteral("同号Flash效果") &&
      get(PublicDataViewKind::PetDetail,"stargods","17").title==QStringLiteral("同号星神") &&
      get(PublicDataViewKind::BattleReplay,"stargods","17").title==QStringLiteral("Flash星神"),"same IDs crossed view/table namespace");
  require(get(PublicDataViewKind::PetSkills,"skills","017").state==PublicValueState::Missing,"text ID was numerically coerced");
  require(get(PublicDataViewKind::PetSkills,"entries",QStringLiteral("空说明")).state==PublicValueState::Empty,"empty official text lost its state");
  const auto term=get(PublicDataViewKind::PetSkills,"entries",QStringLiteral("提升·25"));
  require(term.state==PublicValueState::Present && term.description==QStringLiteral("提升25点") && term.raw.toObject().value("entryName")==QStringLiteral("提升"),"official parameter term rule/source was not retained");
  require(get(PublicDataViewKind::PetSkills,"entries",QStringLiteral("提升-I")).state==PublicValueState::Missing,"similar-name fallback invented an entry");
  const auto pet=get(PublicDataViewKind::PetSkills,"pets","2");
  require(pet.title==QStringLiteral("原始精灵") && pet.formName==QStringLiteral("外观名"),"pet title displayed skin in place of original");
  auto uncertain=std::make_shared<PublicDatasetSnapshot>(*first); auto uncertainMetadata=std::make_shared<PetDetailCatalogSnapshot>(*first->detail);
  auto uncertainPets=uncertainMetadata->root.value("pets").toObject(); uncertainPets.insert("2",QJsonObject{{"name",QStringLiteral("只有展示名字")}});
  uncertainMetadata->root.insert("pets",uncertainPets); uncertain->detail=uncertainMetadata;
  require(PublicDataDetailQuery::resolve({uncertain,{}},{PublicDataViewKind::PetDetail,"pets","2"}).title==QStringLiteral("原名待核实"),
      "missing original identity relationship was guessed from a displayed name");
  bool h5Normal=false,guessedBuff=false;
  for (const auto& link : pet.relations) h5Normal|=link.key.view==PublicDataViewKind::PetSkills && link.key.table=="skills" && link.key.id=="17";
  for (const auto& link : skill.relations) guessedBuff|=link.key.table=="buffs" && link.key.id=="17";
  require(h5Normal && !guessedBuff,"slot namespace was lost or same-number Buff relation invented");
  const auto embedded=get(PublicDataViewKind::PetSkills,"petStarGodSkills","2");
  require(embedded.title==QStringLiteral("精灵内嵌星神技") && embedded.description.contains(QStringLiteral("内嵌记录")) &&
      embedded.relations.size()==1 && embedded.relations.first().key.table=="pets","inline star-god skill was confused with a numeric skill table");
  bool verifiedBuff=false,unclassifiedBuff=false;
  for (const auto& link : get(PublicDataViewKind::BattleReplay,"skills","17").relations) {
    verifiedBuff|=link.key.table=="buffs" && link.key.id=="17"; unclassifiedBuff|=link.key.id=="999";
  }
  require(verifiedBuff && !unclassifiedBuff,"unclassified table relationship was presented as proved");
  const auto unresolved=get(PublicDataViewKind::BattleReplay,"officialEntryMentions","unresolved");
  require(unresolved.description.isEmpty() && !unresolved.raw.toObject().value("description").toString().isEmpty(),
      "unresolved word explanation was asserted or its original discarded");
  require(skill.raw.toObject().value("opaqueCode").toInt()==987654321,"opaque original field discarded");
  require(get(PublicDataViewKind::PetDetail,"astrolabe","0").state==PublicValueState::Present,"known astrolabe node zero lost");
  const PublicDataDetailContext replayOnly{publicDetailFixture("new"),first->replay};
  require(PublicDataDetailQuery::resolve(replayOnly,{PublicDataViewKind::PetSkills,"skills","17"}).state==PublicValueState::Missing,
      "replay-only query borrowed the current full dataset");
  const auto flash=PublicDataDetailQuery::resolve(replayOnly,{PublicDataViewKind::BattleReplay,"pets","2"});
  require(flash.title==QStringLiteral("Flash原始精灵") && flash.revision=="first-flash-sha" && flash.replayReference,
      "replay definition lost pinned version or was presented as occurrence");
  require(flash.relations.size()==1 && flash.relations.first().key.id=="17","unconfirmed candidate became a relation");
  require(flash.source.value("catalogProvenance").toObject().value("packages").toArray().first().toObject().value("url").toString()==
      QStringLiteral("https://example.invalid/official-fixture.swf"),"existing Flash provenance/source links were omitted");
  require(get(PublicDataViewKind::PetDetail,"badges","17").relations.isEmpty(),"cultivation identity joined a same-number combat Buff");
  const auto next=publicDetailFixture("next");
  require(PublicDataDetailQuery::resolve({next,{}},{PublicDataViewKind::PetSkills,"skills","17"}).title.startsWith("next") &&
      get(PublicDataViewKind::PetSkills,"skills","17").title.startsWith("first"),"new publication mutated retained context");
  const auto a=first->shop->allShops[0].goods.first(),b=first->shop->allShops[1].goods.first();
  const auto da=get(PublicDataViewKind::Shop,"goods",a.stableKey()),db=get(PublicDataViewKind::Shop,"goods",b.stableKey());
  require(a.stableKey()!=b.stableKey() && da.title!=db.title && da.title.contains("module-a") && db.title.contains("module-b"),"activity-local identical item IDs crossed shop namespace");
  bool applicable=false;
  for (const auto& link : da.relations) applicable|=link.key.table=="pets" && link.key.id=="1";
  require(!da.raw.isUndefined() && applicable,"shop original or explicit applicable pets lost");
  auto unknownMaterial=std::make_shared<PublicDatasetSnapshot>(*first);
  auto unknownCatalog=std::make_shared<ShopCatalogSnapshot>(*first->shop); unknownCatalog->allShops[0].goods[0].cost="4:987654321:5"; unknownMaterial->shop=unknownCatalog;
  const auto missingMaterial=PublicDataDetailQuery::resolve({unknownMaterial,{}},{PublicDataViewKind::Shop,"goods",a.stableKey()});
  bool priceWithoutId=false;
  for (const auto& field : missingMaterial.fields) if (field.label==QStringLiteral("所需材料")) priceWithoutId=field.text.contains(QStringLiteral("材料名称未收录")) && !field.text.contains("987654321");
  require(priceWithoutId,"unknown material exposed identifier in ordinary presentation");
  auto conflicting=std::make_shared<PublicDatasetSnapshot>(*first); auto duplicate=std::make_shared<ShopCatalogSnapshot>(*first->shop);
  duplicate->allShops.append(duplicate->allShops.first()); conflicting->shop=duplicate;
  require(PublicDataDetailQuery::resolve({conflicting,{}},{PublicDataViewKind::Shop,"goods",a.stableKey()}).state==PublicValueState::InvalidContainer,
      "ambiguous shop identity silently selected first record");
  return ok ? 0 : 1;
}
