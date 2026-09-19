#include "diagnostic_logger.h"
#include "storage/storage_service.h"
#include "target_compatibility_guard.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <windows.h>
#include <bcrypt.h>
#include <deque>
#include <cwctype>
#include <memory>
#include <mutex>
#include <regex>

namespace {
struct Event {
  std::wstring level, code, module, stage, message, action, time;
  quint64 task = 0;
  qint64 bytes() const {
    return sizeof(Event) + static_cast<qint64>(level.capacity() + code.capacity() + module.capacity() +
        stage.capacity() + message.capacity() + action.capacity() + time.capacity()) * sizeof(wchar_t);
  }
};
struct Memory {
  std::mutex mutex;
  std::deque<Event> pending, recent;
  DiagnosticLimits limits;
  qint64 pendingBytes = 0, recentBytes = 0;
  int inflightEvents = 0, outstanding = 0;
  quint64 saved = 0, dropped = 0, failed = 0, admissionFailures = 0;
  bool attached = false, closing = false;
  std::wstring lastCode, report, run;
  std::string salt;
};
// Retained until process exit, including when a final disk write outlives Core.
Memory& memory() { static Memory* value = new Memory; return *value; }
std::wstring now() {
  SYSTEMTIME t{}; GetSystemTime(&t);
  wchar_t text[40]{};
  swprintf_s(text, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay,
      t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  return text;
}
std::wstring token(std::wstring value, const wchar_t* fallback) {
  if (value.empty() || value.size() > 80) return fallback;
  for (wchar_t c : value)
    if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
          (c >= L'0' && c <= L'9') || c == L'_' || c == L'-' || c == L'.')) return fallback;
  return value;
}
std::wstring redact(const std::wstring& source, size_t maximum = 1024, bool omitPayload = true) {
  std::wstring value = source.substr(0, maximum);
  if (source.size() > maximum) value += L" <truncated>";
  if (omitPayload && (value.find(L'{') != std::wstring::npos || value.find(L'[') != std::wstring::npos))
    return L"<structured-payload-omitted>";
  for (wchar_t& c : value) if (c < 32) c = L' ';
  static const std::wregex sensitive(
      LR"((account|username|user_id|uin|token|secret|password|passwd|api[_-]?key|cookie|authorization|raw[_-]?(detail|packet|payload))\s*[:=]\s*("[^"]*"|'[^']*'|[^\s,;]+))",
      std::regex_constants::icase);
  static const std::wregex windowsPath(LR"(([A-Za-z]:[\\/]|\\\\)[^\r\n<>"|?*;]+)");
  static const std::wregex homePath(LR"(/(home|Users)/[^\s;]+)");
  static const std::wregex longId(LR"(\b[0-9]{6,}\b)");
  std::wstring filtered;
  size_t cursor = 0;
  static const std::wregex correlation(LR"(account-[0-9a-f]{24})");
  for (std::wsregex_iterator at(value.begin(), value.end(), sensitive), end; at != end; ++at) {
    filtered.append(value, cursor, at->position() - cursor);
    std::wstring key = (*at)[1].str();
    for (wchar_t& c : key) c = static_cast<wchar_t>(std::towlower(c));
    const auto field = (*at)[3].str();
    filtered += (*at)[1].str() + L"=" + (key == L"account" && std::regex_match(field, correlation) ? field : L"<redacted>");
    cursor = at->position() + at->length();
  }
  filtered.append(value, cursor, std::wstring::npos);
  value = std::move(filtered);
  value = std::regex_replace(value, windowsPath, L"<personal-path>");
  value = std::regex_replace(value, homePath, L"<personal-path>");
  return std::regex_replace(value, longId, L"<numeric-id>");
}
void enqueue(Event value) {
  auto& m = memory(); value.time = now();
  // Remove spare string capacity before charging/copying bounded events.
  for (auto* s : {&value.level, &value.code, &value.module, &value.stage, &value.message, &value.action, &value.time}) s->shrink_to_fit();
  const qint64 bytes = value.bytes();
  std::lock_guard<std::mutex> guard(m.mutex);
  while (!m.recent.empty() && (m.recent.size() >= static_cast<size_t>(m.limits.recentEvents) || m.recentBytes > m.limits.recentBytes - bytes)) {
    m.recentBytes -= m.recent.front().bytes(); m.recent.pop_front();
  }
  if (bytes <= m.limits.recentBytes) { m.recent.push_back(value); m.recentBytes += m.recent.back().bytes(); }
  if (m.closing || m.pending.size() + m.inflightEvents >= static_cast<size_t>(m.limits.pendingEvents) || bytes > m.limits.pendingBytes - m.pendingBytes) {
    ++m.dropped; m.lastCode = m.closing ? L"log_closed" : L"log_queue_capacity"; return;
  }
  m.pending.push_back(std::move(value)); m.pendingBytes += m.pending.back().bytes();
}
QString q(const std::wstring& value) { return QString::fromStdWString(value); }
QByteArray encode(const Event& e) {
  return QJsonDocument(QJsonObject{{QStringLiteral("level"), q(e.level)}, {QStringLiteral("code"), q(e.code)},
      {QStringLiteral("module"), q(e.module)}, {QStringLiteral("taskId"), QString::number(e.task)},
      {QStringLiteral("stage"), q(e.stage)}, {QStringLiteral("observedAt"), q(e.time)},
      {QStringLiteral("message"), q(e.message)}, {QStringLiteral("suggestedAction"), q(e.action)}}).toJson(QJsonDocument::Compact) + '\n';
}
struct IoState {
  DiagnosticStore* store = nullptr; // IO only, owned by IO root's child.
  QString root, run;
  DiagnosticLimits limits;
};
class StoreOwner final : public QObject {
public:
  StoreOwner(std::shared_ptr<IoState> state, QObject* parent) : QObject(parent), state_(std::move(state)),
      store_(state_->root, state_->run, state_->limits) { state_->store = &store_; }
  ~StoreOwner() override { state_->store = nullptr; }
private:
  std::shared_ptr<IoState> state_;
  DiagnosticStore store_;
};
void consume(const std::shared_ptr<IoState>& io, QObject* root, bool final) {
  auto& m = memory();
  try {
    if (!io->store) new StoreOwner(io, root);
    const auto opened = io->store->open();
    {
      std::lock_guard<std::mutex> guard(m.mutex);
      if (opened.saved) { const auto salt = io->store->installationSalt(); m.salt.assign(salt.constData(), salt.size()); }
      if (!opened.code.isEmpty()) m.lastCode = opened.code.toStdWString();
    }
    qint64 consumed = 0;
    do {
      std::deque<Event> batch;
      qint64 reserved = 0;
      {
        std::lock_guard<std::mutex> guard(m.mutex);
        while (!m.pending.empty()) {
          const qint64 size = m.pending.front().bytes();
          if (!batch.empty() && reserved + size > io->limits.batchBytes) break;
          reserved += size; batch.push_back(std::move(m.pending.front())); m.pending.pop_front();
        }
        m.inflightEvents += static_cast<int>(batch.size());
      }
      if (batch.empty()) break;
      QByteArray bytes;
      int saved = 0, failed = 0;
      QString code;
      auto flush = [&] {
        if (bytes.isEmpty()) return;
        const auto written = opened.saved ? io->store->append(bytes) : opened;
        if (written.saved) saved += bytes.count('\n'); else failed += bytes.count('\n');
        if (!written.code.isEmpty()) code = written.code;
        bytes.clear();
      };
      for (const auto& event : batch) {
        const auto encoded = encode(event);
        if (encoded.size() > io->limits.fileBytes) { ++failed; code = QStringLiteral("log_event_capacity"); continue; }
        if (!bytes.isEmpty() && bytes.size() + encoded.size() > io->limits.batchBytes) flush();
        bytes += encoded;
      }
      flush();
      {
        std::lock_guard<std::mutex> guard(m.mutex);
        m.pendingBytes -= reserved; m.inflightEvents -= static_cast<int>(batch.size());
        m.saved += saved; m.failed += failed;
        if (!code.isEmpty()) m.lastCode = code.toStdWString();
      }
      consumed += reserved;
      // Routine IO yields after one batch. Final IO has only the bounded
      // pre-closing queue to drain; no new event can extend its work.
    } while (final && consumed <= io->limits.pendingBytes);
    if (final) io->store->close();
  } catch (...) {
    std::lock_guard<std::mutex> guard(m.mutex);
    m.failed += std::max(1, m.inflightEvents); m.lastCode = L"log_io_exception";
    m.pendingBytes = 0; for (const auto& e : m.pending) m.pendingBytes += e.bytes();
    m.inflightEvents = 0;
  }
  std::lock_guard<std::mutex> guard(m.mutex); --m.outstanding;
}
class Pump final : public QObject {
public:
  Pump(StorageService* storage, QObject* owner, DiagnosticLimits limits) : QObject(owner), storage_(storage), io_(std::make_shared<IoState>()) {
    io_->root = storage->dataRoot(); io_->limits = limits;
    unsigned char random[16]{};
    if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
      auto& m = memory(); std::lock_guard<std::mutex> guard(m.mutex); m.lastCode = L"log_entropy_unavailable"; return;
    }
    io_->run = QStringLiteral("run-%1-%2").arg(GetCurrentProcessId()).arg(QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(random), sizeof(random)).toHex()));
    { auto& m = memory(); std::lock_guard<std::mutex> guard(m.mutex); m.run = io_->run.toStdWString(); m.attached = true; }
    timer_ = new QTimer(this); timer_->setInterval(limits.flushMilliseconds);
    connect(timer_, &QTimer::timeout, this, [this] { flush(false); }); timer_->start(); flush(false);
  }
  bool valid() const { return timer_ != nullptr; }
  StorageService* storage() const { return storage_; }
  bool close() {
    if (closed_) return finalAdmitted_;
    closed_ = true; if (timer_) timer_->stop();
    { auto& m = memory(); std::lock_guard<std::mutex> guard(m.mutex); m.closing = true; }
    return finalAdmitted_ = flush(true);
  }
