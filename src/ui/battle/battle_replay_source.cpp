#include "battle_replay_source.h"
#include "battle_replay_paths.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace {
QString loginAccount(const QJsonValue& value) {
  if (value.isString()) return value.toString();
  if (!value.isDouble()) return {};
  const double number = value.toDouble();
  if (!std::isfinite(number) || number < 0 || number > 9007199254740991.0 || std::floor(number) != number) return {};
  return QString::number(static_cast<qint64>(number));
}
QStringList replayFiles(const QString& directory) {
  QStringList paths;
  for (const auto& info : QDir(directory).entryInfoList(
         {QStringLiteral("*.txt"), QStringLiteral("*.json")}, QDir::Files | QDir::NoSymLinks, QDir::Name))
    paths.append(info.absoluteFilePath());
  return paths;
}
} // namespace

BattleReplaySource::BattleReplaySource(QObject* parent) : BattleReplaySource(Options{}, parent) {}

BattleReplaySource::BattleReplaySource(const Options& options, QObject* parent)
    : QObject(parent), options_(options), watcher_(new QFileSystemWatcher(this)), timer_(new QTimer(this)) {
  options_.settleMs = std::max(1, options_.settleMs);
  options_.pollMs = std::max(1, options_.pollMs);
  options_.maximumReplayBytes = std::max<qint64>(1, options_.maximumReplayBytes);
  options_.maximumPendingBytes = std::max<qint64>(options_.maximumReplayBytes, options_.maximumPendingBytes);
  options_.maximumPendingCount = std::max(1, options_.maximumPendingCount);
  options_.maximumSeenHashes = std::max(1, options_.maximumSeenHashes);
  clock_.start();
  timer_->setInterval(options_.pollMs);
  connect(timer_, &QTimer::timeout, this, &BattleReplaySource::scanNow);
  connect(watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { scanNow(); });
  connect(watcher_, &QFileSystemWatcher::fileChanged, this, [this](const QString& path) {
    auto found = files_.find(path);
    if (found != files_.end()) {
      found->eligible = true;
      found->quietSince = clock_.elapsed();
      found->contentSince = -1;
      found->digest.clear();
    }
    scanNow();
  });
  setIdentity({}, false, QStringLiteral("尚未收到与当前会话一致的游戏昵称，请手动选择我方"));
}

QString BattleReplaySource::authorizedDirectory() {
  return BattleReplayPaths::reportDirectory();
}

void BattleReplaySource::setStatus(const QString& text) {
  if (status_ == text) return;
  status_ = text;
  emit sourceStatusChanged(status_);
}

void BattleReplaySource::startDirectory(const QString& path) {
  stopDirectory();
  if (path.isEmpty()) return;
  directory_ = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
  // Existing files are a baseline, not newly arrived reports. A subsequent
  // write is still observed, including an atomic replacement of the same path.
  if (QFileInfo(directory_).isDir()) {
    for (const auto& file : replayFiles(directory_)) {
      const QFileInfo info(file);
      Candidate candidate;
      candidate.size = info.size();
      candidate.modifiedMs = info.lastModified().toMSecsSinceEpoch();
      candidate.quietSince = clock_.elapsed();
      files_.insert(file, candidate);
    }
  }
  scanNow();
  timer_->start();
}

void BattleReplaySource::stopDirectory() {
  timer_->stop();
  const auto filePaths = watcher_->files(), directories = watcher_->directories();
  if (!filePaths.isEmpty()) watcher_->removePaths(filePaths);
  if (!directories.isEmpty()) watcher_->removePaths(directories);
  files_.clear();
  nextScanOffset_ = 0;
  directory_.clear();
  setStatus(QStringLiteral("样本目录监听未启动；现有游戏入站通道仍可接收战报"));
}

