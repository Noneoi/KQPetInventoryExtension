#include "support/preview_inventory_support.h"
#include "application/catalog/pet_detail_catalog.h"
#include "application/catalog/public_dataset_store.h"
#include "domain/pet_relation_rules.h"
#include "ui/pet/pet_window.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QJsonDocument>
#include <QPainter>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <QTimer>
#include <cmath>
#include <stdexcept>

namespace {
void require(bool valid, const QString& message) { if (!valid) throw std::runtime_error(message.toUtf8().constData()); }
QString sha(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()); }
QByteArray read(const QString& path) { QFile f(path); require(f.open(QIODevice::ReadOnly),QStringLiteral("Cannot read %1").arg(path)); return f.readAll(); }
QJsonObject object(const QByteArray& bytes) {
  QJsonParseError error; const auto doc=QJsonDocument::fromJson(bytes,&error);
  require(error.error==QJsonParseError::NoError && doc.isObject(),QStringLiteral("Expected a JSON object")); return doc.object();
}
void write(const QString& path,const QByteArray& bytes) {
  require(QDir().mkpath(QFileInfo(path).absolutePath()),QStringLiteral("Cannot create QA directory"));
  QFile f(path); require(f.open(QIODevice::WriteOnly) && f.write(bytes)==bytes.size(),QStringLiteral("Cannot write %1").arg(path));
}
QString arg(const QStringList& args,const QString& name,const QString& fallback={}) {
  const int i=args.indexOf(name); return i>=0 && i+1<args.size() ? args[i+1] : fallback;
}
const DetailPage* page(const PreparedPetDetailHandle& detail,DetailSection section) {
  if(detail) for(const auto& p:detail->pages) if(p.section==section)return &p; return nullptr;
}
QVector<DetailEntry> candidates(const DetailPage* p) {
  QVector<DetailEntry> out;
  if(p) for(const auto& e:p->entries)
    if(e.group==DetailRelationGroup::carryCandidate() || e.group==DetailRelationGroup::summonList())out.append(e);
  return out;
}
bool warehouse(DetailOwnership value) {
  return value==DetailOwnership::WarehouseNormal || value==DetailOwnership::WarehouseElite ||
      value==DetailOwnership::WarehouseGoodbye || value==DetailOwnership::WarehouseUnknown;
}
bool has(const DetailPage* p,qint64 id,DetailOwnership where) {
  for(const auto& e:candidates(p))if(e.relatedInstanceId==id && e.ownership==where)return true; return false;
}
bool missing(const DetailPage* p,int race,const QString& name) {
  for(const auto& e:candidates(p))if(e.definitionId==race && e.ownership==DetailOwnership::Missing && e.originalName==name)return true;
  return false;
}
void dataset(PreviewInventory::Fixture& f,PublicDatasetSnapshotHandle value) {
  f.controller.setPublicDataset(value);f.publisher.setPublicDataset(std::move(value));
}
PreparedPetDetailHandle visible(PreviewInventory::Fixture& f,QTextBrowser* browser,qint64 id,
                               DetailSection section,const std::function<bool(const DetailPage*)>& predicate) {
  const bool ready=PreviewInventory::until([&] {
    const auto value=f.inventory.preparedDetail(0,id); const auto p=page(value,section);
    if(!value || !value->detailKnown || value->version.facts.record!=f.inventory.recordVersion(id).key || !p || !predicate(p))return false;
    return !browser->toPlainText().contains(QStringLiteral("正在准备本地详情")) &&
        browser->toPlainText().contains(value->identity.originalName);
  },30000);
  require(ready,QStringLiteral("Relation UI did not become ready: %1").arg(f.diagnostics()));
  return f.inventory.preparedDetail(0,id);
}
void openSection(QTextBrowser* browser,DetailSection section) {
  const QUrl url(QStringLiteral("kqdetail://page/%1/0").arg(int(section)));
  require(browser->toHtml().contains(url.toString()),QStringLiteral("Relation navigation was not offered by PetWindow"));
  require(QMetaObject::invokeMethod(browser,"anchorClicked",Qt::DirectConnection,Q_ARG(QUrl,url)),QStringLiteral("Cannot follow the real detail link"));
}
QString relationHtml(QTextBrowser* browser,DetailSection section) {
  const QStringList names=section==DetailSection::SummonRelations ? QStringList{QStringLiteral("召唤物关系")} :
      QStringList{QStringLiteral("携带关系"),QStringLiteral("吞噬关系"),QStringLiteral("携同关系"),QStringLiteral("天觉携带关系")};
  for(auto block=browser->document()->begin();block.isValid();block=block.next()) {
    if(block.blockFormat().headingLevel()!=3 || !names.contains(block.text().trimmed()))continue;
    QTextCursor cursor(browser->document());cursor.setPosition(block.position());cursor.movePosition(QTextCursor::End,QTextCursor::KeepAnchor);
    return QTextDocumentFragment(cursor).toHtml();
  }
  throw std::runtime_error("Real PetWindow document contains no relation section heading");
}
QJsonObject capture(QTextBrowser* browser,const PreparedPetDetailHandle& detail,DetailSection section,const QString& output,const QString& stem) {
  const auto p=page(detail,section);require(p,QStringLiteral("Prepared relation page missing"));
  const auto html=relationHtml(browser,section);
  QTextDocument document;document.setDefaultFont(browser->font());document.setHtml(html);document.setTextWidth(1060);
  const int height=int(std::ceil(document.size().height()))+24;
  require(height>0 && height<=16384,QStringLiteral("Relation capture exceeds explicit image budget"));
  QImage pixels(1084,height,QImage::Format_ARGB32_Premultiplied);pixels.fill(Qt::white);
  {QPainter painter(&pixels);painter.translate(12,12);document.drawContents(&painter);}
  write(QDir(output).filePath(stem+QStringLiteral(".html")),html.toUtf8());
  require(pixels.save(QDir(output).filePath(stem+QStringLiteral(".png"))),QStringLiteral("Cannot save relation PNG"));
  int backpack=0,stored=0,absent=0,unknown=0;QJsonArray rows,current;
  for(const auto& e:p->entries) {
    const bool candidate=e.group==DetailRelationGroup::carryCandidate() || e.group==DetailRelationGroup::summonList();
    QJsonObject row{{"instanceId",QString::number(e.relatedInstanceId)},{"originalRaceId",e.definitionId},{"originalName",e.originalName},
        {"name",e.name},{"ownership",int(e.ownership)},{"selected",e.selected},{"group",e.group}};
    if(!candidate){current.append(row);continue;} rows.append(row);
    if(e.ownership==DetailOwnership::Backpack)++backpack;else if(warehouse(e.ownership))++stored;
    else if(e.ownership==DetailOwnership::Missing)++absent;else ++unknown;
    require(!e.originalName.isEmpty() && document.toPlainText().contains(e.originalName),QStringLiteral("A candidate original name is missing from rendered UI"));
  }
  return {{"instanceId",QString::number(detail->identity.instanceId)},{"raceId",detail->identity.raceId},{"section",int(section)},
      {"publicRevision",detail->version.publicDatasetRevision},{"rosterRevision",qint64(detail->version.relationRosterRevision)},
      {"candidatesKnown",section==DetailSection::SummonRelations?p->summonCandidatesKnown:p->carryCandidatesKnown},
      {"candidateEntries",rows.size()},{"backpackInstances",backpack},{"warehouseInstances",stored},{"missingFamilies",absent},{"unknownLocationEntries",unknown},
      {"candidates",rows},{"currentRelations",current},{"relationHtml",QDir(output).filePath(stem+QStringLiteral(".html"))},
      {"relationPng",QDir(output).filePath(stem+QStringLiteral(".png"))},{"renderedText",document.toPlainText()}};
}
void countRequests(PetWindow& window,int& requests) {
  QObject::connect(&window,&PetWindow::detailRequested,&window,[&](qint64){++requests;});
  QObject::connect(&window,&PetWindow::listRefreshRequested,&window,[&]{++requests;});
  QObject::connect(&window,&PetWindow::warehouseDetailRefreshRequested,&window,[&](const QList<qint64>&){++requests;});
  QObject::connect(&window,&PetWindow::moveToWarehouseRequested,&window,[&](qint64){++requests;});
  QObject::connect(&window,&PetWindow::moveToBackpackRequested,&window,[&](qint64){++requests;});
}
PublicDatasetSnapshotHandle syntheticDataset() {
  QJsonObject definitions,replayPets,mechanics;
  const QMap<int,QString> names{{100,QStringLiteral("测试主控原名")},{200,QStringLiteral("测试原名甲")},{201,QStringLiteral("测试皮肤甲")},
      {300,QStringLiteral("测试原名乙")},{400,QStringLiteral("测试未持有原名丙")},{500,QStringLiteral("测试新增原名丁")},{999,QStringLiteral("测试不同种族")}};
  for(auto it=names.cbegin();it!=names.cend();++it) {
    const int original=it.key()==201?200:it.key();const auto originalName=names.value(original);
    auto row=QJsonObject{{"name",it.value()},{"sign",it.key()==100?QStringLiteral("吞噬技"):QStringLiteral("神职")}};
    if(it.key()==201)row.insert("groupRaceId",200);definitions.insert(QString::number(it.key()),row);
    replayPets.insert(QString::number(it.key()),QJsonObject{{"identity",QJsonObject{{"status",it.key()==201?"skin":"native"},
        {"baseRaceId",QString::number(original)},{"canonicalName",originalName}}}});
    mechanics.insert(QString::number(it.key()),QJsonObject{{"sign",it.key()==201?QStringLiteral("神职,皮肤"):QStringLiteral("神职")}});
  }
  auto empty=std::make_shared<PetDetailCatalogSnapshot>();empty->loaded=true;QString error;
  auto value=std::make_shared<PublicDatasetSnapshot>();value->revision=QString(64,QLatin1Char('b'));
  value->detail=PetDetailCatalog::prepareOverlay(empty,{{"schema",1},{"pets",definitions}},QStringLiteral("TEST ONLY relation fixture"),{},&error);
  require(bool(value->detail),error);value->skill=std::make_shared<PetSkillCatalogSnapshot>();
  value->shop=std::make_shared<ShopCatalogSnapshot>();value->routine=std::make_shared<RoutineCatalogSnapshot>();
  QJsonArray targets{"200","201","300","400","500"};QJsonObject pairs;
  for(const auto& target:targets)pairs.insert(QStringLiteral("100_")+target.toString(),0);
  const QJsonObject group{{"rows",QJsonArray{QJsonObject{{"rowId",0},{"sourceRaceIds",QJsonArray{"100"}},
      {"targetRaceIds",targets},{"type",16},{"expansionStatus","resolved"},{"isShow",true},{"table","TEST ONLY"}}}},
      {"byPair",pairs},{"bySourceRaceId",QJsonObject{{"100",QJsonArray{0}}}}};
  auto replay=std::make_shared<BattleReplayCatalogSnapshot>();replay->root={{"pets",replayPets},{"petMechanics",mechanics},
      {"replaySupplements",QJsonObject{{"summon",group},{"carry",group}}}};
  replay->revision=1;replay->sha256=sha(QJsonDocument(replay->root).toJson(QJsonDocument::Compact));value->replay=replay;
  return value;
}
QJsonObject synthetic(const QString& output) {
  QTemporaryDir dir;require(dir.isValid(),QStringLiteral("No temporary fixture directory"));qputenv("KQPET_DATA_ROOT",dir.path().toUtf8());
  PreviewInventory::Fixture f(dir.path(),QStringLiteral("synthetic-relation-integration"));require(f.initialized,QStringLiteral("Fixture initialization failed"));
  const auto full=syntheticDataset();auto deferred=std::make_shared<PublicDatasetSnapshot>(*full);deferred->replay.reset();dataset(f,deferred);
  const auto pet=[](int id,int race,const QString& name){return QJsonObject{{"id",id},{"r",race},{"fr",race},{"n",name},{"lv",120},{"zdl",20000},{"xzdl",22000},
      {"czdlv",QJsonObject{{"lv",20000}}},{"mzdlv",QJsonObject{{"lv",22000}}}};};
  auto a=pet(21,201,QStringLiteral("不能当原名的甲皮肤"));auto b=pet(31,300,QStringLiteral("不能当原名的乙昵称"));
  auto wrong=pet(41,999,QStringLiteral("错误外观匹配"));wrong.insert("fr",400);
  auto added=pet(51,500,QStringLiteral("新入库昵称"));auto owner=pet(10,100,QStringLiteral("主控协议名称"));owner.insert("cppl",a);owner.insert("sppl",b);
  require(!owner.contains("asps") && !owner.contains("acps"),QStringLiteral("Fixture accidentally includes candidate arrays"));
  require(f.publishLists({owner,a},{PreviewInventory::brief(b),PreviewInventory::brief(wrong)},{},{b,wrong}) && f.factsReady({10}),QStringLiteral("Initial inventory publication failed"));
  int requests=0,loads=0;bool loadingSeen=false;
  QObject::connect(&f.details,&PetDetailPreparationService::ready,&f.publisher,[&](int,quint64,const PreparedPetDetailHandle& detail){
    for(const auto& p:detail->pages)loadingSeen|=p.relationCatalogLoading;
  });
  QObject::connect(&f.publisher,&InventoryPublisher::relationCatalogNeeded,&f.publisher,[&]{
    ++loads;QTimer::singleShot(200,&f.publisher,[&]{dataset(f,full);f.publisher.relationCatalogLoadFinished(true);});
  });
  PetWindow window(&f.inventory);countRequests(window,requests);window.resize(1450,1000);window.show();window.focusPet(10);
  auto* browser=window.findChild<QTextBrowser*>(QStringLiteral("KQPetPreparedDetail"));require(browser,QStringLiteral("Real detail widget missing"));
  auto prepared=visible(f,browser,10,DetailSection::CarryRelations,[](const DetailPage* p){return p->carryCandidatesKnown && candidates(p).size()==4;});
  require(loads==1 && loadingSeen,QStringLiteral("Deferred same-version catalog did not load through the publisher signal and refresh"));
  const auto verify=[&](const DetailPage* p){
    require(has(p,21,DetailOwnership::Backpack) && has(p,31,DetailOwnership::WarehouseNormal),QStringLiteral("Official candidates did not match the full owned roster"));
    require(missing(p,400,QStringLiteral("测试未持有原名丙")) && missing(p,500,QStringLiteral("测试新增原名丁")),QStringLiteral("Unowned official originals were not shown"));
    for(const auto& e:candidates(p))require(e.relatedInstanceId!=41,QStringLiteral("Face ID was used as the race for eligibility"));
  };
  verify(page(prepared,DetailSection::CarryRelations));verify(page(prepared,DetailSection::SummonRelations));
  openSection(browser,DetailSection::CarryRelations);
  prepared=visible(f,browser,10,DetailSection::CarryRelations,[](const DetailPage* p){return p->carryCandidatesKnown && candidates(p).size()==4;});
  QJsonArray captures;captures.append(capture(browser,prepared,DetailSection::CarryRelations,output,QStringLiteral("synthetic-carry-before")));
  const auto old=prepared;const auto roster=prepared->version.relationRosterRevision;
  bool currentOriginal=false;for(const auto& e:page(prepared,DetailSection::CarryRelations)->entries)
    if(e.group==DetailRelationGroup::carriedEnvoy())currentOriginal|=e.originalName==QStringLiteral("测试原名甲");
  require(currentOriginal,QStringLiteral("Current related object's original name was not resolved"));
  require(f.publishLists({owner,a},{PreviewInventory::brief(b),PreviewInventory::brief(wrong),PreviewInventory::brief(added)},{},{b,wrong,added}),QStringLiteral("New warehouse member failed to publish"));
  prepared=visible(f,browser,10,DetailSection::CarryRelations,[](const DetailPage* p){return has(p,51,DetailOwnership::WarehouseNormal);});
  require(prepared->version.relationRosterRevision!=roster,QStringLiteral("New matching member did not invalidate the frozen roster"));
  require(f.publishLists({owner,a,b},{PreviewInventory::brief(wrong),PreviewInventory::brief(added)},{},{wrong,added}),QStringLiteral("Backpack movement failed to publish"));
  prepared=visible(f,browser,10,DetailSection::CarryRelations,[](const DetailPage* p){return has(p,31,DetailOwnership::Backpack) && has(p,51,DetailOwnership::WarehouseNormal);});
  require(has(page(old,DetailSection::CarryRelations),31,DetailOwnership::WarehouseNormal),QStringLiteral("An inventory change mutated an old prepared snapshot"));
  captures.append(capture(browser,prepared,DetailSection::CarryRelations,output,QStringLiteral("synthetic-carry-after-move")));
  openSection(browser,DetailSection::Overview);
  visible(f,browser,10,DetailSection::SummonRelations,[](const DetailPage* p){return p->summonCandidatesKnown && has(p,31,DetailOwnership::Backpack);});
  openSection(browser,DetailSection::SummonRelations);
  prepared=visible(f,browser,10,DetailSection::SummonRelations,[](const DetailPage* p){return p->summonCandidatesKnown && candidates(p).size()==4;});
  captures.append(capture(browser,prepared,DetailSection::SummonRelations,output,QStringLiteral("synthetic-summon")));
  require(requests==0,QStringLiteral("A relation lookup attempted a game request"));window.hide();require(f.close(),QStringLiteral("Fixture shutdown failed"));
  return {{"passed",true},{"kind","synthetic-end-to-end"},{"deferredCatalogRequests",loads},{"loadingStateObserved",loadingSeen},{"gameRequests",requests},{"captures",captures}};
}

