#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QStringList>

class QFileSystemWatcher;
class QTimer;

// Read-only ingestion for the workbench. It never opens a page, sends a game
// command or changes a watched file. All deliveries own the original bytes so
// a subsequently overwritten file cannot change a pending replay.
class BattleReplaySource final : public QObject {
  Q_OBJECT
public:
  struct Options {
    int settleMs = 750;
    int pollMs = 500;
    qint64 maximumReplayBytes = 32 * 1024 * 1024;
    qint64 maximumPendingBytes = 64 * 1024 * 1024;
    int maximumPendingCount = 32;
    int maximumSeenHashes = 2048;
  };

  explicit BattleReplaySource(QObject* parent = nullptr);
  BattleReplaySource(const Options& options, QObject* parent = nullptr);
  static QString authorizedDirectory();
  QString directory() const { return directory_; }
  QString status() const { return status_; }
  int bufferedCount() const { return static_cast<int>(pending_.size()); }
  QString identityName() const { return identityName_; }
  bool identityReliable() const { return identityReliable_; }
  QString identityEvidence() const { return identityEvidence_; }
  // Production adapter also accepts authenticated, weak read-only sessions.
  // It does not promote source/write trust or infer a nickname from an account.
  void updateSession(const QString& account, quint64 epoch, bool authenticated);

public slots:
  void startDirectory(const QString& path);
  void stopDirectory();
  void scanNow();
  void observeInbound(const QString& method, const QString& payload);
  // Epoch/account are the accepted InventoryProjection session, not a value
  // taken from the replay or its filename. A login nickname is promoted only
  // after that login is followed by a matching, advancing accepted session.
  void setSession(const QString& account, quint64 epoch, bool active);
  void invalidateSession(const QString& reason);
  void setIdentity(const QString& playerName, bool reliable, const QString& evidence);
  // The composition root calls this after connecting a lazily created page.
  // Subsequent deliveries go straight to the page's own pending-replay list.
  void replayPending();
  void suspendDelivery() { deliveryEnabled_ = false; }

signals:
  void replayReady(const QString& sourceLabel, const QByteArray& original, const QString& sha256);
  void identityChanged(const QString& playerName, bool reliable, const QString& evidence);
  void sourceStatusChanged(const QString& status);
  void sourceIssue(const QString& sourceLabel, const QString& reason);
  void bufferedCountChanged(int count);
  // Emitted before identity/delivery for a new observed login or session.
  // Both lazy source buffers and an already-created page reset at this barrier.
  void deliveryContextReset();

private:
  struct Candidate {
    qint64 size = -1, modifiedMs = -1, quietSince = 0, contentSince = -1;
    QByteArray digest;
    bool eligible = false;
  };
  struct Pending {
    QString label, sha256;
    QByteArray original;
  };
  struct Login {
    QString account, nickname;
    quint64 observedEpoch = 0;
  };
  void refreshWatches(const QStringList& files);
  void inspectFile(const QString& path, Candidate& candidate, qint64 now);
  void accept(const QString& label, const QByteArray& bytes, const QByteArray& hash);
  void resetDeliveryContext(const QString& reason);
  void flushPending();
  void setStatus(const QString& text);
  bool ordinaryReplay(const QByteArray& bytes) const;

  Options options_;
  QFileSystemWatcher* watcher_;
  QTimer* timer_;
  QElapsedTimer clock_;
  QString directory_, status_;
  QHash<QString, Candidate> files_;
  qsizetype nextScanOffset_ = 0;
  QSet<QByteArray> seen_;
  QList<QByteArray> seenOrder_;
  QList<Pending> pending_;
  qint64 pendingBytes_ = 0;
  bool deliveryEnabled_ = false;
  quint64 inboundSequence_ = 0;
  QString sessionAccount_, identityName_, identityEvidence_;
  quint64 sessionEpoch_ = 0;
  bool sessionActive_ = false, identityReliable_ = false;
  bool awaitingLoginSession_ = false;
  Login login_;
};
