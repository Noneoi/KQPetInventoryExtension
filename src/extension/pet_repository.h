#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>

class QJsonValue;

class PetRepository final : public QObject {
  Q_OBJECT

public:
  explicit PetRepository(QObject* parent = nullptr);

  QList<QJsonObject> backpackPets() const;
  QList<QJsonObject> warehousePets() const;
  QJsonObject detailFor(qint64 instanceId) const;
  QJsonObject warehousePet(qint64 instanceId) const;
  QList<qint64> warehouseIdsByDetailAge() const;
  bool hasCachedDetail(qint64 instanceId) const;
  QDateTime detailSavedAt(qint64 instanceId) const;

  QString accountKey() const { return accountKey_; }
  QString cachePath() const { return cachePath_; }
  QString dataRoot() const { return cacheRoot_; }
  QDateTime updatedAt() const { return updatedAt_; }
  bool isOnlineData() const { return onlineData_; }
  bool isAuthenticated() const { return authenticated_; }
  quint64 sessionGeneration() const { return sessionGeneration_; }

  void beginListRefresh(quint64 requestGeneration, const QString& account,
                        quint64 sessionGeneration);
  void expectListPart(const QString& command, quint64 requestGeneration,
                      const QString& account, quint64 sessionGeneration);
  void cancelListPart(const QString& command, quint64 requestGeneration);
  void expectDetail(qint64 instanceId, quint64 requestGeneration,
                    const QString& account, quint64 sessionGeneration);
  void cancelDetailRequest(qint64 instanceId, quint64 requestGeneration);

public slots:
  void handlePacket(const QString& method, const QString& payload);

signals:
  void dataChanged();
  void detailChanged(qint64 instanceId);
  void listResponseAccepted(const QString& command, quint64 requestGeneration);
  void detailResponseAccepted(qint64 instanceId, quint64 requestGeneration);
  void detailResponseRejected(qint64 instanceId, quint64 requestGeneration,
                              const QString& reason);
  void accountSessionChanged(const QString& account, quint64 sessionGeneration);
  void visualMismatchDetected(qint64 instanceId);
  void statusChanged(const QString& status);

private:
  struct RequestExpectation {
    QString account;
    quint64 sessionGeneration = 0;
    quint64 requestGeneration = 0;
    qint64 instanceId = 0;
    bool active = false;
  };

  static qint64 petId(const QJsonObject& pet);
  static QList<QJsonObject> objectsIn(const QJsonValue& value);
  static QJsonObject merge(const QJsonObject& base, const QJsonObject& overlay);
  static QString stringValue(const QJsonValue& value);
  static QJsonObject warehouseBriefForCache(const QJsonObject& pet);
  static bool validDetail(const QJsonObject& pet);
  static bool visualIdentityDiffers(const QJsonObject& detail,
                                    const QJsonObject& brief);

  bool parseBackpack(const QJsonObject& packet);
  bool parseWarehouse(const QJsonObject& packet);
  bool parseDetail(const QJsonObject& packet, quint64 requestGeneration);
  bool expectationMatches(const RequestExpectation& expectation) const;
  void activateAccountSession(const QString& account);
  void clearExpectations();
  void loadCache();
  void loadAccount();
  void loadDetails();
  void saveInventory();
  bool saveDetail(qint64 instanceId, const QJsonObject& detail,
                  const QDateTime& savedAt = {});
  void setAccountPaths();
  void writeLastAccount() const;
  void migrateLegacyCache();
  QString accountDirectory(const QString& account) const;
  QList<QJsonObject> sorted(const QHash<qint64, QJsonObject>& source) const;

  QHash<qint64, QJsonObject> backpack_;
  QHash<qint64, QJsonObject> warehouse_;
  QHash<qint64, QJsonObject> details_;
  QHash<qint64, QDateTime> detailSavedTimes_;
  QString accountKey_ = QStringLiteral("default");
  QString cacheRoot_;
  QString cachePath_;
  QString detailsPath_;
  QDateTime updatedAt_;
  quint64 sessionGeneration_ = 0;
  bool authenticated_ = false;
  bool onlineData_ = false;
  RequestExpectation backpackExpectation_;
  RequestExpectation warehouseExpectation_;
  RequestExpectation detailExpectation_;
};
