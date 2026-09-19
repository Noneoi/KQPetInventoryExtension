#include "pet_detail_preparation_service.h"
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <array>
#include <atomic>
#include <optional>

namespace DetailServiceInternal {
struct State {
  QMutex delivery;
  PetDetailPreparationService* receiver = nullptr;
  std::atomic_bool closing{false};
  std::atomic<int> posted{0};
  std::atomic<quint64> retained{0}, reserved{0}, liveLeases{0}, peak{0};
  quintptr ownerThread = 0;
  void notify() {
    QMutexLocker guard(&delivery);
    if (!receiver || closing.load()) return;
    auto* target = receiver;
    QMetaObject::invokeMethod(target,[target] { target->releasedBudget(); },Qt::QueuedConnection);
  }
  void deliver(std::shared_ptr<Completion> result) {
    QMutexLocker guard(&delivery);
    if (!receiver || closing.load()) return;
    auto* target = receiver;
    QMetaObject::invokeMethod(target,[target,result = std::move(result)] { target->receive(result); },Qt::QueuedConnection);
  }
  void updatePeak() {
    const quint64 value = retained.load() + reserved.load(); quint64 old = peak.load();
    while (old < value && !peak.compare_exchange_weak(old,value)) {}
  }
};
struct Reservation {
  std::shared_ptr<State> state;
  std::atomic<quint64> bytes{0};
  void release() { state->reserved.fetch_sub(bytes.exchange(0)); }
  ~Reservation() { release(); }
};
struct Retention {
  std::shared_ptr<State> state;
  PetDerivedFactsHandle facts;
  quint64 bytes = 0;
  ~Retention() { facts.reset(); state->retained.fetch_sub(bytes); state->liveLeases.fetch_sub(1); state->notify(); }
};
struct Posted {
  std::shared_ptr<State> state;
  ~Posted() { state->posted.fetch_sub(1); state->notify(); }
};
struct Work {
  quint64 id = 0;
  int consumer = 0;
  DetailSelection selection;
  DetailSection section = DetailSection::Overview;
  int pageIndex = 0;
  std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
  std::shared_ptr<Reservation> reservation;
  std::optional<FrozenDetailInputs> input;
  PetDerivedFactsHandle facts;
  std::unique_ptr<PetDetailPreparation> preparation;
  std::optional<QVector<DetailRelatedSummary>> related;
  QSet<qint64> dependencies;
  bool waitingInput = true, waitingSummaries = false, posted = false;
  quint64 lastWorkItems = 0;
  DetailPreparationProgress progress;
};
struct Completion {
  std::shared_ptr<Work> work;
  DetailStep step = DetailStep::Failed;
  DetailPreparationStatus failure = DetailPreparationStatus::InvalidRequest;
  QString error;
  QVector<qint64> needs;
  DetailPreparationProgress progress;
  std::unique_ptr<PreparedPetDetail> result;
  qint64 elapsed = 0;
};
}
using namespace DetailServiceInternal;
namespace {
void attachLease(PreparedPetDetail& result, const std::shared_ptr<void>& lease) {
  result.memoryRetention = lease; result.identity.memoryRetention = lease; result.identity.level.memoryRetention = lease;
  for (auto& field : result.talent) field.memoryRetention = lease;
  for (auto& field : result.sacred) field.memoryRetention = lease;
  for (auto& page : result.pages) {
    page.memoryRetention = lease;
    for (auto& entry : page.entries) { entry.memoryRetention = lease; for (auto& field : entry.fields) field.memoryRetention = lease; }
  }
}
bool sectionValid(DetailSection section) { return int(section) >= int(DetailSection::Overview) && int(section) <= int(DetailSection::CarryRelations); }
}
struct PetDetailPreparationService::Impl {
  ComputeExecutor executor;
  DetailServiceLimits limits;
  std::shared_ptr<State> state = std::make_shared<State>();
  QTimer retry;
  QString account;
  quint64 epoch = 0, nextRequest = 0, metadataRevision = 0;
  QByteArray metadataDigest;
  int nextConsumer = 0;
  struct Slot {
    DetailSelection selection;
    quint64 id = 0;
    DetailSection section = DetailSection::Overview;
    int pageIndex = 0;
    bool dirty = false;
    PreparedPetDetailHandle result;
    QSet<qint64> dependencies;
  };
  std::array<Slot,kDetailConsumerCount> consumers;
  std::shared_ptr<Work> active;
  DetailServiceStats counters;
  void schedule(int consumer) {
    auto& slot = consumers[consumer]; slot.id = ++nextRequest; slot.dirty = true;
    if (active && active->consumer == consumer) active->cancelled->store(true);
    retry.start(0);
  }
  bool current(const std::shared_ptr<Work>& work) const {
    return work && !state->closing.load() && !work->cancelled->load() && consumers[work->consumer].id == work->id &&
        work->selection.account == account && work->selection.epoch == epoch;
  }
};
PetDetailPreparationService::PetDetailPreparationService(ComputeExecutor executor, DetailServiceLimits limits, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
  impl_->executor = std::move(executor); impl_->limits = limits;
  impl_->limits.preparation.sliceItems = qBound(1,limits.preparation.sliceItems,128);
  impl_->limits.preparation.sliceMilliseconds = qBound(1,limits.preparation.sliceMilliseconds,16);
  impl_->limits.preparation.resultBytes = qMax<quint64>(1,qMin(limits.preparation.resultBytes,limits.retainedResultBytes));
  impl_->limits.preparation.expandedCarryResultBytes = qMax<quint64>(1,qMin(limits.preparation.expandedCarryResultBytes,limits.retainedResultBytes));
  impl_->state->receiver = this; impl_->state->ownerThread = reinterpret_cast<quintptr>(QThread::currentThreadId());
  impl_->retry.setSingleShot(true); connect(&impl_->retry,&QTimer::timeout,this,&PetDetailPreparationService::pump);
  qRegisterMetaType<DetailSelection>(); qRegisterMetaType<PreparedPetDetailHandle>(); qRegisterMetaType<DetailPreparationStatus>();
  qRegisterMetaType<FrozenDetailInputs>(); qRegisterMetaType<QVector<DetailRelatedSummary>>();
}
PetDetailPreparationService::~PetDetailPreparationService() { shutdown(); }
DetailSubmission PetDetailPreparationService::request(int consumer, const DetailSelection& selection) {
  DetailSubmission result;
  if (impl_->state->closing.load()) { result.status = DetailPreparationStatus::Closing; return result; }
  if (consumer < 0 || consumer >= kDetailConsumerCount || selection.instanceId <= 0 || selection.account.isEmpty() || selection.account.size() > 1024 ||
      selection.account != impl_->account || selection.epoch != impl_->epoch) {
    result.error = QStringLiteral("invalid detail consumer, instance or session"); return result;
  }
  auto& slot = impl_->consumers[consumer];
  if (slot.id && slot.selection == selection && slot.section == DetailSection::Overview &&
      (slot.result || slot.dirty || (impl_->active && impl_->active->id == slot.id))) {
    result.requestId = slot.id; result.accepted = true; result.status = slot.result ? DetailPreparationStatus::Ready : DetailPreparationStatus::Queued; return result;
  }
  slot.selection = selection; slot.section = DetailSection::Overview; slot.pageIndex = 0; slot.result.reset(); slot.dependencies.clear();
  impl_->schedule(consumer); result.requestId = slot.id; result.accepted = true; result.status = DetailPreparationStatus::Queued;
  emit stateChanged(); return result;
}
DetailSubmission PetDetailPreparationService::requestPage(int consumer, DetailSection section, int pageIndex) {
  DetailSubmission result;
  if (impl_->state->closing.load()) { result.status = DetailPreparationStatus::Closing; return result; }
  if (consumer < 0 || consumer >= kDetailConsumerCount || !impl_->consumers[consumer].id || !sectionValid(section) || pageIndex < 0 || pageIndex > 1000000 ||
      ((section == DetailSection::Overview || section == DetailSection::CarryRelations) && pageIndex != 0)) { result.error = QStringLiteral("invalid detail page"); return result; }
  auto& slot = impl_->consumers[consumer]; slot.section = section; slot.pageIndex = pageIndex;
  impl_->schedule(consumer); result.requestId = slot.id; result.accepted = true; result.status = DetailPreparationStatus::Queued; emit stateChanged(); return result;
}
void PetDetailPreparationService::provideInputs(quint64 requestId, FrozenDetailInputs inputs) {
  const auto work = impl_->active;
  if (!impl_->current(work) || work->id != requestId || !work->waitingInput || work->posted) return;
  if (!inputs.raw || !inputs.facts || inputs.raw->key.account != work->selection.account || inputs.raw->key.epoch != work->selection.epoch ||
      inputs.raw->key.instanceId != work->selection.instanceId || !inputs.metadata ||
      (impl_->metadataRevision && (inputs.metadata->revision != impl_->metadataRevision || inputs.metadata->contentDigest != impl_->metadataDigest))) {
    work->cancelled->store(true); impl_->active.reset(); impl_->retry.start(0);
    emit failed(work->consumer,work->id,DetailPreparationStatus::InvalidRequest,QStringLiteral("provider supplied incompatible selected-detail inputs")); return;
  }
  // Local epoch-zero observations can be displayed, but do not establish a
  // verified live session even if a provider passed an over-optimistic flag.
  if (inputs.raw->key.epoch == 0) inputs.sourceVerified = false;
  work->facts = inputs.facts; work->input = std::move(inputs); work->waitingInput = false; impl_->retry.start(0);
}
void PetDetailPreparationService::rejectInputs(quint64 requestId, const QString& reason) {
  const auto work = impl_->active;
  if (!impl_->current(work) || work->id != requestId || !work->waitingInput || work->posted) return;
  // A terminal provider failure ends this wait. Retain the selection descriptor
  // so a later explicit request can retry, without retaining an old result as
  // a successful cache hit or blocking the other consumer's pending selection.
  auto& consumer = impl_->consumers[work->consumer];
  consumer.result.reset(); consumer.dependencies.clear();
  work->cancelled->store(true); impl_->active.reset(); impl_->retry.start(0);
  const QPointer<PetDetailPreparationService> alive(this);
  emit failed(work->consumer,work->id,DetailPreparationStatus::InvalidRequest,reason);
  if (alive) emit stateChanged();
}
void PetDetailPreparationService::provideRelatedSummaries(quint64 requestId, QVector<DetailRelatedSummary> summaries) {
  const auto work = impl_->active;
  if (!impl_->current(work) || work->id != requestId || !work->waitingSummaries || work->posted) return;
  if (summaries.size() > 32) {
    work->cancelled->store(true); impl_->active.reset(); impl_->retry.start(0);
    emit failed(work->consumer,work->id,DetailPreparationStatus::InvalidRequest,QStringLiteral("related summary batch exceeds 32 entries")); return;
  }
  work->related = std::move(summaries); work->waitingSummaries = false; impl_->retry.start(0);
}
void PetDetailPreparationService::pump() {
  if (impl_->state->closing.load() || impl_->state->posted.load() != 0) return;
  if (impl_->active && !impl_->current(impl_->active)) {
    auto work = std::move(impl_->active); ++impl_->counters.cancelledTasks;
    const QPointer<PetDetailPreparationService> alive(this);
    emit failed(work->consumer,work->id,DetailPreparationStatus::Cancelled,QStringLiteral("selected-detail request was replaced"));
    if (!alive) return;
  }
  if (!impl_->active) {
    int consumer = -1;
    for (int i = 0; i < kDetailConsumerCount; ++i) { const int candidate = (impl_->nextConsumer + i) % kDetailConsumerCount; if (impl_->consumers[candidate].dirty) { consumer = candidate; break; } }
    if (consumer < 0) return;
    auto& slot = impl_->consumers[consumer]; slot.dirty = false; impl_->nextConsumer = (consumer + 1) % kDetailConsumerCount;
    const quint64 reserve = slot.section == DetailSection::CarryRelations
        ? impl_->limits.preparation.expandedCarryResultBytes : impl_->limits.preparation.resultBytes;
    if (impl_->state->retained.load() + impl_->state->reserved.load() > impl_->limits.retainedResultBytes - reserve) {
      emit failed(consumer,slot.id,DetailPreparationStatus::BudgetExceeded,QStringLiteral("visible detail results are retaining the result capacity")); return;
    }
    auto work = std::make_shared<Work>(); work->consumer = consumer; work->id = slot.id; work->selection = slot.selection;
    work->section = slot.section; work->pageIndex = slot.pageIndex;
    work->reservation = std::make_shared<Reservation>(); work->reservation->state = impl_->state; work->reservation->bytes = reserve;
    impl_->state->reserved.fetch_add(reserve); impl_->state->updatePeak(); impl_->active = work;
    emit inputsNeeded(work->id,consumer,work->selection); return;
  }
  const auto work = impl_->active;
  if (work->waitingInput || work->waitingSummaries || work->posted) return;
  if (!impl_->executor) {
    impl_->active.reset(); emit failed(work->consumer,work->id,DetailPreparationStatus::ComputeUnavailable,QStringLiteral("existing Compute executor is unavailable")); return;
  }
  const auto state = impl_->state; const auto limits = impl_->limits.preparation;
  auto posted = std::make_shared<Posted>(); posted->state = state; state->posted.fetch_add(1);
  bool accepted = false;
  try { accepted = impl_->executor([state,work,posted,limits] {
    auto completion = std::make_shared<Completion>(); completion->work = work; QElapsedTimer timer; timer.start();
    try {
      if (state->closing.load() || work->cancelled->load()) completion->step = DetailStep::Cancelled;
      else if (reinterpret_cast<quintptr>(QThread::currentThreadId()) == state->ownerThread) {
        completion->failure = DetailPreparationStatus::ComputeUnavailable; completion->error = QStringLiteral("detail executor ran on Core instead of Compute");
      } else {
        if (!work->preparation && work->input) { work->preparation = std::make_unique<PetDetailPreparation>(std::move(*work->input),work->section,work->pageIndex,limits); work->input.reset(); }
        bool summariesValid = true;
        if (work->related) { summariesValid = work->preparation->provideSummaries(std::move(*work->related)); work->related.reset(); }
        if (!summariesValid) completion->error = QStringLiteral("related summary response does not match its requested identities");
        else {
          completion->step = work->preparation->step(*work->cancelled); completion->progress = work->preparation->progress();
          if (completion->step == DetailStep::NeedSummaries) completion->needs = work->preparation->requestedSummaries();
          if (completion->step == DetailStep::Complete) completion->result = work->preparation->takeResult();
          if (completion->step == DetailStep::Failed) completion->error = work->preparation->error();
        }
      }
    } catch (...) { completion->step = DetailStep::Failed; completion->error = QStringLiteral("detail preparation raised an exception"); }
    completion->elapsed = timer.nsecsElapsed(); state->deliver(std::move(completion));
  }); } catch (...) {}
  if (!accepted) { impl_->retry.start(10); return; }
  work->posted = true; ++impl_->counters.postedSlices;
}
void PetDetailPreparationService::receive(std::shared_ptr<Completion> completion) {
  if (impl_->state->closing.load()) return;
  const auto work = completion->work;
  impl_->counters.completedComputeNanoseconds += completion->elapsed;
  impl_->counters.maximumSliceNanoseconds = qMax(impl_->counters.maximumSliceNanoseconds,completion->elapsed);
  if (completion->progress.workItems >= work->lastWorkItems) {
    impl_->counters.workItems += completion->progress.workItems - work->lastWorkItems;
    work->lastWorkItems = completion->progress.workItems; work->progress = completion->progress;
  }
  work->posted = false;
  if (impl_->active != work) return;
  if (!impl_->current(work) || completion->step == DetailStep::Cancelled) { impl_->retry.start(0); return; }
  if (completion->step == DetailStep::More) { impl_->retry.start(0); return; }
  if (completion->step == DetailStep::NeedSummaries) {
    work->waitingSummaries = true;
    for (auto id : completion->needs) work->dependencies.insert(id);
    emit relatedSummariesNeeded(work->id,completion->needs); return;
  }
  impl_->active.reset(); impl_->retry.start(0);
  if (completion->step != DetailStep::Complete || !completion->result) {
    emit failed(work->consumer,work->id,completion->failure,completion->error); return;
  }
  auto value = std::move(completion->result); const quint64 bytes = preparedPetDetailRetainedBytes(*value);
  if (bytes > work->reservation->bytes.load()) {
    emit failed(work->consumer,work->id,DetailPreparationStatus::BudgetExceeded,QStringLiteral("prepared detail exceeded its reserved output size")); return;
  }
  auto retention = std::make_shared<Retention>(); retention->state = impl_->state; retention->facts = work->facts; retention->bytes = bytes;
  impl_->state->retained.fetch_add(bytes); impl_->state->liveLeases.fetch_add(1); work->reservation->release();
  value->chargedBytes = bytes; attachLease(*value,retention); impl_->state->updatePeak();
  PreparedPetDetailHandle handle(value.release()); auto& slot = impl_->consumers[work->consumer]; slot.result = handle; slot.dependencies = work->dependencies;
  ++impl_->counters.completedTasks;
  const QPointer<PetDetailPreparationService> alive(this); emit ready(work->consumer,work->id,handle); if (alive) emit stateChanged();
}
void PetDetailPreparationService::releasedBudget() { if (!impl_->state->closing.load()) impl_->retry.start(0); }
void PetDetailPreparationService::invalidateMetadata(quint64 revision, const QByteArray& digest) {
  if (impl_->metadataRevision == revision && impl_->metadataDigest == digest) return;
  impl_->metadataRevision = revision; impl_->metadataDigest = digest;
  for (int i = 0; i < kDetailConsumerCount; ++i) if (impl_->consumers[i].id) impl_->schedule(i);
}
void PetDetailPreparationService::summariesChanged(const QSet<qint64>& ids) {
  for (int i = 0; i < kDetailConsumerCount; ++i) {
    const auto& slot = impl_->consumers[i]; if (!slot.id) continue;
    bool affected = ids.contains(slot.selection.instanceId);
    for (auto id : ids) if (slot.dependencies.contains(id) || (impl_->active && impl_->active->consumer == i && impl_->active->dependencies.contains(id))) { affected = true; break; }
    if (affected) impl_->schedule(i);
  }
}
void PetDetailPreparationService::release(int consumer) {
  if (consumer < 0 || consumer >= kDetailConsumerCount) return;
  if (impl_->active && impl_->active->consumer == consumer) impl_->active->cancelled->store(true);
  impl_->consumers[consumer] = {}; impl_->retry.start(0); emit stateChanged();
}
void PetDetailPreparationService::bindSession(const QString& account, quint64 epoch) {
  if (impl_->account == account && impl_->epoch == epoch) return;
  impl_->account = account; impl_->epoch = epoch; impl_->metadataRevision = 0; impl_->metadataDigest.clear();
  if (impl_->active) impl_->active->cancelled->store(true);
  impl_->consumers = {}; impl_->retry.start(0); emit stateChanged();
}
bool PetDetailPreparationService::shutdown() {
  if (!impl_->state->closing.exchange(true)) {
    { QMutexLocker guard(&impl_->state->delivery); impl_->state->receiver = nullptr; }
    impl_->retry.stop(); if (impl_->active) impl_->active->cancelled->store(true); impl_->active.reset(); impl_->consumers = {};
  }
  return impl_->state->posted.load() == 0;
}
DetailServiceStats PetDetailPreparationService::stats() const {
  auto result = impl_->counters;
  result.activeTasks = impl_->state->posted.load(); result.retainedResultBytes = impl_->state->retained.load(); result.reservedResultBytes = impl_->state->reserved.load();
  result.peakRetainedBytes = impl_->state->peak.load(); result.liveResultLeases = impl_->state->liveLeases.load(); result.closing = impl_->state->closing.load();
  if (impl_->active) result.workingBytes = impl_->active->progress.retainedWorkingBytes;
  for (const auto& slot : impl_->consumers) { result.selectedConsumers += slot.id != 0; result.waitingDescriptors += slot.dirty; result.workingBytes += quint64(slot.dependencies.size()) * 32; }
  return result;
}
