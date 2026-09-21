#pragma once
#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QVector>

// Immutable catalog values. No resource registration, singleton or IO dependency.
enum class ShopExchangeSection {
  Permanent,
  ActivityShop,
  DiamondActivity,
};

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
  // Only a verified catalog contract may set these. Empty cost / limit fields
  // do not prove that a project is free or has unlimited exchanges.
  bool provenFree = false;
  bool provenUnlimited = false;
  // Non-zero only when the official project wording proves that one exchange
  // resolves an exact number of units for one enhance type.
  int provenGapUnitsPerExchange = 0;
  QString provenGapCode;
  QVector<int> raceIds;
  QDate shelfDate;
  QDate removalDate;
  bool hasRemovalDate = false;
  // Empty denotes the original SEF protocol. Activity-local shop/item IDs
  // belong to their own official module and must never share its counters.
  QString sourceKey;
  QString sourceUrl;
  QString costDescription;
  QJsonObject activityQueries;
  QJsonObject quotaObservation;
  QJsonObject observationWhen;
  QJsonArray activityCosts;
  QJsonArray priceOptions;
  // Classification is evidence based: diamond activities require an
  // explicitly parsed diamond price/currency in the official activity code.
  ShopExchangeSection section = ShopExchangeSection::Permanent;
  bool hasIdentity() const { return shopId > 0 && (sourceKey.isEmpty() ? itemServerId > 0 : itemServerId >= 0); }

  bool isOnlineOn(const QDate& date) const;
  QString itemKey() const;
  QString stableKey() const;
};

struct ShopExchangeShop {
  int shopId = 0;
  QString name;
  QString siKey;
  QList<ShopExchangeGood> goods;
  QString sourceKey;
  QJsonObject observation;
  // A bounded official in-game click key (for example btnNewAct_...); never
  // arbitrary JavaScript or an inferred server command.
  QString navigationLink;
  // Why the updater inspected this activity: hud, recent release, linked, or
  // emergency.  This is display evidence, not a claim that the event is open.
  QString activityEvidence;
  QString activityEvidenceDate;
};

struct ShopCatalogSnapshot {
  quint64 revision = 0;
  QJsonObject root;
  QList<ShopExchangeShop> allShops;
  QString extension;
  QString getInfoCommand;
  QString getInfoParams = QStringLiteral("{}");
  int activityId = 0;
  bool loaded = false;
  QString sourceLabel;
  QDateTime sourceUpdatedAt;
};

struct RoutineTaskDefinition {
  int id = 0;
  QString name;
  int dayFinish = 0;
  int dayActive = 0;
  int weekFinish = 0;
  int weekDailyMax = 0;
  int weekActive = 0;
};

struct ActivityOverviewDefinition {
  QString key;
  QString name;
  QDate startDate;
  int redPointId = 0;
  QVector<int> redPointIds;
};

struct RoutineCatalogSnapshot {
  quint64 revision = 0;
  QJsonObject root;
  QList<RoutineTaskDefinition> tasks;
  QList<ActivityOverviewDefinition> activities;
  QVector<int> dayPrizeThresholds;
  QVector<int> weekPrizeThresholds;
  QDateTime sourceUpdatedAt;
  QString sourceLabel;
};

struct PetDetailCatalogSnapshot {
  QByteArray contentDigest; // Stable source-content identity for disposable derived indexes.
  quint64 revision = 0;
  QJsonObject root;
  QHash<QString, int> coreToRace;
  QHash<QString, int> eraCoreToRace;
  bool loaded = false;
  QString sourceLabel;
  QDateTime sourceUpdatedAt;
};

// Public, account-independent skill metadata.  Kept separate from the
// cultivation catalog because it is much larger and has its own official H5
// release cadence.  GUI consumers retain this immutable snapshot just like
// the existing pet metadata snapshot.
struct PetSkillCatalogSnapshot {
  QByteArray contentDigest;
  quint64 revision = 0;
  QJsonObject root;
  QDateTime sourceUpdatedAt;
  QString sourceLabel;
  bool loaded = false;
};
