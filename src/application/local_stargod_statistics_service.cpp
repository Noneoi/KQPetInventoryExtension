#include "local_stargod_statistics_service.h"
#include "../domain/local_stargod_count.h"
#include "../domain/checked_json_numbers.h"
#include "../storage/storage_service.h"
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QTimer>
#include <atomic>

namespace LocalStargodStatisticsInternal {
struct State {
  QMutex mutex;
  LocalStargodStatisticsService* receiver = nullptr;
  std::atomic_bool closing{false};
  void publish(quint64 generation, LocalStargodStatistics result) {
    QMutexLocker lock(&mutex);
    if (!receiver || closing.load()) return;
    auto* target = receiver;
    QMetaObject::invokeMethod(target,[target,generation,result = std::move(result)]() mutable {
      target->receive(generation,std::move(result));
    },Qt::QueuedConnection);
  }
};
namespace {
constexpr qint64 maximumDetailBytes = 16 * 1024 * 1024;
struct CountedPet { LocalStargodPetCounts counts; QDateTime observedAt; QString name; };

class Job final : public QObject {
public:
  Job(StorageContext context, quint64 epoch, QJsonObject definitions, quint64 generation,
      std::shared_ptr<State> state, std::shared_ptr<std::atomic_bool> cancelled, QObject* parent)
      : QObject(parent), context_(std::move(context)), definitions_(std::move(definitions)), generation_(generation),
        state_(std::move(state)), cancelled_(std::move(cancelled)) {
    result_.account = context_->account(); result_.epoch = epoch; result_.running = true;
    directory_ = QDir(context_->directory()).filePath(QStringLiteral("details"));
  }
  void start() {
    // Only direct files in this frozen account capability are in scope.
    const auto root = QDir::cleanPath(context_->dataRoot());
    auto ancestor = QDir::cleanPath(context_->directory());
    while (ancestor.compare(root,Qt::CaseInsensitive) != 0) {
      if (!ancestor.startsWith(root + QLatin1Char('/'),Qt::CaseInsensitive) || QFileInfo(ancestor).isSymLink()) {
        finish(QStringLiteral("本地详情目录不能经过链接或超出缓存目录")); return;
      }
      ancestor = QFileInfo(ancestor).absolutePath();
    }
    for (const auto& path : {context_->directory(),directory_}) {
      const QFileInfo info(path);
      if (info.isSymLink() || (info.exists() && (!info.isDir() || !info.isReadable()))) {
        finish(QStringLiteral("本地详情目录不是可读取的普通目录")); return;
      }
    }
    iterator_ = std::make_unique<QDirIterator>(directory_,QStringList{QStringLiteral("*.json")},QDir::Files | QDir::Hidden,QDirIterator::NoIteratorFlags);
    progressClock_.start();
    QTimer::singleShot(0,this,[this] { step(); });
  }
private:
  void add(const LocalStargodPetCounts& count, int sign) {
    result_.ordinaryEquipped += sign * count.ordinaryEquipped;
    result_.ordinaryBackpack += sign * count.ordinaryBackpack;
    result_.changeableEquipped += sign * count.changeableEquipped;
    result_.changeableBackpack += sign * count.changeableBackpack;
    result_.unknownEntries += sign * count.unknownEntries;
    if (!count.complete()) result_.incompletePets += sign;
  }
  void read(const QString& path) {
    ++result_.scannedFiles;
    const QFileInfo fileInfo(path);
    if (fileInfo.isSymLink() || fileInfo.size() <= 0 || fileInfo.size() > maximumDetailBytes) {
      ++result_.skippedFiles; return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { ++result_.skippedFiles; return; }
    const auto bytes = file.read(maximumDetailBytes + 1);
    if (bytes.size() > maximumDetailBytes || file.error() != QFileDevice::NoError) { ++result_.skippedFiles; return; }
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(bytes,&parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) { ++result_.skippedFiles; return; }
    const auto envelope = doc.object(); const auto pet = envelope.value(QStringLiteral("pet")).toObject();
    qint64 schema = 0,id = 0,envelopeId = 0;
    if (!DomainNumeric::checkedInteger(envelope.value(QStringLiteral("schema")),&schema,2,4) ||
        envelope.value(QStringLiteral("account")).toString() != result_.account ||
        !DomainNumeric::checkedInteger(pet.value(QStringLiteral("id")),&id,1) ||
        (schema == 4 && envelope.value(QStringLiteral("trust")).toString() != QStringLiteral("verified-observation") &&
         envelope.value(QStringLiteral("trust")).toString() != QStringLiteral("read-only-observation")) ||
        (envelope.contains(QStringLiteral("complete")) && (!envelope.value(QStringLiteral("complete")).isBool() ||
         !envelope.value(QStringLiteral("complete")).toBool())) ||
        (envelope.contains(QStringLiteral("instanceId")) &&
         (!DomainNumeric::checkedInteger(envelope.value(QStringLiteral("instanceId")),&envelopeId,1) || envelopeId != id))) {
      ++result_.skippedFiles; return;
    }
    auto observedAt = QDateTime::fromString(envelope.value(QStringLiteral("observedAt")).toString(),Qt::ISODateWithMs);
    if (!observedAt.isValid()) observedAt = QDateTime::fromString(envelope.value(QStringLiteral("savedAt")).toString(),Qt::ISODateWithMs);
    if (!observedAt.isValid()) observedAt = fileInfo.lastModified();
    auto previous = pets_.find(id);
    if (previous != pets_.end()) {
      ++result_.duplicatePets;
      if (previous->observedAt > observedAt || (previous->observedAt == observedAt && previous->name <= fileInfo.fileName())) return;
      add(previous->counts,-1);
    } else ++result_.countedPets;
    const auto counts = countLocalStargods(pet,definitions_);
    add(counts,1);
    pets_.insert(id,{counts,observedAt,fileInfo.fileName()});
  }
  void step() {
    if (state_->closing.load() || cancelled_->load()) { result_.cancelled = true; finish(); return; }
    QElapsedTimer slice; slice.start(); int files = 0;
    while (iterator_->hasNext() && files++ < 8 && slice.elapsed() < 8) {
      read(iterator_->next());
      if (cancelled_->load()) break;
    }
    if (!iterator_->hasNext()) { finish(); return; }
    if (progressClock_.elapsed() >= 100) { state_->publish(generation_,result_); progressClock_.restart(); }
    QTimer::singleShot(0,this,[this] { step(); });
  }
  void finish(QString error = {}) {
    result_.running = false; result_.error = std::move(error);
    result_.completed = result_.error.isEmpty() && !result_.cancelled;
    result_.finishedAt = QDateTime::currentDateTimeUtc();
    state_->publish(generation_,std::move(result_));
    deleteLater();
  }
  StorageContext context_;
  QJsonObject definitions_;
  quint64 generation_;
  std::shared_ptr<State> state_;
  std::shared_ptr<std::atomic_bool> cancelled_;
  LocalStargodStatistics result_;
  QString directory_;
  std::unique_ptr<QDirIterator> iterator_;
  QHash<qint64,CountedPet> pets_;
  QElapsedTimer progressClock_;
};
}
}

struct LocalStargodStatisticsService::Impl {
  QPointer<StorageService> storage;
  std::shared_ptr<LocalStargodStatisticsInternal::State> state = std::make_shared<LocalStargodStatisticsInternal::State>();
  std::shared_ptr<std::atomic_bool> cancelled;
  quint64 generation = 0;
  QString account;
  quint64 epoch = 0;
  bool busy = false;
};
LocalStargodStatisticsService::LocalStargodStatisticsService(StorageService* storage, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
  impl_->storage = storage; impl_->state->receiver = this;
  qRegisterMetaType<LocalStargodStatistics>();
}
LocalStargodStatisticsService::~LocalStargodStatisticsService() { close(); }
bool LocalStargodStatisticsService::busy() const { return impl_->busy; }
bool LocalStargodStatisticsService::request(StorageContext context, quint64 epoch, QJsonObject stargods) {
  if (impl_->state->closing.load() || !impl_->storage || !context || context->isShared() || context->account().isEmpty()) return false;
  if (impl_->busy && impl_->account == context->account() && impl_->epoch == epoch &&
      impl_->cancelled && !impl_->cancelled->load()) return false;
  const auto generation = impl_->generation + 1;
  auto cancelled = std::make_shared<std::atomic_bool>(false);
  auto state = impl_->state;
  const auto account = context->account();
  if (!impl_->storage->postAuxiliary([context = std::move(context),epoch,stargods = std::move(stargods),generation,state,cancelled](QObject* io) mutable {
    if (state->closing.load()) return;
    (new LocalStargodStatisticsInternal::Job(std::move(context),epoch,std::move(stargods),generation,state,cancelled,io))->start();
  })) return false;
  if (impl_->cancelled) impl_->cancelled->store(true);
  impl_->generation = generation;
  impl_->account = account; impl_->epoch = epoch;
  impl_->cancelled = std::move(cancelled); impl_->busy = true;
  LocalStargodStatistics result; result.account = account; result.epoch = epoch; result.running = true;
  emit updated(result); return true;
}
void LocalStargodStatisticsService::cancel() { if (impl_->cancelled) impl_->cancelled->store(true); }
void LocalStargodStatisticsService::close() {
  impl_->state->closing.store(true); cancel();
  QMutexLocker lock(&impl_->state->mutex); impl_->state->receiver = nullptr; impl_->busy = false;
}
void LocalStargodStatisticsService::receive(quint64 generation, LocalStargodStatistics result) {
  if (generation != impl_->generation || impl_->state->closing.load()) return;
  if (!result.running) { impl_->busy = false; impl_->cancelled.reset(); }
  emit updated(result);
}