private:
  bool flush(bool final) {
    if (!storage_) return false;
    auto& m = memory();
    { std::lock_guard<std::mutex> guard(m.mutex);
      if (!final && (m.outstanding || (started_ && m.pending.empty()))) return true;
      ++m.outstanding;
    }
    if (!storage_->postAuxiliary([io = io_, final](QObject* root) { consume(io, root, final); })) {
      std::lock_guard<std::mutex> guard(m.mutex); --m.outstanding; ++m.admissionFailures;
      m.lastCode = final ? L"log_close_not_queued" : L"log_storage_queue_busy"; return false;
    }
    started_ = true; return true;
  }
  QPointer<StorageService> storage_;
  std::shared_ptr<IoState> io_;
  QTimer* timer_ = nullptr;
  bool started_ = false, closed_ = false, finalAdmitted_ = false;
};
QPointer<Pump>& pump() { static QPointer<Pump> value; return value; } // Core only.
}
void DiagnosticLogger::initialize(const TargetCompatibilityReport& report) {
  try {
    // The formatter is an owned compatibility report, not a protocol payload;
    // preserve its fixed [OK]/[FAIL] labels while cleaning external text first.
    TargetCompatibilityReport cleaned = report;
    cleaned.failureReason = redact(cleaned.failureReason);
    cleaned.kqProName = redact(cleaned.kqProName);
    { auto& m = memory(); std::lock_guard<std::mutex> guard(m.mutex); m.report = redact(cleaned.format(), 4096, false); }
    enqueue({L"INFO", report.supported ? L"compatibility_supported" : L"compatibility_unsupported", L"compatibility",
        L"initialize", L"compatibility evaluation completed", L"inspect compatibility report", {}, 0});
  } catch (...) { /* Diagnostics must not interrupt an unsupported-client refusal. */ }
}
bool DiagnosticLogger::attachStorage(StorageService* storage, QObject* owner, DiagnosticLimits limits) {
  if (!storage || !owner || storage->thread() != QThread::currentThread() || owner->thread() != QThread::currentThread()) return false;
  if (pump()) return pump()->storage() == storage && !status().closing;
  {
    auto& m = memory(); std::lock_guard<std::mutex> guard(m.mutex);
    if (m.closing) return false;
    limits = m.limits = DiagnosticStore::boundedLimits(limits);
    while (!m.pending.empty() && (m.pending.size() > static_cast<size_t>(limits.pendingEvents) || m.pendingBytes > limits.pendingBytes)) {
      m.pendingBytes -= m.pending.front().bytes(); m.pending.pop_front(); ++m.dropped;
    }
    while (!m.recent.empty() && (m.recent.size() > static_cast<size_t>(limits.recentEvents) || m.recentBytes > limits.recentBytes)) {
      m.recentBytes -= m.recent.front().bytes(); m.recent.pop_front();
    }
  }
  pump() = new Pump(storage, owner, limits);
  if (!pump()->valid()) { delete pump(); pump() = nullptr; return false; }
  return true;
}
bool DiagnosticLogger::closeStorage(StorageService* storage) {
  return storage && QThread::currentThread() == storage->thread() && pump() && pump()->storage() == storage && pump()->close();
}
DiagnosticStatus DiagnosticLogger::status() {
  auto& m = memory(); std::lock_guard<std::mutex> guard(m.mutex);
  return {m.attached, m.closing, m.salt.size() == 32, static_cast<int>(m.pending.size()) + m.inflightEvents,
      m.pendingBytes, m.recentBytes, m.saved, m.dropped, m.failed, m.admissionFailures, q(m.lastCode)};
}
void DiagnosticLogger::event(const DiagnosticEvent& e, bool error) {
  try { enqueue({error ? L"ERROR" : L"INFO", token(e.code.left(81).toStdWString(), L"event"), token(e.module.left(81).toStdWString(), L"general"),
      token(e.stage.left(81).toStdWString(), L"unspecified"), redact(e.message.left(1025).toStdWString()), redact(e.suggestedAction.left(257).toStdWString(), 256), {}, e.taskId}); }
  catch (...) { auto& m = memory(); std::lock_guard<std::mutex> guard(m.mutex); ++m.dropped; m.lastCode = L"log_encode_exception"; }
}
void DiagnosticLogger::write(const wchar_t* level, const std::wstring& category, const std::wstring& message, bool error) {
  try { enqueue({level, error ? L"operation_error" : std::wstring(level) == L"WARN" ? L"operation_warning" : L"operation_event",
      token(category, L"general"), L"observe", redact(message), error ? L"inspect task status" : L"none", {}, 0}); }
  catch (...) { auto& m = memory(); std::lock_guard<std::mutex> guard(m.mutex); ++m.dropped; m.lastCode = L"log_encode_exception"; }
}
void DiagnosticLogger::info(const QString& c, const QString& m) { write(L"INFO", c.left(81).toStdWString(), m.left(1025).toStdWString(), false); }
void DiagnosticLogger::warning(const QString& c, const QString& m) { write(L"WARN", c.left(81).toStdWString(), m.left(1025).toStdWString(), false); }
void DiagnosticLogger::error(const QString& c, const QString& m) { write(L"ERROR", c.left(81).toStdWString(), m.left(1025).toStdWString(), true); }
void DiagnosticLogger::info(const wchar_t* c, const std::wstring& m) { write(L"INFO", c ? c : L"general", m, false); }
void DiagnosticLogger::error(const wchar_t* c, const std::wstring& m) { write(L"ERROR", c ? c : L"general", m, true); }
QString DiagnosticLogger::maskedAccount(const QString& account) {
  if (account.isEmpty()) return QStringLiteral("anonymous");
  if (account.size() > 1024) return QStringLiteral("account-correlation-unavailable");
  QByteArray salt;
  { auto& m = memory(); std::lock_guard<std::mutex> guard(m.mutex); salt = QByteArray::fromStdString(m.salt); }
  if (salt.size() != 32) return QStringLiteral("account-correlation-unavailable");
  const auto bytes = QByteArrayLiteral("KQPet/account-correlation/v1\0") + salt + account.toUtf8();
  return QStringLiteral("account-%1").arg(QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().left(24)));
}
QString DiagnosticLogger::diagnosticText() {
  auto& m = memory(); std::lock_guard<std::mutex> guard(m.mutex);
  QString text = q(m.report) + QStringLiteral("\nRun: %1\nLog: <data-root>/logs/%1/\nSaved: %2; pending: %3; dropped: %4; failed: %5; admission failures: %6\nLast code: %7\nAccount correlation: %8\n")
      .arg(q(m.run)).arg(m.saved).arg(m.pending.size() + m.inflightEvents).arg(m.dropped).arg(m.failed).arg(m.admissionFailures).arg(q(m.lastCode))
      .arg(m.salt.size() == 32 ? QStringLiteral("installation-salted, not anonymous") : QStringLiteral("unavailable"));
  for (const auto& e : m.recent) text += QString::fromUtf8(encode(e));
  return text;
}
