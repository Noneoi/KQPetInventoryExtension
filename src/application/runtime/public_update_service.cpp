#include "public_update_service.h"
#include "storage/storage_service.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <atomic>

namespace {
const QStringList Keys{QStringLiteral("pets"), QStringLiteral("skills"), QStringLiteral("shop"),
    QStringLiteral("images"), QStringLiteral("icons"), QStringLiteral("routines"), QStringLiteral("battleReplay")};
QString label(const QString& key) {
  static const QHash<QString, QString> labels{{"pets", "精灵与养成"}, {"skills", "技能与效果"},
      {"shop", "兑换活动"}, {"images", "精灵图片"}, {"icons", "星神与属性图标"},
      {"routines", "日常与活动"}, {"battleReplay", "战斗回放资料"}};
  return labels.value(key, QStringLiteral("公共资料"));
}
bool identity(const QString& text, int size) {
  static const QRegularExpression hex(QStringLiteral("^[a-f0-9]+$"));
  return text.size() == size && hex.match(text).hasMatch();
}
QString hash(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
QByteArray bytes(const QString& root, const QString& relative, QString* error, bool required = false) {
  const auto path = QDir(root).filePath(relative);
  const auto base = QDir::fromNativeSeparators(QFileInfo(root).canonicalFilePath());
  QFileInfo info(path);
  if (!info.exists()) {
    if (required) *error = QStringLiteral("公共资料记录缺失");
    return {};
  }
  if (base.isEmpty() || !QDir::fromNativeSeparators(info.canonicalFilePath()).startsWith(base + '/', Qt::CaseInsensitive) ||
      !info.isFile() || info.size() <= 0 || info.size() > 16 * 1024 * 1024) {
    *error = QStringLiteral("公共资料记录路径或大小无效"); return {};
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) { *error = QStringLiteral("无法读取公共资料记录"); return {}; }
  return file.readAll();
}
QJsonObject object(const QByteArray& raw, QString* error) {
  if (raw.isEmpty()) return {};
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(raw, &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject()) {
    *error = QStringLiteral("公共资料记录格式无效"); return {};
  }
  return document.object();
}
QJsonObject read(const QString& root, const QString& relative, QString* error, bool required = false) {
  return object(bytes(root, relative, error, required), error);
}
QStringList strings(const QJsonValue& value) {
  QStringList result;
  for (const auto& item : value.toArray()) if (item.isString() && Keys.contains(item.toString()) && !result.contains(item.toString())) result.append(item.toString());
  return result;
}
QString receiptTime(const QJsonObject& receipt) {
  return receipt.value("checkedUtc").toString(receipt.value("createdUtc").toString(receipt.value("recordedUtc").toString()));
}
QDateTime orderTime(const QString& root, const QJsonObject& receipt, QString* error) {
  const auto id = receipt.value("attempt").toString();
  const auto request = identity(id, 32) ? read(root, QStringLiteral("public/transactions/%1/request.json").arg(id), error) : QJsonObject{};
  const auto value = request.value("attempt") == id ? request.value("createdUtc") : receipt.value("createdUtc");
  return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
}
QJsonValue redact(QJsonValue value) {
  if (value.isObject()) {
    auto object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) it.value() = redact(it.value());
    return object;
  }
  if (value.isArray()) {
    QJsonArray array;
    for (const auto& item : value.toArray()) array.append(redact(item));
    return array;
  }
  if (value.isString()) {
    auto text = value.toString();
    static const QRegularExpression localPath(QStringLiteral("(?<![A-Za-z0-9_])(?:[A-Za-z]:[\\\\/]|\\\\\\\\)[^\\r\\n\"<>]*"));
    text.replace(localPath, QStringLiteral("[本地路径已隐藏]"));
    if (text.startsWith('/') && !text.startsWith("//")) text = QStringLiteral("[本地路径已隐藏]");
    return text;
  }
  return value;
}
QJsonObject latestReceipt(const QString& root, QString* error) {
  auto latest = read(root, QStringLiteral("public/reports/latest.json"), error);
  const auto request = read(root, QStringLiteral("public/reports/latest-request.json"), error);
  const auto id = request.value("attempt").toString();
  if (identity(id, 32)) {
    const auto own = read(root, QStringLiteral("public/transactions/%1/receipt.json").arg(id), error);
    if (own.value("attempt") == id) {
      const auto previousTime = orderTime(root, latest, error), requestTime = orderTime(root, own, error);
      // Old receipts without an immutable request time can have a fresh
      // recordedUtc solely because another process acknowledged them.
      if (latest.isEmpty() || latest.value("kind") == QJsonValue("migration") || !previousTime.isValid() ||
          (requestTime.isValid() && requestTime >= previousTime)) latest = own;
    }
  }
  return latest;
}
}