PublicDatasetSnapshotHandle mirrorDataset(const QString& source,const QString& mirror,QJsonArray& inputs) {
  const auto copy=[&](const QString& relative,const QString& expected={},qint64 length=-1){
    const auto path=QDir(source).filePath(relative);const auto bytes=read(path);
    require(expected.isEmpty() || sha(bytes)==expected,QStringLiteral("Source object digest differs"));
    require(length<0 || bytes.size()==length,QStringLiteral("Source object length differs"));
    write(QDir(mirror).filePath(relative),bytes);inputs.append(QJsonObject{{"path",path},{"sha256",sha(bytes)},{"bytes",bytes.size()}});return bytes;
  };
  const auto pointer=object(copy(QStringLiteral("public/current.json")));const auto revision=pointer.value("revision").toString();
  const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));require(digest.match(revision).hasMatch(),QStringLiteral("Invalid source revision"));
  const auto manifest=object(copy(QStringLiteral("public/releases/%1/manifest.json").arg(revision),revision));
  QSet<QString> copied;
  std::function<void(const QJsonObject&,int)> ref=[&](const QJsonObject& value,int depth){
    const auto id=value.value("sha256").toString();require(depth<=16 && digest.match(id).hasMatch(),QStringLiteral("Invalid retained source reference"));
    if(!copied.contains(id)){copy(QStringLiteral("public/objects/%1/data").arg(id),id,qint64(value.value("bytes").toDouble()));copied.insert(id);}
    const auto origin=value.value("origin").toObject();for(const auto* key:{"rawObject","embeddedObject"})if(origin.value(QLatin1String(key)).isObject())ref(origin.value(QLatin1String(key)).toObject(),depth+1);
  };
  for(const auto* key:{"components","attachments"}){const auto values=manifest.value(QLatin1String(key)).toObject();for(const auto& value:values)ref(value.toObject(),0);}
  const auto attempt=pointer.value("attempt").toString();
  if(QRegularExpression(QStringLiteral("^[0-9a-f]{32}$")).match(attempt).hasMatch()){
    const auto receipt=QStringLiteral("public/transactions/%1/receipt.json").arg(attempt);
    if(QFileInfo::exists(QDir(source).filePath(receipt)))copy(receipt);
  }
  // prepareCurrent can repair its receipt. That side effect is confined to
  // this exact current-version mirror, never the original user's cache.
  const auto loaded=PublicDatasetStore(mirror).prepareCurrent(true);
  require(loaded.ok() && loaded.snapshot && loaded.snapshot->replay,loaded.error);return loaded.snapshot;
}
QJsonObject actual(const QString& source,const QString& output,const QString& requestedAccount,bool expectedCounts) {
  QTemporaryDir temporary;require(temporary.isValid(),QStringLiteral("No temporary mirror directory"));QJsonArray inputs;
  const auto shared=mirrorDataset(source,QDir(temporary.path()).filePath(QStringLiteral("catalog-mirror")),inputs);
  const auto inputObject=[&](const QString& path){const auto bytes=read(path);inputs.append(QJsonObject{{"path",path},{"sha256",sha(bytes)},{"bytes",bytes.size()}});return object(bytes);};
  const auto accountRoot=QDir(source).filePath(QStringLiteral("accounts"));
  auto accounts=requestedAccount.isEmpty()?QDir(accountRoot).entryList(QDir::Dirs|QDir::NoDotAndDotDot):QStringList{requestedAccount};
  QString account;QJsonObject inventory;QMap<int,qint64> chosen;QMap<qint64,QJsonObject> details;
  for(const auto& name:accounts){
    const auto file=QDir(accountRoot).filePath(name+QStringLiteral("/inventory.json"));if(!QFileInfo::exists(file))continue;
    const auto candidate=inputObject(file);QMap<int,qint64> found;QMap<qint64,QJsonObject> full;
    QJsonArray all=candidate.value("backpack").toArray();for(const auto& p:candidate.value("warehouse").toArray())all.append(p);
    for(int race:{6769,7532}){
      QList<qint64> ids;
      for(const auto& value:all){const auto pet=value.toObject();if(petRaceId(pet)==race)ids.append(petInstanceId(pet));}
      for(auto id:ids){const auto path=QDir(accountRoot).filePath(name+QStringLiteral("/details/%1.json").arg(id));if(!QFileInfo::exists(path))continue;
        auto pet=inputObject(path).value("pet").toObject();if(petRaceId(pet)!=race || petInstanceId(pet)!=id)continue;
        pet.remove("asps");pet.remove("acps");full.insert(id,pet);found.insert(race,id);break;}
    }
    if(found.size()==2){account=name;inventory=candidate;chosen=found;details=full;break;}
  }
  require(chosen.size()==2,QStringLiteral("Both cached relation samples (r6769/r7532) were not found"));
  const auto fixtureRoot=QDir(temporary.path()).filePath(QStringLiteral("inventory-fixture"));qputenv("KQPET_DATA_ROOT",fixtureRoot.toUtf8());
  PreviewInventory::Fixture f(fixtureRoot,QStringLiteral("local-relation-reproduction"));require(f.initialized,QStringLiteral("Actual fixture initialization failed"));dataset(f,shared);
  QJsonArray backpack,normal,elite;QList<QJsonObject> warehouseDetails;
  for(const auto& value:inventory.value("backpack").toArray()){auto pet=value.toObject();if(details.contains(petInstanceId(pet)))pet=details.value(petInstanceId(pet));backpack.append(pet);}
  for(const auto& value:inventory.value("warehouse").toArray()){const auto pet=value.toObject();
    (pet.value("_warehouseGroup")==QStringLiteral("elite")?elite:normal).append(pet);
    if(details.contains(petInstanceId(pet)))warehouseDetails.append(details.value(petInstanceId(pet)));
  }
  require(f.publishLists(backpack,normal,elite,warehouseDetails) && f.factsReady(details.keys()),QStringLiteral("Actual complete inventory did not publish"));
  PetWindow window(&f.inventory);int requests=0;countRequests(window,requests);window.resize(1450,1000);window.show();
  auto* browser=window.findChild<QTextBrowser*>(QStringLiteral("KQPetPreparedDetail"));require(browser,QStringLiteral("Actual detail browser missing"));QJsonArray captures;
  for(int race:{6769,7532}){
    const auto id=chosen.value(race);const auto section=race==6769?DetailSection::CarryRelations:DetailSection::SummonRelations;window.focusPet(id);
    visible(f,browser,id,section,[&](const DetailPage* p){return section==DetailSection::CarryRelations?p->carryCandidatesKnown:p->summonCandidatesKnown;});
    openSection(browser,section);
    const auto prepared=visible(f,browser,id,section,[&](const DetailPage* p){return section==DetailSection::CarryRelations?p->carryCandidatesKnown:p->summonCandidatesKnown;});
    auto record=capture(browser,prepared,section,output,QStringLiteral("actual-%1-r%2-relations").arg(id).arg(race));
    const auto options=petRelationOptions(shared->replay->root,race,section==DetailSection::CarryRelations?PetRelationKind::Carry:PetRelationKind::Summon);
    record.insert("officialFamilyCount",options.families.size());record.insert("candidateFieldsRemoved",QJsonArray{"asps","acps"});
    if(expectedCounts){
      const int total=race==6769?3:13,bag=race==6769?0:4,stored=race==6769?3:9;
      require(record.value("candidateEntries").toInt()==total && record.value("backpackInstances").toInt()==bag && record.value("warehouseInstances").toInt()==stored,
          QStringLiteral("Actual r%1 candidate/location anchor differs: %2").arg(race).arg(QString::fromUtf8(QJsonDocument(record).toJson(QJsonDocument::Compact))));
    }
    require(record.value("candidateEntries").toInt()>1 && record.value("unknownLocationEntries")==QJsonValue(0),QStringLiteral("Actual UI still only shows current/unknown relations"));captures.append(record);
  }
  require(requests==0,QStringLiteral("Actual reproduction attempted a game request"));window.hide();require(f.close(),QStringLiteral("Actual fixture shutdown failed"));
  for(const auto& input:inputs){const auto row=input.toObject();require(sha(read(row.value("path").toString()))==row.value("sha256").toString(),QStringLiteral("An original input changed during reproduction"));}
  return {{"passed",true},{"kind","actual-data-offscreen-end-to-end"},{"sourceDataRoot",source},{"sourceAccount",account},
      {"publicRevision",shared->revision},{"backpackPublished",backpack.size()},{"warehousePublished",normal.size()+elite.size()},
      {"expectedCountsChecked",expectedCounts},{"gameRequests",requests},{"sourceFilesUnchanged",true},{"sourceInputs",inputs},{"captures",captures}};
}
}