void BattleReplaySource::refreshWatches(const QStringList& files) {
  QSet<QString> wantedDirectories;
  // Keep the closest existing ancestor watched as well: a deleted directory
  // loses its native watch. Polling also recovers missed/unsupported watches.
  QString ancestor = directory_;
  while (!ancestor.isEmpty()) {
    const QFileInfo info(ancestor);
    if (info.isDir()) {
      wantedDirectories.insert(info.absoluteFilePath());
      if (ancestor != directory_) break;
    }
    const QString parent = info.dir().absolutePath();
    if (parent == ancestor) break;
    ancestor = parent;
  }
  const auto watchedFiles = watcher_->files(), watchedDirectories = watcher_->directories();
  const QSet<QString> wantedFiles(files.begin(), files.end());
  for (const auto& old : watchedFiles) if (!wantedFiles.contains(old)) watcher_->removePath(old);
  for (const auto& old : watchedDirectories) if (!wantedDirectories.contains(old)) watcher_->removePath(old);
  for (const auto& file : files) if (!watchedFiles.contains(file)) watcher_->addPath(file);
  for (const auto& dir : wantedDirectories) if (!watchedDirectories.contains(dir)) watcher_->addPath(dir);
}

void BattleReplaySource::scanNow() {
  if (directory_.isEmpty()) return;
  const bool available = QFileInfo(directory_).isDir();
  const auto paths = available ? replayFiles(directory_) : QStringList{};
  refreshWatches(paths);
  setStatus(available ? QStringLiteral("自动接收游戏战报；只读监听 %1（新文件写完后加入待阅）").arg(directory_)
                      : QStringLiteral("自动接收游戏战报；等待样本目录 %1 恢复").arg(directory_));
  const QSet<QString> current(paths.begin(), paths.end());
  for (auto it = files_.begin(); it != files_.end();)
    if (!current.contains(it.key())) it = files_.erase(it); else ++it;
  const qint64 now = clock_.elapsed();
  int reads = 0;
  const auto first = paths.isEmpty() ? 0 : nextScanOffset_ % paths.size();
  for (qsizetype index = 0; index < paths.size(); ++index) {
    const auto position = (first + index) % paths.size();
    const auto& path = paths[position];
    const QFileInfo info(path);
    auto found = files_.find(path);
    if (found == files_.end() || found->size != info.size() || found->modifiedMs != info.lastModified().toMSecsSinceEpoch()) {
      Candidate candidate;
      candidate.size = info.size(); candidate.modifiedMs = info.lastModified().toMSecsSinceEpoch();
      candidate.quietSince = now; candidate.eligible = true;
      found = files_.insert(path, candidate);
    }
    if (found->eligible && now - found->quietSince >= options_.settleMs) {
      // A bulk directory import must not monopolize the workbench event loop.
      // Metadata is cheap; at most two full file reads happen in one scan.
      const bool readsFile = found->size > 0 && found->size <= options_.maximumReplayBytes;
      if (readsFile && reads >= 2) continue;
      if (readsFile) {
        ++reads;
        // Unreadable/locked files still consume a read attempt. Rotate past
        // them so two early filenames cannot starve every later report.
        nextScanOffset_ = (position + 1) % paths.size();
      }
      inspectFile(path, *found, now);
    }
  }
}