PublicUpdateState PublicUpdateService::readState(const QString& root, const QString& sessionRevision) {
  PublicUpdateState state;
  state.sessionRevision = sessionRevision;
  QString error;
  const auto current = read(root, QStringLiteral("public/current.json"), &error);
  if (!current.isEmpty()) {
    const auto revision = current.value("revision").toString();
    if (!identity(revision, 64) || current.value("schema") != QJsonValue(1)) error = QStringLiteral("当前公共资料版本记录无效");
    else {
      const auto raw = bytes(root, QStringLiteral("public/releases/%1/manifest.json").arg(revision), &error, true);
      const auto manifest = object(raw, &error);
      const auto parts = manifest.value("components").toObject();
      bool complete = parts.size() == Keys.size();
      for (const auto& key : Keys) complete &= parts.value(key).isObject();
      if (hash(raw) != revision || !complete || manifest.value("schema") != QJsonValue(1)) error = QStringLiteral("当前公共资料清单校验不通过");
      else {
        state.diskRevision = revision;
        state.previousRevision = current.value("previousRevision").toString();
        state.canRestorePrevious = identity(state.previousRevision, 64) && state.previousRevision != revision;
        if (state.canRestorePrevious) {
          QString previousError;
          const auto previous = bytes(root, QStringLiteral("public/releases/%1/manifest.json").arg(state.previousRevision), &previousError);
          state.canRestorePrevious = !previous.isEmpty() && previousError.isEmpty() && hash(previous) == state.previousRevision;
        }
        state.sessionApplied = !sessionRevision.isEmpty() && sessionRevision == revision;
      }
    }
  }
  const auto receipt = latestReceipt(root, &error);
  const auto attempt = receipt.value("attempt").toString();
  QJsonObject candidate, validation;
  if (!receipt.isEmpty() && !identity(attempt, 32)) error = QStringLiteral("最近检查记录的事务身份无效");
  if (identity(attempt, 32)) {
    state.latestAttempt = attempt;
    state.checkedUtc = receiptTime(receipt);
    candidate = read(root, QStringLiteral("public/transactions/%1/candidate.json").arg(attempt), &error);
    if (candidate.value("attempt").toString() != attempt) candidate = {};
    const auto request = read(root, QStringLiteral("public/transactions/%1/request.json").arg(attempt), &error);
    state.requestedComponents = strings(candidate.value("requestedComponents"));
    if (state.requestedComponents.isEmpty()) state.requestedComponents = strings(request.value("requestedComponents"));
    state.effectiveComponents = strings(candidate.value("effectiveComponents"));
    if (state.effectiveComponents.isEmpty()) state.effectiveComponents = strings(receipt.value("effectiveComponents"));
    if (!candidate.isEmpty()) {
      const auto raw = bytes(root, QStringLiteral("public/transactions/%1/validation.json").arg(attempt), &error);
      validation = object(raw, &error);
      const auto binding = candidate.value("validation").toObject();
      if (hash(raw) == binding.value("reportSha256").toString() && validation.value("attempt") == attempt &&
          validation.value("revision") == candidate.value("revision") && validation.value("passed").toBool())
        state.candidatePassed = receipt.value("candidatePassed").toBool();
      else { validation = {}; error = QStringLiteral("候选检查记录校验不通过，需重新检查"); }
    }
    const auto target = receipt.value("revision").toString(candidate.value("revision").toString());
    const bool sameDisk = target == state.diskRevision && (current.value("attempt") == attempt ||
        (!receipt.value("plannedGeneration").toString().isEmpty() && receipt.value("plannedGeneration") == current.value("generation")));
    state.diskCommitted = receipt.value("diskCommitted").toBool() && sameDisk;
    if (state.diskCommitted) state.candidatePassed = true;
    auto results = receipt.value("results").toObject();
    if (results.isEmpty()) results = candidate.value("results").toObject();
    bool problems = false, failed = false, changed = false;
    for (const auto& key : Keys) {
      const auto row = results.value(key).toObject();
      PublicUpdateComponentState part;
      part.key = key; part.label = label(key); part.status = row.value("status").toString(QStringLiteral("notChecked"));
      part.hasWarnings = !row.value("error").toString().isEmpty();
      part.retryable = row.value("retryable").toBool() || part.status == "failed" || part.status == "retained";
      if (part.retryable) state.retryComponents.append(key);
      if (part.status == "retained" || part.status == "failed") { part.message = QStringLiteral("沿用已有资料，可重试"); failed = true; }
      else if (part.hasWarnings) part.message = part.retryable ? QStringLiteral("部分内容未取得，可重试补齐") : QStringLiteral("资料已核对，部分官方内容仍待适配");
      else if (part.status == "updated") part.message = QStringLiteral("资料内容已更新");
      else if (part.status == "unchanged") part.message = QStringLiteral("内容未变化");
      else part.message = QStringLiteral("本次未检查");
      changed |= part.status == "updated";
      problems |= part.retryable || part.hasWarnings;
      part.technical = row;
      state.components.append(part);
    }
    const auto status = receipt.value("state").toString();
    const auto kind = receipt.value("kind").toString();
    if (status == "cancelled" && !state.diskCommitted) { state.outcome = PublicUpdateOutcome::Cancelled; state.summary = QStringLiteral("检查已取消，继续使用原有资料"); }
    else if (state.diskCommitted) {
      if (!state.sessionApplied) { state.outcome = PublicUpdateOutcome::PendingApply; state.summary = QStringLiteral("资料已保存，当前会话尚未加载该版本"); }
      else if (kind == "rollback" || kind == "restore") { state.outcome = PublicUpdateOutcome::Restored; state.summary = QStringLiteral("已恢复资料版本，整个工作台使用同一份资料"); }
      else if (problems) { state.outcome = PublicUpdateOutcome::Partial; state.summary = QStringLiteral("已核对的资料已应用；部分项目有提示，具体影响和处理方式见结果详情"); }
      else if (!candidate.isEmpty() && !changed && candidate.value("baseRevision") == candidate.value("revision")) { state.outcome = PublicUpdateOutcome::Unchanged; state.summary = QStringLiteral("检查完成，资料内容未变化"); }
      else { state.outcome = PublicUpdateOutcome::Applied; state.summary = QStringLiteral("资料已应用到当前工作台"); }
    } else if (state.candidatePassed && receipt.value("error").toString().isEmpty() && error.isEmpty()) {
      state.outcome = PublicUpdateOutcome::PendingApply; state.summary = QStringLiteral("候选资料已检查，尚未切换现用版本");
    } else {
      state.outcome = PublicUpdateOutcome::KeptPrevious;
      state.summary = failed || !receipt.value("error").toString().isEmpty() ? QStringLiteral("检查未完成，原有资料已保留") : QStringLiteral("上次检查尚未完成，现用资料仍然可用");
      if (state.retryComponents.isEmpty()) state.retryComponents = state.requestedComponents;
    }
    state.technical.insert("receipt", receipt);
    if (!candidate.isEmpty() && !validation.isEmpty()) {
        const auto change = validation.value("changeReport").toObject();
        if (!change.isEmpty()) {
          const auto file = change.value("file").toString();
          if (file != "changes.json") error = QStringLiteral("变更说明路径无效");
          else {
            const auto changeBytes = bytes(root, QStringLiteral("public/transactions/%1/changes.json").arg(attempt), &error, true);
            const auto changes = object(changeBytes, &error);
            if (hash(changeBytes) != change.value("sha256").toString() || changeBytes.size() != change.value("bytes").toInteger() ||
                changes.value("attempt") != attempt || changes.value("revision") != candidate.value("revision")) error = QStringLiteral("变更说明与实际候选不匹配");
            else {
              state.changesRecorded = true;
              state.technical.insert("changeSummary", QJsonObject{{"technicalOnly", changes.value("technicalOnly")},
                  {"technicalChanges", changes.value("technicalChanges")}});
              state.totalChanges = changes.value("total").toInteger(); state.changesTruncated = changes.value("truncated").toBool();
              for (const auto& value : changes.value("rows").toArray()) {
                const auto row = value.toObject(); PublicUpdateChange item;
                item.component = row.value("component").toString(); item.category = row.value("category").toString();
                item.kind = row.value("kind").toString(); item.name = row.value("name").toString();
                item.beforeName = row.value("beforeName").toString(); item.afterName = row.value("afterName").toString();
                item.beforeText = row.value("beforeText").toString(); item.afterText = row.value("afterText").toString();
                item.technical = row.value("technical").toObject(); state.changes.append(item);
              }
            }
          }
        }
    }
  } else {
    state.summary = state.diskRevision.isEmpty() ? QStringLiteral("尚未检查公共资料") : QStringLiteral("已有公共资料版本，尚无本次检查记录");
    if (!state.diskRevision.isEmpty() && !state.sessionApplied) state.outcome = PublicUpdateOutcome::PendingApply;
  }
  state.readable = error.isEmpty(); state.error = error;
  if (!error.isEmpty() && !state.diskCommitted) { state.candidatePassed = false; state.outcome = PublicUpdateOutcome::KeptPrevious; }
  return state;
}

