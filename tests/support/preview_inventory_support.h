#pragma once

#include "protocol_test_support.h"
#include "application/analysis/asset_analysis_controller.h"
#include "application/views/inventory_projection.h"
#include "application/views/inventory_publisher.h"
#include "application/pet/pet_derivation_cache.h"
#include "application/pet/pet_detail_preparation_service.h"
#include "domain/pet_identity.h"
#include "storage/storage_service.h"
#include "domain/asset_derivation.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QTextBrowser>
#include <QThread>
#include <QUrl>
#include <functional>
#include <cstdio>

namespace PreviewInventory {
inline QJsonObject brief(const QJsonObject& pet) { return AssetDerivation::identityFields(pet); }
inline bool until(const std::function<bool()>& predicate, int timeoutMs = 10000) {
  QElapsedTimer timer; timer.start();
  do {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    if (predicate()) return true;
    QThread::msleep(1);
  } while (timer.elapsed() < timeoutMs);
  return false;
}

// Preview-only ingress uses explicit synthetic provenance. Windows receive the
// same immutable Projection as production; no test constructs prepared detail
// DTOs. Core facades share this test event loop, with exactly one real Storage
// IO executor and the Controller's real Compute executor for facts/details.
class Fixture final {
public:
  Fixture(const QString& root, const QString& account)
      : storage(root), repository(nullptr,&storage,QDir(root).filePath(QStringLiteral("preview-empty-legacy"))),
        controller(&repository,nullptr,nullptr),
        derivations([this](auto work) { return controller.postPriorityCompute(std::move(work)); },&storage),
        details([this](auto work) { return controller.postPriorityCompute(std::move(work)); }),
        inventory(root,nullptr), publisher(&repository,&inventory) {
    controller.setCompatibilityIdentity(QStringLiteral("synthetic-preview-build"),QStringLiteral("synthetic-preview-profile"),true);
    QObject::connect(&controller,&AssetAnalysisController::derivedFactsChanged,&publisher,
        [this](qint64 id,const PetDerivedFactsHandle& facts) { publisher.factsUpdated(id,facts); });
    QObject::connect(&controller,&AssetAnalysisController::derivedFactsFailed,&publisher,&InventoryPublisher::factsFailed);
    QObject::connect(&inventory,&InventoryProjection::detailInterestsChanged,&publisher,
        [this](const QString& account,quint64 epoch,const QSet<qint64>& ids) {
      if (current(account,epoch)) publisher.setDetailInterests(ids);
    });
    QObject::connect(&inventory,&InventoryProjection::detailSelectionChanged,&publisher,
        [this](const QString& account,quint64 epoch,int consumer,qint64 id) {
      if (current(account,epoch)) publisher.selectDetail(consumer,id);
    });
    QObject::connect(&inventory,&InventoryProjection::detailPageRequested,&publisher,
        [this](const QString& account,quint64 epoch,int consumer,DetailSection section,int page) {
      if (current(account,epoch)) { ++pageRequests; publisher.requestDetailPage(consumer,section,page); }
    });
    publisher.setDetailService(&details,&derivations);
    controller.setDerivationCache(&derivations);
    initialized = waitForRepositoryIdle(&repository,30000) && login(account);
  }
  ~Fixture() { close(); }
  bool login(const QString& account) {
    deliverVerifiedFixture(&repository,{{QStringLiteral("_cmd"),QStringLiteral("21_1")},
        {QStringLiteral("info"),QJsonObject{{QStringLiteral("n"),account}}}});
    return waitForRepositoryIdle(&repository,30000) && until([&] {
      return inventory.accountKey()==account && inventory.snapshot()->sessionEpoch==repository.sessionGeneration();
    },30000);
  }
  bool publishLists(const QJsonArray& backpack,const QJsonArray& normal,const QJsonArray& elite,
                    const QList<QJsonObject>& fullWarehouseDetails = {}) {
    const auto account = repository.accountKey(); const auto epoch = repository.sessionGeneration();
    const auto generation = ++listGeneration_;
    QJsonArray positions;
    for (const auto& pet : backpack) positions.append(QString::number(petInstanceId(pet.toObject())));
    repository.beginListRefresh(generation,account,epoch);
    deliverVerifiedFixture(&repository,{{QStringLiteral("_cmd"),QStringLiteral("2_1_10")},
        {QStringLiteral("pl"),backpack},{QStringLiteral("pps"),positions},{QStringLiteral("ppc"),12}});
    repository.expectListPart(QStringLiteral("2_1_S"),generation,account,epoch);
    deliverVerifiedFixture(&repository,{{QStringLiteral("_cmd"),QStringLiteral("2_1_S")},
        {QStringLiteral("ns"),normal},{QStringLiteral("es"),elite},{QStringLiteral("rb"),QJsonArray{}}});
    for (const auto& pet : fullWarehouseDetails) {
      repository.expectDetail(petInstanceId(pet),++detailRequest_,account,epoch);
      deliverVerifiedFixture(&repository,{{QStringLiteral("_cmd"),QStringLiteral("2_1_R")},{QStringLiteral("p"),pet}});
    }
    return waitForRepositoryIdle(&repository,30000) && until([&] {
      return inventory.backpackPets().size()==backpack.size() &&
          inventory.warehousePets().size()==normal.size()+elite.size();
    },30000);
  }
  bool factsReady(const QList<qint64>& ids = {}) {
    const auto expected = ids.isEmpty() ? repository.currentInstanceIds() : ids;
    const QSet<qint64> expectedSet(expected.begin(),expected.end());
    QSet<qint64> missing = expectedSet;
    const QString account = repository.accountKey();
    const quint64 epoch = repository.sessionGeneration();
    const auto metadata = inventory.metadataSnapshot();
    bool contextChanged = !metadata;
    const auto contextCurrent = [&] {
      const auto currentMetadata = inventory.metadataSnapshot();
      return metadata && currentMetadata && repository.accountKey()==account &&
          repository.sessionGeneration()==epoch && inventory.accountKey()==account &&
          inventory.snapshot() && inventory.snapshot()->sessionEpoch==epoch &&
          metadata->revision==currentMetadata->revision && metadata->contentDigest==currentMetadata->contentDigest;
    };
    const auto refresh = [&](qint64 id) {
      const auto facts = inventory.derivedFactsFor(id);
      if (facts && facts->key.record==repository.recordVersion(id).key) missing.remove(id);
      else missing.insert(id);
    };
    for (qint64 id : expected) refresh(id);
    // Observe completed IDs instead of repeatedly walking an ever-longer ready
    // prefix on the same event loop that must deliver Core/Projection events.
    const auto detail = QObject::connect(&inventory,&InventoryProjection::detailChanged,&inventory,[&](qint64 id) {
      if (expectedSet.contains(id)) refresh(id);
    });
    const auto metadataChanged = QObject::connect(&inventory,&InventoryProjection::metadataChanged,&inventory,[&](quint64) { contextChanged = true; });
    const auto sessionChanged = QObject::connect(&inventory,&InventoryProjection::accountSessionChanged,&inventory,[&](const QString&,quint64) { contextChanged = true; });
    const bool signalled = until([&] {
      if (contextChanged || !contextCurrent()) { contextChanged = true; return true; }
      if (!missing.isEmpty()) return false;
      // A full current-version check remains the final success condition.
      // Membership changes also cannot silently turn an all-pets wait partial.
      const auto currentIds = repository.currentInstanceIds();
      if (ids.isEmpty() && QSet<qint64>(currentIds.begin(),currentIds.end())!=expectedSet) {
        contextChanged = true; return true;
      }
      for (qint64 id : expected) refresh(id);
      return missing.isEmpty();
    },30000);
    QObject::disconnect(detail); QObject::disconnect(metadataChanged); QObject::disconnect(sessionChanged);
    const bool ready = signalled && !contextChanged && contextCurrent() && missing.isEmpty();
    if (!ready) std::fprintf(stderr,"FAIL: projected facts not ready (contextChanged=%d): %s\n",contextChanged,diagnostics().toUtf8().constData());
    return ready;
  }
  QString diagnostics() const {
    const auto raw = repository.rawCacheStats(); const auto facts = derivations.stats(); const auto detail = details.stats();
    int present = 0; QStringList missing;
    const auto ids = repository.currentInstanceIds();
    for (auto id : ids) {
      if (inventory.derivedFactsFor(id)) ++present;
      else if (missing.size()<8) missing.append(QString::number(id));
    }
    return QStringLiteral("epoch=%1 projectionEpoch=%2 ids=%3 facts=%4 missing=[%5] reads=%6 writes=%7 raw=%8/protected=%9 rawBytes=%10 cachePending=%11 active=%12 computations=%13 rejected=%14 detailTasks=%15 detailError=%16")
        .arg(repository.sessionGeneration()).arg(inventory.snapshot() ? inventory.snapshot()->sessionEpoch : 0)
        .arg(ids.size()).arg(present).arg(missing.join(','))
        .arg(repository.pendingReadCount()).arg(repository.pendingPersistenceCount())
        .arg(raw.residentRecords).arg(raw.protectedRecords).arg(raw.chargedBytes)
        .arg(facts.pendingTasks).arg(facts.activeTasks).arg(facts.computations).arg(facts.rejected)
        .arg(detail.activeTasks).arg(inventory.detailPreparationError(1));
  }
  bool preparedVisible(QTextBrowser* browser,int consumer,qint64 id) {
    return browser && until([&] {
      const auto value = inventory.preparedDetail(consumer,id);
      return value && value->detailKnown && browser->toPlainText().contains(value->identity.name) &&
          browser->toPlainText().contains(QStringLiteral("实例 %1").arg(id)) &&
          browser->toHtml().contains(QStringLiteral("kqdetail://page/0/0"));
    });
  }
  bool followPage(QTextBrowser* browser,int consumer,qint64 id,DetailSection section,int pageIndex) {
    if (!browser) return false;
    const QUrl link(QStringLiteral("kqdetail://page/%1/%2").arg(int(section)).arg(pageIndex));
    if (!browser->toHtml().contains(link.toString())) return false;
    const int before = pageRequests;
    if (!QMetaObject::invokeMethod(browser,"anchorClicked",Qt::DirectConnection,Q_ARG(QUrl,link))) return false;
    return until([&] {
      const auto value = inventory.preparedDetail(consumer,id);
      if (!value || pageRequests!=before+1 || !browser->toPlainText().contains(value->identity.name)) return false;
      if (section==DetailSection::Overview) {
        if (value->pages.size()!=6 || !browser->toHtml().contains(QStringLiteral("kqdetail://page/4/1"))) return false;
        for (const auto& page : value->pages) if (page.pageIndex!=0) return false;
        return true;
      }
      for (const auto& page : value->pages) {
        if (page.section!=section || page.pageIndex!=pageIndex) continue;
        return page.entries.size()==64 && page.totalItems==130 &&
            browser->toPlainText().contains(QStringLiteral("第 %1 页").arg(pageIndex+1));
      }
      return false;
    });
  }
  bool close() {
    if (closed_) return clean_;
    closed_ = true; details.shutdown();
    clean_ = controller.shutdownAnalysis(2000) && storage.shutdown(2000);
    return clean_;
  }
  StorageService storage;
  PetRepository repository;
  AssetAnalysisController controller;
  PetDerivationCache derivations;
  PetDetailPreparationService details;
  InventoryProjection inventory;
  InventoryPublisher publisher;
  bool initialized = false;
  int pageRequests = 0;
private:
  bool current(const QString& account,quint64 epoch) const {
    return account==repository.accountKey() && epoch==repository.sessionGeneration();
  }
  quint64 listGeneration_ = 0, detailRequest_ = 0;
  bool closed_ = false, clean_ = false;
};

inline QJsonObject pagedPet(qint64 id,int race,const QString& name,int level,int power) {
  QJsonArray stars;
  for (int index=0; index<130; ++index) stars.append(index%2 ? 66 : 67);
  return {{QStringLiteral("id"),id},{QStringLiteral("r"),race},{QStringLiteral("fr"),race},
      {QStringLiteral("n"),name},{QStringLiteral("lv"),level},{QStringLiteral("zdl"),power},{QStringLiteral("xzdl"),power+2000},
      {QStringLiteral("czdlv"),QJsonObject{{QStringLiteral("lv"),power-100},{QStringLiteral("sgv"),100}}},
      {QStringLiteral("mzdlv"),QJsonObject{{QStringLiteral("lv"),power+1900},{QStringLiteral("sgv"),100}}},
      {QStringLiteral("stargodSlotMaxLevel"),8},{QStringLiteral("sgs"),QStringLiteral("0:8#0:8#0:8")},
      {QStringLiteral("sgsp"),stars},{QStringLiteral("badge"),QStringLiteral("101:5#201:0")},
      {QStringLiteral("shenjue"),QStringLiteral("1034#2#6|7:5")}};
}
}
