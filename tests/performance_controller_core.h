#pragma once

// Included inside the performance executable's private namespace. This facade
// only drives production controllers and observes their signals; it does not
// capture raw input, derive facts, or own another analysis worker.
class BenchmarkCore final : public QObject {
public:
  using Ready = std::function<void(const QJsonObject&)>;
  using Result = std::function<void(QJsonObject, std::shared_ptr<const AnalysisWorkResult>)>;
  using Completed = std::function<void(QJsonObject)>;
  BenchmarkCore(QString root, std::shared_ptr<PerformanceDataset> data, BenchmarkOptions options,
                QObject* gui, Ready ready, Result result, Completed completed)
      : data_(std::move(data)), options_(std::move(options)), gui_(gui), ready_(std::move(ready)),
        result_(std::move(result)), completed_(std::move(completed)) {
    storage_ = new StorageService(root, {}, {}, this);
    repository_ = new PetRepository(this, storage_, QDir(root).filePath(QStringLiteral("legacy-empty")));
    connect(storage_, &StorageService::completed, this, [this](const StorageResult& value) {
      if (!measuring_) return;
      if (value.absolutePath.contains(QStringLiteral("/details/")) || value.absolutePath.contains(QStringLiteral("\\details\\"))) {
        const QString status = storageStatusName(value.status);
        rawReadStatuses_[status] += 1;
        rawReadActiveNs_ += value.readElapsedNanoseconds;
        rawReadQueueNs_ += value.queueWaitNanoseconds;
        if (value.status == StorageStatus::Loaded) { readBytes_ += value.content.size(); ++readCount_; }
        else if (value.status == StorageStatus::NotFound) ++missingReads_;
      }
      sampleMemory();
    });
    connect(repository_,&PetRepository::rawRecordLoadFinished,this,[this](const PetRecordKey&,bool loaded,const QString& error,StorageStatus status) {
      if (measuring_ && !loaded && !error.isEmpty()) rawLoadErrors_[error] += 1;
      if (measuring_) rawLoadStatuses_[storageStatusName(status)] += 1;
    });
    pulse_.setParent(this); pulse_.setInterval(5); pulse_.setTimerType(Qt::PreciseTimer);
    connect(&pulse_, &QTimer::timeout, this, [this] {
      const qint64 time = nowNs();
      if (pulseAt_) coreGapNs_ = qMax(coreGapNs_, time - pulseAt_);
      pulseAt_ = time; sampleMemory();
      if (controller_ && !priorityProbePending_) {
        const quint64 id = generation_;
        QPointer<BenchmarkCore> self(this);
        priorityProbePending_ = controller_->postPriorityCompute([self,time,id] {
          const qint64 wait = nowNs() - time;
          if (self) QMetaObject::invokeMethod(self,[self,id,wait] {
            if (!self) return;
            self->priorityProbePending_ = false;
            if (self->generation_ == id) self->computePriorityWaitNs_ = qMax(self->computePriorityWaitNs_,wait);
          },Qt::QueuedConnection);
        });
      }
    });
  }
  ~BenchmarkCore() override {
    // close() has already stopped Compute and IO. Destroy dependent facades
    // before the shared Storage QObject, independent of child creation order.
    delete controller_; controller_ = nullptr;
    delete cache_; cache_ = nullptr;
    delete shop_; shop_ = nullptr;
    delete repository_; repository_ = nullptr;
    delete storage_; storage_ = nullptr;
  }
  void prepare() {
    if (!data_->error.isEmpty()) { publishReady(); return; }
    if (repository_->pendingReadCount() || repository_->pendingPersistenceCount() || repository_->cacheLoading()) {
      QTimer::singleShot(5,this,[this] { prepare(); }); return;
    }
    if (!loggedIn_) {
      loggedIn_ = true;
      deliverVerifiedFixture(repository_, {{QStringLiteral("_cmd"),QStringLiteral("21_1")},
          {QStringLiteral("info"),QJsonObject{{QStringLiteral("n"),QStringLiteral("performance-fixture")}}}});
      QTimer::singleShot(5,this,[this] { prepare(); }); return;
    }
    if (!listsLoaded_) {
      listsLoaded_ = true;
      QJsonArray backpack, normal, elite, positions;
      int index = 0;
      for (const auto& pet : data_->summaries) {
        if (pet.location == QStringLiteral("背包")) { backpack.append(performanceFixturePet(*data_,index)); positions.append(QString::number(pet.instanceId)); }
        else if (pet.location == QStringLiteral("普通仓库")) normal.append(pet.pet);
        else elite.append(pet.pet);
        ++index;
      }
      repository_->beginListRefresh(1,repository_->accountKey(),repository_->sessionGeneration());
      deliverVerifiedFixture(repository_, {{QStringLiteral("_cmd"),QStringLiteral("2_1_10")},
          {QStringLiteral("pl"),backpack},{QStringLiteral("pps"),positions},{QStringLiteral("ppc"),12}});
      repository_->expectListPart(QStringLiteral("2_1_S"),1,repository_->accountKey(),repository_->sessionGeneration());
      deliverVerifiedFixture(repository_, {{QStringLiteral("_cmd"),QStringLiteral("2_1_S")},
          {QStringLiteral("ns"),normal},{QStringLiteral("es"),elite},{QStringLiteral("rb"),QJsonArray{}}});
      data_->directory = QFileInfo(repository_->cachePath()).absolutePath() + QStringLiteral("/details");
      QTimer::singleShot(5,this,[this] { prepare(); }); return;
    }
    if (repository_->currentInstanceIds().size() != data_->spec.pets) {
      data_->error = QStringLiteral("production Repository rejected fixture membership: %1/%2")
          .arg(repository_->currentInstanceIds().size()).arg(data_->spec.pets); publishReady(); return;
    }
    const auto data = data_;
    QPointer<BenchmarkCore> self(this);
    if (!storage_->postAuxiliary([self,data](QObject*) {
      const bool finished = writePerformanceFixtureBatch(*data);
      if (self) QMetaObject::invokeMethod(self,[self,finished] {
        if (!self || self->closing_) return;
        if (finished || !self->data_->error.isEmpty()) self->initializeControllers();
        else self->prepare();
      },Qt::QueuedConnection);
    })) QTimer::singleShot(1,this,[this] { prepare(); });
  }
  void start(int index, bool warmup, qint64 clickedAt) {
    if (closing_) return;
    ++generation_; active_ = measuring_ = true; runIndex_ = index; warmup_ = warmup;
    started_ = clickedAt; guiToCoreQueueNs_ = nowNs()-clickedAt;
    last_.reset(); readBytes_ = readCount_ = missingReads_ = 0;
    rawReadActiveNs_ = rawReadQueueNs_ = 0;
    rawReadStatuses_.clear(); rawLoadErrors_.clear(); rawLoadStatuses_.clear();
    capturedAt_ = cancelRequestedAt_ = coreGapNs_ = computePriorityWaitNs_ = 0;
    rawPeak_ = factsPeak_ = inputPeak_ = resultPeak_ = ioPeak_ = logicalSumPeak_ = 0;
    previousCache_ = cache_->stats(); sendsAtStart_ = fixtureSends_;
    previousRepositoryIo_ = repository_->ioMetrics();
    readAdmissionRetriesAtStart_ = controller_->preparationReadAdmissionRetries();
    pulseAt_ = nowNs(); pulse_.start(); sampleMemory();
    if (!cacheAttached_) { cacheAttached_ = true; controller_->setDerivationCache(cache_); }
    controller_->requestAnalysis();
    expectedJob_ = controller_->currentAnalysisJob();
    if (options_.cancelAfterMs >= 0 && options_.cancelPhase == QStringLiteral("preparation")) armCancellation();
  }
  void finalize(QJsonObject metrics) {
    final_ = std::move(metrics);
    if (!last_ || !options_.save) { finishProbes(); return; }
    saveStarted_ = nowNs(); waitingSnapshot_ = true;
    const bool accepted = controller_->recordSnapshot();
    final_.insert(QStringLiteral("snapshotAdmissionNs"),nowNs()-saveStarted_);
    if (!accepted) { waitingSnapshot_ = false; final_.insert(QStringLiteral("snapshotSaved"),false);
      final_.insert(QStringLiteral("snapshotError"),lastStatus_); finishProbes(); }
  }
  bool close() {
    closing_ = true; pulse_.stop();
    QElapsedTimer timer; timer.start();
    const bool compute = !controller_ || controller_->shutdownAnalysis(2000);
    const bool io = storage_->shutdown(static_cast<unsigned long>(qMax<qint64>(0,2000-timer.elapsed())));
    return compute && io;
  }
private:
  template<class Action> void gui(Action action) {
    const QPointer<QObject> target = gui_;
    if (target) QMetaObject::invokeMethod(target,[target,action=std::move(action)] { if (target) action(); },Qt::QueuedConnection);
  }
  ObservationClockSample clockSample() const {
    return {QDateTime(QDate(2026,9,9),QTime(10,0),Qt::UTC),1000,0,QStringLiteral("synthetic/UTC")};
  }
  void deliverShop(const QJsonObject& packet) {
    auto envelope = verifiedFixtureEnvelope(repository_,packet);
    envelope.receivedMonotonicMs = clockSample().monotonicMs;
    repository_->handleEnvelope(envelope);
  }
  void initializeControllers() {
    if (!data_->error.isEmpty()) { publishReady(); return; }
    if (shop_) { waitForFixtureIdle(); return; }
    shop_ = new ShopExchangeController(repository_,this);
    shop_->setObservationClock([this] { return clockSample(); });
    connect(repository_,&PetRepository::packetObserved,shop_,[this](const QJsonObject& packet,const InboundEnvelope& envelope) {
      shop_->handleDecodedEnvelope(envelope,packet);
    });
    shop_->setSender([this](const QString&,const QString&,const QString&) { ++fixtureSends_; return true; });
    // The isolated sender has no Bridge or network capability. Only setup
    // creates the two request expectations required by the production parser.
    if (!shop_->requestInfo()) { data_->error = QStringLiteral("fixture shop request setup rejected"); publishReady(); return; }
    auto packet = data_->shopPacket;
    packet.insert(QStringLiteral("_cmd"),ShopExchangeCatalog::instance().getInfoCommand());
    for (const auto& definition : ShopExchangeCatalog::instance().snapshot()->allShops) {
      const QString key = QStringLiteral("si%1").arg(definition.shopId);
      if (!packet.contains(key)) packet.insert(key,QJsonObject{});
    }
    deliverShop(packet);
    QJsonObject materials{{QStringLiteral("_cmd"),QStringLiteral("3_11")},
        {QStringLiteral("4"),QJsonArray{}},{QStringLiteral("8"),QJsonArray{}}};
    for (auto it = data_->balances.constBegin(); it != data_->balances.constEnd(); ++it) {
      const auto fields = it.key().split(QLatin1Char(':'));
      if (fields.front() == QStringLiteral("134")) continue;
      auto array = materials.value(fields.front()).toArray();
      array.append(QJsonObject{{QStringLiteral("i"),fields.back().toInt()},{QStringLiteral("n"),it.value()}});
      materials.insert(fields.front(),array);
    }
    deliverShop(materials);
    if (data_->balances.contains(QStringLiteral("134:1"))) {
      const QJsonObject member{{QStringLiteral("lCToken"),data_->balances.value(QStringLiteral("134:1"))}};
      deliverShop({{QStringLiteral("_cmd"),QStringLiteral("1015_2A")},
          {QStringLiteral("infos"),QJsonObject{{QStringLiteral("UnionMemberInfo"),member}}}});
    }
    QSet<QString> supplied;
    for (const auto& good : data_->goods) if (!good.provenUnlimited) {
      const QString group = QStringLiteral("si%1").arg(good.shopId);
      const QString key = shopQuotaValidityKey(good.shopId,good.limitKey);
      if (supplied.contains(key)) continue;
      const auto clock = clockSample();
      TrustedObservationValidity proof{repository_->accountKey(),repository_->sessionGeneration(),group,
          shop_->observedSequence(group),good.limitKey,QStringLiteral("synthetic-period-2026-09-09"),
          QDateTime(QDate(2026,9,9),QTime(0,0),Qt::UTC),QDateTime(QDate(2026,9,10),QTime(0,0),Qt::UTC),clock.utc,0,
          QStringLiteral("tests/performance_controller_core.h synthetic frozen server period evidence")};
      QString error;
      if (!shop_->acceptQuotaValidityEvidence(proof,&error)) { data_->error = QStringLiteral("synthetic period admission: ")+error; break; }
      supplied.insert(key);
    }
    AnalysisEnvironment environment;
    environment.businessDate = [] { return QDate(2026,9,9); };
    const auto catalog = data_->catalog; const auto metadata = data_->metadataCatalog;
    environment.shopCatalogSnapshot = [catalog] { return catalog; };
    environment.petMetadataSnapshot = [metadata] { return metadata; };
    controller_ = new AssetAnalysisController(repository_,shop_,nullptr,this,std::move(environment));
    controller_->setCompatibilityIdentity(QStringLiteral("offline-performance-harness"),QStringLiteral("synthetic-source"),true);
    cache_ = new PetDerivationCache([this](std::function<void()> work) {
      return controller_->postPriorityCompute(std::move(work));
    },storage_,{},this); // All production budgets retain their default values.
    connect(controller_,&AssetAnalysisController::analysisInputCaptured,this,[this](quint64) {
      if (!active_) return;
      capturedAt_ = nowNs(); sampleMemory();
      if (options_.cancelAfterMs >= 0 && options_.cancelPhase == QStringLiteral("worker")) armCancellation();
    });
    connect(controller_,&AssetAnalysisController::analysisJobFinished,this,[this](const AnalysisJobFinished& value) { finished(value); });
    connect(controller_,&AssetAnalysisController::statusChanged,this,[this](const QString& status) { lastStatus_ = status; });
    connect(controller_,&AssetAnalysisController::persistenceChanged,this,
        [this](const QString&,quint64,const QString& record,quint64,StorageStatus status,const QString& error) {
      if (!waitingSnapshot_ || record == QStringLiteral("asset-analysis.json") || status == StorageStatus::Queued || status == StorageStatus::Superseded) return;
      waitingSnapshot_ = false; final_.insert(QStringLiteral("snapshotSaveNs"),nowNs()-saveStarted_);
      final_.insert(QStringLiteral("snapshotSaved"),status == StorageStatus::Saved);
      if (!error.isEmpty()) final_.insert(QStringLiteral("snapshotError"),error);
      finishProbes();
    });
    waitForFixtureIdle();
  }
  void waitForFixtureIdle() {
    if (repository_->pendingReadCount() || repository_->pendingPersistenceCount() || repository_->cacheLoading() ||
        storage_->state().outstandingTasks || shop_->pendingStorageCount() || controller_->persistencePendingTaskCount()) {
      QTimer::singleShot(5,this,[this] { waitForFixtureIdle(); }); return;
    }
    publishReady();
  }
  void publishReady() {
    auto row = data_->description(); row.insert(QStringLiteral("error"),data_->error);
    row.insert(QStringLiteral("fixtureRequestExpectations"),fixtureSends_);
    row.insert(QStringLiteral("derivedRamEntriesBeforeFirstClick"),cache_ ? cache_->stats().residentEntries : 0);
    row.insert(QStringLiteral("derivationsBeforeFirstClick"),cache_ ? qint64(cache_->stats().computations) : 0);
    row.insert(QStringLiteral("coldDerivedDiskInitiallyEmpty"),true);
    row.insert(QStringLiteral("rawObservedResidentRecordsBeforeClick"),repository_->rawCacheStats().residentRecords);
    row.insert(QStringLiteral("rawObservedChargedBytesBeforeClick"),qint64(repository_->rawCacheStats().chargedBytes));
    row.insert(QStringLiteral("sourceInputs"),QStringLiteral("schema3 durable originals + verified synthetic summary/shop envelopes; no captured or prederived AnalysisWorkInput"));
    const auto callback = ready_; gui([callback,row] { callback(row); });
  }
  void armCancellation() {
    const quint64 id = generation_;
    QTimer::singleShot(options_.cancelAfterMs,this,[this,id] {
      if (id == generation_ && active_ && controller_->analysisRunning()) {
        cancelRequestedAt_ = nowNs(); controller_->cancelAnalysis();
      }
    });
  }
  QJsonObject cacheJson() const {
    const auto raw = repository_->rawCacheStats(); const auto facts = cache_->stats(); const auto io = storage_->state();
    return {{QStringLiteral("rawChargedBytes"),qint64(raw.chargedBytes)},{QStringLiteral("rawResidentBytes"),qint64(raw.residentBytes)},
        {QStringLiteral("rawResidentRecords"),raw.residentRecords},{QStringLiteral("rawProtectedRecords"),raw.protectedRecords},
        {QStringLiteral("rawEvictedRecordsLifetime"),qint64(raw.evictedRecords)},{QStringLiteral("rawUntrackedExportsLifetime"),qint64(raw.untrackedExports)},
        {QStringLiteral("factsResidentBytes"),qint64(facts.residentFactsBytes)},{QStringLiteral("factsRetainedBytes"),qint64(facts.retainedFactsBytes)},
        {QStringLiteral("factsReservedBytes"),qint64(facts.reservedFactsBytes)},{QStringLiteral("factsQueuedInputBytes"),qint64(facts.queuedInputBytes)},
        {QStringLiteral("factsMetadataBytes"),qint64(facts.metadataBytes)},{QStringLiteral("factsResidentEntries"),facts.residentEntries},
        {QStringLiteral("factsPendingTasks"),facts.pendingTasks},{QStringLiteral("indexIntentBytes"),qint64(facts.indexIntentBytes)},
        {QStringLiteral("indexPendingTasks"),facts.pendingIndexTasks},{QStringLiteral("ioOutstandingBytes"),io.outstandingBytes},
        {QStringLiteral("ioOutstandingTasks"),io.outstandingTasks},
        {QStringLiteral("rawPeakBytesSampled"),qint64(rawPeak_)},{QStringLiteral("factsPeakBytesSampled"),qint64(factsPeak_)},
        {QStringLiteral("workerInputPeakBytesSampled"),qint64(inputPeak_)},{QStringLiteral("workerResultPeakBytesSampled"),qint64(resultPeak_)},
        {QStringLiteral("ioPeakBytesSampled"),qint64(ioPeak_)},{QStringLiteral("conservativeConcurrentLedgerSumPeakSampled"),qint64(logicalSumPeak_)}};
  }
  void sampleMemory() {
    if (!controller_ || !cache_) return;
    const auto raw = repository_->rawCacheStats(); const auto facts = cache_->stats();
    const auto worker = controller_->analysisMemoryUsage(); const auto io = storage_->state();
    const quint64 factBytes = facts.retainedFactsBytes + facts.reservedFactsBytes + facts.queuedInputBytes + facts.metadataBytes + facts.indexIntentBytes;
    const quint64 ioBytes = qMax<qint64>(0,io.outstandingBytes);
    rawPeak_ = qMax(rawPeak_,raw.chargedBytes); factsPeak_ = qMax(factsPeak_,factBytes);
    inputPeak_ = qMax(inputPeak_,worker.inputChargedBytes); resultPeak_ = qMax(resultPeak_,worker.resultChargedBytes);
    ioPeak_ = qMax(ioPeak_,ioBytes);
    // Separate ledgers can conservatively charge the same shared payload.
    // This is an upper bound across named ledgers, not a physical allocation.
    logicalSumPeak_ = qMax(logicalSumPeak_,raw.chargedBytes+factBytes+ioBytes+worker.inputChargedBytes+worker.resultChargedBytes+worker.compiledCatalogChargedBytes);
  }
  void finished(const AnalysisJobFinished& value) {
    if (!active_ || closing_ || value.key.jobId != expectedJob_.jobId ||
        value.key.account != expectedJob_.account || value.key.epoch != expectedJob_.epoch) return;
    active_ = false; sampleMemory(); pulse_.stop();
    const qint64 completedAt = nowNs();
    last_ = value.outcome == AnalysisJobOutcome::Published ? controller_->retainedAnalysisResult() : nullptr;
    const auto cache = cache_->stats();
    const auto repositoryIo = repository_->ioMetrics();
    QJsonObject readStatuses, loadErrors, loadStatuses;
    for (auto it=rawReadStatuses_.cbegin(); it!=rawReadStatuses_.cend(); ++it) readStatuses.insert(it.key(),it.value());
    for (auto it=rawLoadErrors_.cbegin(); it!=rawLoadErrors_.cend(); ++it) loadErrors.insert(it.key(),it.value());
    for (auto it=rawLoadStatuses_.cbegin(); it!=rawLoadStatuses_.cend(); ++it) loadStatuses.insert(it.key(),it.value());
    QJsonObject row{{QStringLiteral("kind"),QStringLiteral("sample")},{QStringLiteral("caseId"),data_->spec.id()},
        {QStringLiteral("sampleIndex"),runIndex_},{QStringLiteral("warmup"),warmup_},{QStringLiteral("cacheMode"),options_.cacheMode},
        {QStringLiteral("finishedJob"),QJsonObject{{QStringLiteral("jobId"),qint64(value.key.jobId)},
          {QStringLiteral("account"),value.key.account},{QStringLiteral("epoch"),qint64(value.key.epoch)},
          {QStringLiteral("inventoryRevision"),qint64(value.key.versions.inventory)},
          {QStringLiteral("detailRevision"),qint64(value.key.versions.details)},
          {QStringLiteral("catalogRevision"),qint64(value.key.versions.catalog)},
          {QStringLiteral("metadataRevision"),qint64(value.key.versions.metadata)},
          {QStringLiteral("catalogDay"),qint64(value.key.versions.catalogDay)},
          {QStringLiteral("quotaFreshnessRevision"),qint64(value.key.versions.quotaFreshness)},
          {QStringLiteral("build"),value.key.versions.build},{QStringLiteral("profile"),value.key.versions.profile}}},
        {QStringLiteral("clickMonotonicNs"),started_},{QStringLiteral("guiToCoreQueueNs"),guiToCoreQueueNs_},
        {QStringLiteral("preparationToFrozenInputNs"),capturedAt_ ? capturedAt_-started_-guiToCoreQueueNs_ : completedAt-started_-guiToCoreQueueNs_},
        {QStringLiteral("workerQueueMeterPrepareCalculateNs"),capturedAt_ ? completedAt-capturedAt_ : 0},
        {QStringLiteral("readBytes"),readBytes_},{QStringLiteral("readFiles"),readCount_},{QStringLiteral("missingOriginalReadFiles"),missingReads_},
        {QStringLiteral("rawStorageStatusCounts"),readStatuses},{QStringLiteral("rawLoadStatusCounts"),loadStatuses},{QStringLiteral("rawLoadErrorCounts"),loadErrors},
        {QStringLiteral("rawDiskReadActiveWallNs"),rawReadActiveNs_},{QStringLiteral("rawDiskReadQueueWaitNs"),rawReadQueueNs_},
        {QStringLiteral("coreRawJsonDecodeCalls"),qint64(repositoryIo.rawJsonDecodeCalls-previousRepositoryIo_.rawJsonDecodeCalls)},
        {QStringLiteral("coreRawJsonDecodeNs"),repositoryIo.rawJsonDecodeNanoseconds-previousRepositoryIo_.rawJsonDecodeNanoseconds},
        {QStringLiteral("coreRawApplyNs"),repositoryIo.rawApplyNanoseconds-previousRepositoryIo_.rawApplyNanoseconds},
        {QStringLiteral("preparationReadAdmissionRetries"),qint64(controller_->preparationReadAdmissionRetries()-readAdmissionRetriesAtStart_)},
        {QStringLiteral("sourcePreparationIncludedInClick"),true},{QStringLiteral("outcome"),outcomeName(value.outcome)},
        {QStringLiteral("finishStage"),value.phase == AnalysisJobPhase::Compute ? QStringLiteral("worker") :
          value.phase == AnalysisJobPhase::Preparation ? QStringLiteral("preparation") : QStringLiteral("queued")},
        {QStringLiteral("coreEventLoopMaximumGapNs"),coreGapNs_},{QStringLiteral("computePriorityMaximumWaitNs"),computePriorityWaitNs_},
        {QStringLiteral("networkRequests"),fixtureSends_-sendsAtStart_},{QStringLiteral("networkWaitNs"),0},
        {QStringLiteral("cancellationPhaseRequested"),options_.cancelPhase},{QStringLiteral("cancellationRequested"),cancelRequestedAt_>0},
        {QStringLiteral("cancellationLatencyNs"),value.cancellationLatencyNanoseconds},
        {QStringLiteral("cancellationEndToEndNs"),cancelRequestedAt_ ? completedAt-cancelRequestedAt_ : 0},
        {QStringLiteral("measuredInputBytes"),qint64(value.measuredInputBytes)},{QStringLiteral("inputMeasurementComplete"),value.inputMeasurementComplete},
        {QStringLiteral("maximumInputMeterSliceNs"),value.maximumInputMeterSliceNanoseconds},
        {QStringLiteral("maximumComputeSliceNs"),value.slices.maximumSliceNanoseconds},
        {QStringLiteral("maximumAtomicComputeNs"),value.slices.maximumAtomicWorkNanoseconds},{QStringLiteral("sortNs"),value.slices.sortNanoseconds},
        {QStringLiteral("resultRetainedByteBreakdown"),QJsonObject{{QStringLiteral("recommendationHeap"),qint64(value.slices.resultRowHeapBytes)},
          {QStringLiteral("recommendationCapacity"),qint64(value.slices.resultRowCapacityBytes)},
          {QStringLiteral("overviewHeap"),qint64(value.slices.overviewHeapBytes)},
          {QStringLiteral("overviewCapacity"),qint64(value.slices.overviewCapacityBytes)}}},
        {QStringLiteral("logicalMemory"),memoryJson(controller_->analysisMemoryUsage())},{QStringLiteral("productionCaches"),cacheJson()},
        {QStringLiteral("cacheComputations"),qint64(cache.computations-previousCache_.computations)},
        {QStringLiteral("cacheComputeActiveWallNs"),qint64(cache.completedComputeMicroseconds-previousCache_.completedComputeMicroseconds)*1000},
        {QStringLiteral("cacheRawDeriveActiveWallNs"),qint64(cache.completedRawDeriveMicroseconds-previousCache_.completedRawDeriveMicroseconds)*1000},
        {QStringLiteral("cacheIndexDecodeActiveWallNs"),qint64(cache.completedIndexDecodeMicroseconds-previousCache_.completedIndexDecodeMicroseconds)*1000},
        {QStringLiteral("cacheHits"),qint64(cache.cacheHits-previousCache_.cacheHits)},
        {QStringLiteral("derivedIndexHits"),qint64(cache.indexHits-previousCache_.indexHits)},
        {QStringLiteral("derivedIndexMisses"),qint64(cache.indexMisses-previousCache_.indexMisses)},
        {QStringLiteral("maximumCacheComputeNsLifetime"),cache.maximumComputeMicroseconds*1000},
        {QStringLiteral("productionLimits"),QJsonObject{{QStringLiteral("inputBytes"),qint64(AnalysisMemoryLimits{}.inputBytes)},
          {QStringLiteral("resultBytes"),qint64(AnalysisMemoryLimits{}.resultBytes)},{QStringLiteral("totalBytes"),qint64(AnalysisMemoryLimits{}.totalBytes)},
          {QStringLiteral("rawBytes"),qint64(PetRecordCacheLimits{}.maximumBytes)},
          {QStringLiteral("factsResidentBytes"),qint64(PetDerivationCacheLimits{}.residentBytes)},
          {QStringLiteral("factsRetainedBytes"),qint64(PetDerivationCacheLimits{}.retainedBytes)},
          {QStringLiteral("storageOutstandingBytes"),StorageLimits{}.maximumOutstandingBytes},
          {QStringLiteral("storageRecordBytes"),StorageLimits{}.maximumRecordBytes}}}};
    if (last_) {
      row.insert(QStringLiteral("pipeline"),pipelineStatistics(last_->work));
      row.insert(QStringLiteral("maximumCatalogPreparationSliceNs"),last_->maximumPreparationSliceNanoseconds);
      row.insert(QStringLiteral("maximumAtomicCatalogPreparationNs"),last_->maximumAtomicPreparationNanoseconds);
      row.insert(QStringLiteral("catalogCacheHit"),last_->catalogCacheHit);
      row.insert(QStringLiteral("activeInputMeterNs"),last_->activeInputMeterNanoseconds);
      row.insert(QStringLiteral("catalogCompileNs"),last_->catalogCompileNanoseconds);
      row.insert(QStringLiteral("conditionPrepareNs"),last_->conditionPrepareNanoseconds);
      row.insert(QStringLiteral("candidateComputeNs"),last_->candidateComputeNanoseconds);
      row.insert(QStringLiteral("sortNs"),last_->sortNanoseconds);
      row.insert(QStringLiteral("productionComputeActiveWallNs"),
          qint64(cache.completedComputeMicroseconds-previousCache_.completedComputeMicroseconds)*1000 +
          last_->activeInputMeterNanoseconds + last_->catalogCompileNanoseconds +
          last_->conditionPrepareNanoseconds + last_->candidateComputeNanoseconds + last_->sortNanoseconds);
      row.insert(QStringLiteral("resultPets"),last_->overview.pets.size());
      row.insert(QStringLiteral("resultMissingDetailPets"),last_->overview.missingDetailPets);
      row.insert(QStringLiteral("resultRecommendations"),last_->recommendations.size());
      row.insert(QStringLiteral("correct"),last_->overview.pets.size()==data_->spec.pets &&
          last_->overview.missingDetailPets==data_->spec.pets-data_->detailedIndexes.size() &&
          last_->work.goodsCompiled+last_->work.goodsReused==data_->goods.size() &&
          last_->work.accountConditionsPrepared==data_->goods.size() &&
          last_->work.candidatePairsVisited==data_->expectedQualifiedPairs && last_->work.maximumLiveCandidates<=2 &&
          last_->work.preparedFactsReused==data_->spec.pets && last_->work.rawPowerCalculations==0 && fixtureSends_==sendsAtStart_);
    } else {
      row.insert(QStringLiteral("correct"),options_.cancelAfterMs>=0 && cancelRequestedAt_>0 && value.outcome==AnalysisJobOutcome::Cancelled);
      row.insert(QStringLiteral("error"),value.error.isEmpty() ? lastStatus_ : value.error);
    }
    const auto callback = result_; const auto result = last_;
    gui([callback,row,result] { callback(row,result); });
  }
  void finishProbes() {
    if (last_ && options_.probes) {
      // These independent probes contain catalog/account stages only. No
      // reconstruction of raw cultivation is allowed after the real run.
      auto input = performanceInputTemplate(*data_,last_->key);
      input.overview.pets.clear(); input.overview.totalPets = 0;
      const auto goods = data_->goods; QPointer<BenchmarkCore> self(this);
      if (!controller_->postPriorityCompute([self,input=std::move(input),goods] {
        auto probes = performancePhaseProbes(input,goods);
        probes.remove(QStringLiteral("petDeriveNs")); probes.remove(QStringLiteral("candidateQualificationNs"));
        probes.insert(QStringLiteral("scope"),QStringLiteral("catalog-and-account-only; actual pet preparation is measured in the production cache"));
        if (self) QMetaObject::invokeMethod(self,[self,probes] {
          if (!self || self->closing_) return;
          self->final_.insert(QStringLiteral("independentPhaseProbes"),probes);
          for (const auto& pair : {qMakePair(QStringLiteral("probeCatalogCompileNs"),QStringLiteral("catalogCompileNs")),
              qMakePair(QStringLiteral("probeMaterialIndexNs"),QStringLiteral("materialIndexNs")),
              qMakePair(QStringLiteral("probeAccountPrepareNs"),QStringLiteral("accountPrepareNs"))}) self->final_.insert(pair.first,probes.value(pair.second));
          self->completeSample();
        },Qt::QueuedConnection);
      })) QTimer::singleShot(1,this,[this] { finishProbes(); });
      return;
    }
    completeSample();
  }
  void completeSample() {
    // Finish all necessary derived-index persistence before reusing the warm
    // state. Its time is reported separately and never shifted out of a cold
    // click that needed reads or derivation.
    if (cache_->stats().pendingTasks || cache_->stats().pendingIndexTasks || repository_->pendingPersistenceCount() || repository_->pendingReadCount() ||
        controller_->persistencePendingTaskCount() || storage_->state().outstandingTasks) {
      QTimer::singleShot(5,this,[this] { completeSample(); }); return;
    }
    sampleMemory(); measuring_ = false;
    final_.insert(QStringLiteral("sampleThroughPersistenceNs"),nowNs()-started_);
    final_.insert(QStringLiteral("logicalMemoryAfterSaveAndProbes"),memoryJson(controller_->analysisMemoryUsage()));
    final_.insert(QStringLiteral("productionCachesAfterPersistence"),cacheJson());
    const auto callback = completed_; const auto row = final_; gui([callback,row] { callback(row); });
  }
  std::shared_ptr<PerformanceDataset> data_; BenchmarkOptions options_; QPointer<QObject> gui_;
  Ready ready_; Result result_; Completed completed_;
  StorageService* storage_ = nullptr; PetRepository* repository_ = nullptr;
  ShopExchangeController* shop_ = nullptr; AssetAnalysisController* controller_ = nullptr; PetDerivationCache* cache_ = nullptr;
  std::shared_ptr<const AnalysisWorkResult> last_; PetDerivationCacheStats previousCache_;
  PetRepositoryIoMetrics previousRepositoryIo_;
  quint64 readAdmissionRetriesAtStart_ = 0;
  AnalysisJobKey expectedJob_;
  quint64 generation_ = 0;
  int runIndex_ = 0, readCount_ = 0, missingReads_ = 0, fixtureSends_ = 0, sendsAtStart_ = 0;
  bool warmup_ = false, closing_ = false, active_ = false, measuring_ = false, priorityProbePending_ = false;
  bool loggedIn_ = false, listsLoaded_ = false, cacheAttached_ = false, waitingSnapshot_ = false;
  qint64 started_ = 0, guiToCoreQueueNs_ = 0, capturedAt_ = 0, readBytes_ = 0;
  qint64 rawReadActiveNs_ = 0, rawReadQueueNs_ = 0;
  qint64 coreGapNs_ = 0, computePriorityWaitNs_ = 0, pulseAt_ = 0, cancelRequestedAt_ = 0, saveStarted_ = 0;
  quint64 rawPeak_ = 0, factsPeak_ = 0, inputPeak_ = 0, resultPeak_ = 0, ioPeak_ = 0, logicalSumPeak_ = 0;
  QTimer pulse_; QJsonObject final_; QString lastStatus_;
  QHash<QString,int> rawReadStatuses_, rawLoadErrors_, rawLoadStatuses_;
};