QJsonObject PublicUpdateService::publicDiagnostics(const QString& root, const QString& sessionRevision) {
  QString error;
  const auto current = read(root, QStringLiteral("public/current.json"), &error);
  const auto receipt = latestReceipt(root, &error);
  QJsonObject manifest;
  const auto revision = current.value("revision").toString();
  if (identity(revision, 64)) manifest = read(root, QStringLiteral("public/releases/%1/manifest.json").arg(revision), &error);
  const auto projection = readState(root, sessionRevision);
  QJsonArray components, changes;
  for (const auto& item : projection.components) components.append(QJsonObject{{"component", item.key}, {"status", item.status},
      {"message", item.message}, {"retryable", item.retryable}, {"technical", item.technical}});
  for (const auto& item : projection.changes) changes.append(QJsonObject{{"component", item.component}, {"category", item.category},
      {"kind", item.kind}, {"name", item.name}, {"beforeName", item.beforeName}, {"afterName", item.afterName},
      {"beforeText", item.beforeText}, {"afterText", item.afterText}, {"technical", item.technical}});
  return redact(QJsonObject{{"schema", 1}, {"scope", "public-dataset-only; no account files or battle logs"},
      {"current", current}, {"manifest", manifest}, {"latestReceipt", receipt},
      {"components", components}, {"changeSummary", QJsonObject{{"recorded", projection.changesRecorded},
        {"total", projection.totalChanges}, {"truncated", projection.changesTruncated}, {"rows", changes}}},
      {"technical", projection.technical}, {"currentProcessSessionRevision", sessionRevision},
      {"readError", error.isEmpty() ? projection.error : error}}).toObject();
}