int main(int argc,char** argv) {
  if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))qputenv("QT_QPA_PLATFORM","offscreen");
  QApplication app(argc,argv);app.setApplicationName(QStringLiteral("KQPetRelationIntegration"));
  QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
  QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
  app.setFont(QFont(QStringLiteral("Microsoft YaHei"),10));
  QTemporaryDir defaultOutput;const auto args=app.arguments();const auto output=QDir::cleanPath(arg(args,QStringLiteral("--output-dir"),defaultOutput.path()));
  const auto source=arg(args,QStringLiteral("--data-root"));QJsonObject result;int code=0;
  try {
    require(!output.isEmpty(),QStringLiteral("Output directory missing"));
    if(!source.isEmpty())require(!QFileInfo(output).absoluteFilePath().startsWith(QDir(source).absolutePath()+QLatin1Char('/'),Qt::CaseInsensitive),QStringLiteral("QA output must be outside user data"));
    require(QDir().mkpath(output),QStringLiteral("Cannot create output directory"));
    result=source.isEmpty()?synthetic(output):actual(QDir(source).absolutePath(),output,arg(args,QStringLiteral("--account")),args.contains(QStringLiteral("--expected-counts")));
  }catch(const std::exception& e){result={{"passed",false},{"error",QString::fromUtf8(e.what())}};code=1;}
  result.insert("offscreen",QGuiApplication::platformName()==QStringLiteral("offscreen"));result.insert("gameTransportConnected",false);
  try{write(QDir(output).filePath(QStringLiteral("pet-relation-integration.json")),QJsonDocument(result).toJson(QJsonDocument::Indented));}catch(...){code=2;}
  std::printf("%s\n",QJsonDocument(result).toJson(QJsonDocument::Compact).constData());return code;
}
