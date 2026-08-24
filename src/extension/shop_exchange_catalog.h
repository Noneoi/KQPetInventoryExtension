#pragma once

#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QVector>

struct ShopExchangeGood {
  int shopId = 0;
  int itemServerId = 0;
  int tab = 0;
  QString shopName;
  QString description;
  QString limitText;
  QString limitKey;
  QString limitLabel;
  int limitCount = 0;
  QString cost;
  QString enhanceType;
  QString unlock;
  QString tag;
  // Non-zero only when the official project wording proves that one exchange
  // resolves an exact number of units for one enhance type.
  int provenGapUnitsPerExchange = 0;
  QString provenGapCode;
  QVector<int> raceIds;
  QDate shelfDate;
  QDate removalDate;
  bool hasRemovalDate = false;

  bool isOnlineOn(const QDate& date) const;
  QString itemKey() const;
  QString stableKey() const;
};

struct ShopExchangeShop {
  int shopId = 0;
  QString name;
  QString siKey;
  QList<ShopExchangeGood> goods;
};

class ShopExchangeCatalog final {
public:
  static ShopExchangeCatalog& instance();

  bool isLoaded() const { return loaded_; }
  QString extension() const { return extension_; }
  QString getInfoCommand() const { return getInfoCommand_; }
  QString getInfoParams() const { return getInfoParams_; }
  int activityId() const { return activityId_; }
  QList<ShopExchangeShop> shops(const QDate& date = QDate::currentDate()) const;
  QList<ShopExchangeGood> onlineGoods(const QDate& date = QDate::currentDate()) const;
  QString sourceLabel() const { return sourceLabel_; }
  QDateTime sourceUpdatedAt() const { return sourceUpdatedAt_; }

  // Loads an already generated external catalog when present. The embedded
  // catalog remains the last-known-good fallback.
  bool reloadFromDataRoot(const QString& dataRoot, QString* error = nullptr);
  // Finds the newest official SEFConfig.as, parses every CommonEnhancePrize,
  // atomically writes a runtime catalog, then switches to it.
  bool updateFromOfficialData(const QString& dataRoot, QString* error = nullptr);

  static QJsonObject itemObject(const QJsonObject& packet, const ShopExchangeGood& good);
  static int usedCount(const QJsonObject& packet, const ShopExchangeGood& good);
  static int remainingCount(const QJsonObject& packet, const ShopExchangeGood& good);

private:
  ShopExchangeCatalog();
  bool loadRoot(const QJsonObject& root, const QString& source,
                const QDateTime& updatedAt, QString* error = nullptr);
  static QJsonObject parseOfficialConfig(const QString& path,
                                         const QJsonObject& protocol,
                                         QString* error);
  static QString findOfficialConfig(QString* error);
  static QDate parseYmd(const QString& text);
  static QJsonObject objectValue(const QJsonObject& object, const QString& key);

  QJsonObject root_;
  QList<ShopExchangeShop> shops_;
  QString extension_;
  QString getInfoCommand_;
  QString getInfoParams_ = QStringLiteral("{}");
  int activityId_ = 0;
  bool loaded_ = false;
  QString sourceLabel_;
  QDateTime sourceUpdatedAt_;
};
