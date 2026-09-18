#pragma once

#include "../domain/catalog_types.h"
#include "../application/catalog_business_date.h"

#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QVector>
#include <memory>
#include <atomic>


class ShopExchangeCatalog final {
public:
  static ShopExchangeCatalog& instance();

  std::shared_ptr<const ShopCatalogSnapshot> snapshot() const { return std::atomic_load(&snapshot_); }
  bool isLoaded() const { return snapshot()->loaded; }
  QString extension() const { return snapshot()->extension; }
  QString getInfoCommand() const { return snapshot()->getInfoCommand; }
  QString getInfoParams() const { return snapshot()->getInfoParams; }
  int activityId() const { return snapshot()->activityId; }
  QList<ShopExchangeShop> shops(const QDate& date = currentCatalogBusinessDate()) const;
  QList<ShopExchangeShop> protocolShops(const QDate& date = currentCatalogBusinessDate()) const;
  QList<ShopExchangeGood> onlineGoods(const QDate& date = currentCatalogBusinessDate()) const;
  QString sourceLabel() const { return snapshot()->sourceLabel; }
  QDateTime sourceUpdatedAt() const { return snapshot()->sourceUpdatedAt; }

  static std::shared_ptr<const ShopCatalogSnapshot> prepare(
      const QJsonObject& root, const QString& source, const QDateTime& updatedAt,
      QString* error = nullptr);
  static QJsonObject parseOfficialText(const QString& text, const QJsonObject& protocol,
                                      QString* error = nullptr);

  // -1 means unknown; absent fields are never assumed to mean zero uses.
  static int usedCount(const QJsonObject& packet, const ShopExchangeGood& good);
  // Explicit unlimited contracts return INT_MAX for legacy display consumers.
  static int remainingCount(const QJsonObject& packet, const ShopExchangeGood& good);

private:
  ShopExchangeCatalog();
  friend class CatalogIoService;
  void publish(std::shared_ptr<const ShopCatalogSnapshot> value) { std::atomic_store(&snapshot_, std::move(value)); }
  static QDate parseYmd(const QString& text);

  std::shared_ptr<const ShopCatalogSnapshot> snapshot_ = std::make_shared<ShopCatalogSnapshot>();
};