struct PublicUpdateService::Impl {
  QPointer<StorageService> storage;
  PublicUpdateState state;
  std::shared_ptr<std::atomic_bool> closed = std::make_shared<std::atomic_bool>(false);
  quint64 request = 0;
  bool refreshInFlight = false;
  bool refreshDirty = false;
  bool refreshRetryPending = false;
};
PublicUpdateService::PublicUpdateService(StorageService* storage, QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>()) {
  impl_->storage = storage;
  qRegisterMetaType<PublicUpdateState>(); qRegisterMetaType<PublicUpdateProgress>();
}
PublicUpdateService::~PublicUpdateService() { close(); }
PublicUpdateState PublicUpdateService::state() const { return impl_->state; }
bool PublicUpdateService::refresh() {
  if (!impl_->storage || impl_->closed->load()) return false;
  if (impl_->refreshInFlight) { impl_->refreshDirty = true; return true; }
  impl_->refreshDirty = false;
  impl_->refreshInFlight = true;
  const auto root = impl_->storage->dataRoot(), revision = impl_->state.sessionRevision;
  const auto request = ++impl_->request; QPointer<PublicUpdateService> self(this); const auto closed = impl_->closed;
  const bool admitted = impl_->storage->postAuxiliary([self, closed, root, revision, request](QObject*) {
    if (closed->load()) return;
    const auto state = readState(root, revision);
    if (self) QMetaObject::invokeMethod(self, [self, state, request] { if (self) self->acceptState(state, request); }, Qt::QueuedConnection);
  });
  if (!admitted) {
    impl_->refreshInFlight = false; impl_->refreshDirty = true;
    if (!impl_->refreshRetryPending) {
      impl_->refreshRetryPending = true;
      QTimer::singleShot(250, this, [this] {
        impl_->refreshRetryPending = false;
        if (!impl_->closed->load() && impl_->refreshDirty && !impl_->refreshInFlight) refresh();
      });
    }
  }
  return admitted;
}
void PublicUpdateService::acceptState(PublicUpdateState state, quint64 request) {
  if (impl_->closed->load() || request != impl_->request) return;
  impl_->refreshInFlight = false;
  const bool dirty = impl_->refreshDirty;
  impl_->refreshDirty = false;
  if (state.sessionRevision != impl_->state.sessionRevision) { refresh(); return; }
  state.pictureCompleted = impl_->state.pictureCompleted; state.pictureTotal = impl_->state.pictureTotal;
  if (impl_->state.busy) { state.busy = true; state.canCancel = impl_->state.canCancel; state.progress = impl_->state.progress;
    state.summary = impl_->state.summary; state.outcome = PublicUpdateOutcome::Checking; }
  impl_->state = std::move(state); emit stateChanged(impl_->state);
  if (dirty) refresh();
}
void PublicUpdateService::setSessionRevision(const QString& revision) {
  if (impl_->state.sessionRevision == revision) return;
  impl_->state.sessionRevision = revision; refresh();
}
void PublicUpdateService::setBusy(bool busy, bool canCancel) {
  impl_->state.busy = busy; impl_->state.canCancel = busy && canCancel;
  if (busy) {
    impl_->state.outcome = PublicUpdateOutcome::Checking;
    impl_->state.summary = QStringLiteral("正在处理公共资料，当前工作台继续使用已加载的版本");
  }
  emit stateChanged(impl_->state); if (!busy) refresh();
}
void PublicUpdateService::observeProgress(const PublicUpdateProgress& progress) {
  impl_->state.progress = progress;
  if (progress.phase == PublicUpdatePhase::Applying) impl_->state.canCancel = false;
  if ((progress.component == "images" || progress.component == "icons") && progress.total >= 0) {
    impl_->state.pictureCompleted = progress.completed; impl_->state.pictureTotal = progress.total;
  }
  emit stateChanged(impl_->state);
}
void PublicUpdateService::observePictures(qint64 completed, qint64 total) {
  if (total < 0 || completed < 0 || completed > total) return;
  impl_->state.pictureCompleted = completed; impl_->state.pictureTotal = total;
  emit stateChanged(impl_->state);
}
bool PublicUpdateService::exportDiagnostics(const QString& destination) {
  if (!impl_->storage || impl_->closed->load() || destination.isEmpty()) return false;
  const auto root = impl_->storage->dataRoot(), revision = impl_->state.sessionRevision;
  QPointer<PublicUpdateService> self(this); const auto closed = impl_->closed;
  return impl_->storage->postAuxiliary([self, closed, root, revision, destination](QObject*) {
    if (closed->load()) return;
    const auto data = QJsonDocument(publicDiagnostics(root, revision)).toJson(QJsonDocument::Indented);
    QSaveFile file(destination);
    const bool ok = file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
    if (self) QMetaObject::invokeMethod(self, [self, ok, destination] {
      if (self) emit self->diagnosticsExported(ok, destination, ok ? QStringLiteral("公共资料诊断已导出") : QStringLiteral("无法保存诊断文件"));
    }, Qt::QueuedConnection);
  });
}
void PublicUpdateService::close() { impl_->closed->store(true); ++impl_->request; }
