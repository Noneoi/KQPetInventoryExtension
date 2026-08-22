#pragma once

#include <QAbstractTableModel>
#include <QJsonObject>
#include <QList>

class PetImageCache;
class PetRepository;

class PetTableModel final : public QAbstractTableModel {
  Q_OBJECT

public:
  enum DataRole {
    InstanceIdRole = Qt::UserRole,
    IdentityTextRole = Qt::UserRole + 1,
    PetObjectRole = Qt::UserRole + 2,
  };

  enum class Location {
    Backpack,
    Warehouse,
  };

  explicit PetTableModel(Location location, PetRepository* repository = nullptr,
                         PetImageCache* imageCache = nullptr,
                         QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index,
                int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const override;

  void setPets(const QList<QJsonObject>& pets);
  bool updatePet(const QJsonObject& pet);
  QJsonObject petAt(int row) const;
  qint64 instanceIdAt(int row) const;
  int rowForInstanceId(qint64 instanceId) const;
  Location location() const;

private:
  QString displayName(const QJsonObject& pet) const;
  QString displayValue(const QJsonObject& pet, int column) const;

  Location location_ = Location::Warehouse;
  PetRepository* repository_ = nullptr;
  PetImageCache* imageCache_ = nullptr;
  QList<QJsonObject> pets_;
};