void BattleReplaySource::inspectFile(const QString& path, Candidate& candidate, qint64 now) {
  if (candidate.size <= 0) return;
  if (candidate.size > options_.maximumReplayBytes) {
    candidate.eligible = false;
    emit sourceIssue(path, QStringLiteral("战报超过自动接入的 %1 MiB 限制，未载入").arg(options_.maximumReplayBytes / 1024 / 1024));
    return;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return;
  const QByteArray bytes = file.read(options_.maximumReplayBytes + 1);
  const QFileInfo after(path);
  if (file.error() != QFileDevice::NoError || bytes.size() != candidate.size || after.size() != candidate.size ||
      after.lastModified().toMSecsSinceEpoch() != candidate.modifiedMs) {
    candidate.quietSince = now; candidate.contentSince = -1; candidate.digest.clear();
    return;
  }
  const QByteArray digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
  if (candidate.digest != digest || candidate.contentSince < 0) {
    candidate.digest = digest; candidate.contentSince = now;
    return;
  }
  if (now - candidate.contentSince < options_.settleMs) return;
  candidate.eligible = false;
  if (ordinaryReplay(bytes)) accept(path, bytes, digest);
  // A partial/unrelated JSON file is not a replay notification. A later write
  // invalidates the candidate and is checked again; no guessed packet repair.
}

bool BattleReplaySource::ordinaryReplay(const QByteArray& bytes) const {
  const auto document = QJsonDocument::fromJson(bytes);
  if (!document.isObject()) return false;
  const auto root = document.object();
  const auto battle = root.contains(QStringLiteral("br")) ? root[QStringLiteral("br")].toObject() : root;
  const auto teamBattle = battle[QStringLiteral("tbt")];
  if (!teamBattle.isNull() && !teamBattle.isUndefined()) return false;
  return battle[QStringLiteral("or")].isObject() && battle[QStringLiteral("fr")].isArray() &&
      battle[QStringLiteral("or")].toObject()[QStringLiteral("team")].isArray();
}

void BattleReplaySource::observeInbound(const QString& method, const QString& payload) {
  if ((method != QStringLiteral("recivedata") && method != QStringLiteral("cutdata") && method != QStringLiteral("otherreturn")) ||
      payload.isEmpty() || payload.size() > options_.maximumReplayBytes) return;
  // Most captured messages are unrelated inventory/GUI packets. Keep this
  // passive subscriber cheap without broadening the bridge's capture scope.
  if (!payload.contains(QStringLiteral("\"br\"")) && !payload.contains(QStringLiteral("\"or\"")) &&
      !payload.contains(QStringLiteral("21_1"))) return;
  const auto bytes = payload.toUtf8();
  if (bytes.size() > options_.maximumReplayBytes) return;
  const auto document = QJsonDocument::fromJson(bytes);
  if (!document.isObject()) return;
  const auto root = document.object();
  if (root[QStringLiteral("_cmd")].toString() == QStringLiteral("21_1")) {
    if (method != QStringLiteral("recivedata")) return;
    const auto info = root[QStringLiteral("info")].toObject();
    const auto account = loginAccount(info[QStringLiteral("n")]);
    // An invalid account cannot establish a login boundary in PetRepository.
    if (account.isEmpty()) return;
    resetDeliveryContext(QStringLiteral("已观察到新的登录记录，旧会话待阅已隔离"));
    login_ = {account, info[QStringLiteral("nn")].toString(), sessionEpoch_};
    awaitingLoginSession_ = true;
    setIdentity({}, false, QStringLiteral("已收到登录记录，等待该账号被本会话接纳；尚不自动选择我方"));
    return;
  }
  if (!ordinaryReplay(bytes)) return;
  const QString label = QStringLiteral("游戏入站战报 %1（现有被动通道 %2）").arg(++inboundSequence_).arg(method);
  accept(label, bytes, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
}

void BattleReplaySource::accept(const QString& label, const QByteArray& bytes, const QByteArray& hash) {
  if (seen_.contains(hash)) return;
  seen_.insert(hash); seenOrder_.append(hash);
  while (seenOrder_.size() > options_.maximumSeenHashes) seen_.remove(seenOrder_.takeFirst());
  const QString sha = QString::fromLatin1(hash.toHex());
  if (deliveryEnabled_ && !awaitingLoginSession_) { emit replayReady(label, bytes, sha); return; }
  while (!pending_.isEmpty() && (pending_.size() >= options_.maximumPendingCount ||
                                pendingBytes_ + bytes.size() > options_.maximumPendingBytes)) {
    const auto removed = pending_.takeFirst(); pendingBytes_ -= removed.original.size();
    emit sourceIssue(removed.label, QStringLiteral("回放页尚未接收或登录会话待确认，待阅缓冲已满；最早一份战报已移出缓冲"));
  }
  pending_.append({label, sha, bytes}); pendingBytes_ += bytes.size();
  emit bufferedCountChanged(bufferedCount());
}

void BattleReplaySource::replayPending() {
  deliveryEnabled_ = true;
  flushPending();
}

void BattleReplaySource::flushPending() {
  while (deliveryEnabled_ && !awaitingLoginSession_ && !pending_.isEmpty()) {
    const auto record = pending_.takeFirst(); pendingBytes_ -= record.original.size();
    emit replayReady(record.label, record.original, record.sha256);
  }
  emit bufferedCountChanged(bufferedCount());
}

void BattleReplaySource::resetDeliveryContext(const QString& reason) {
  const int removed = bufferedCount();
  pending_.clear(); pendingBytes_ = 0;
  // Identical bytes can legitimately be received again in a later session,
  // after the earlier page's pending list has been reset.
  seen_.clear(); seenOrder_.clear();
  emit deliveryContextReset();
  emit bufferedCountChanged(0);
  if (removed > 0) emit sourceIssue(QStringLiteral("会话待阅区"),
    QStringLiteral("%1；已移出此前的 %2 份待阅快照，原文件未改动").arg(reason).arg(removed));
}

void BattleReplaySource::updateSession(const QString& account, quint64 epoch, bool authenticated) {
  // The read projection sets authenticated only for an accepted login and
  // clears it on disconnect, authentication/rejection or capture failure.
  // Weak read sessions remain eligible without promoting game-write trust.
  setSession(account, epoch, authenticated && !account.isEmpty() && epoch > 0);
}

void BattleReplaySource::invalidateSession(const QString& reason) {
  login_ = {}; awaitingLoginSession_ = false; sessionActive_ = false;
  resetDeliveryContext(reason);
  setIdentity({}, false, reason + QStringLiteral("；请手动选择我方"));
}

void BattleReplaySource::setSession(const QString& account, quint64 epoch, bool active) {
  if (account == sessionAccount_ && epoch == sessionEpoch_ && active == sessionActive_) return;
  const bool matchingBoundary = awaitingLoginSession_ && active && epoch > sessionEpoch_ &&
      login_.observedEpoch == sessionEpoch_ && !account.isEmpty() && login_.account == account;
  const bool matchingLogin = matchingBoundary && !login_.nickname.isEmpty();
  const auto nickname = matchingLogin ? login_.nickname : QString{};
  if (!matchingBoundary) resetDeliveryContext(QStringLiteral("会话发生变化或登录未被该账号接纳，待阅按会话隔离"));
  sessionAccount_ = account; sessionEpoch_ = epoch; sessionActive_ = active;
  login_ = {}; awaitingLoginSession_ = false;
  if (matchingLogin) {
    setIdentity(nickname, true, QStringLiteral("已记录的 21_1.info.nn；info.n 与随后接纳的当前账号、会话 %1 一致").arg(epoch));
  } else {
    setIdentity({}, false, active
      ? QStringLiteral("当前会话没有已核实的登录昵称（info.nn）；登录号不能代替战报玩家名，请手动选择我方")
      : QStringLiteral("当前未登录或会话已失效，离线账号不能证明本场我方，请手动选择我方"));
  }
  // A login and its first report may be captured in the same GUI drain. Wait
  // until the projection/workbench session reset has happened before delivery.
  flushPending();
}

void BattleReplaySource::setIdentity(const QString& playerName, bool reliable, const QString& evidence) {
  reliable = reliable && !playerName.isEmpty() && !evidence.isEmpty();
  if (identityName_ == playerName && identityReliable_ == reliable && identityEvidence_ == evidence) return;
  identityName_ = playerName; identityReliable_ = reliable; identityEvidence_ = evidence;
  emit identityChanged(identityName_, identityReliable_, identityEvidence_);
}
