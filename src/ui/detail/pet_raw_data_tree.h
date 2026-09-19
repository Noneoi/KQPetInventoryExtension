#pragma once

#include <QJsonObject>
#include <QTreeWidget>
#include "contracts/pet_record_types.h"

// Shared read-only raw inspector. Hidden tabs retain only the immutable JSON;
// each expansion creates at most one small page of children, without recursion.
class PetRawDataTree final : public QTreeWidget {
public:
  explicit PetRawDataTree(QWidget* parent = nullptr);
  ~PetRawDataTree() override;
  void setJson(const QJsonObject& value);
  void setRecord(const QJsonObject& value, RawPetRecordHandle record);
  void clearJson();
protected:
  void showEvent(QShowEvent* event) override;
private:
  void populateRoot();
  void populateItem(QTreeWidgetItem* item);
  void appendPage(const QJsonValue& value, int offset, QTreeWidgetItem* parent);
  void addValue(const QString& key, const QJsonValue& value, QTreeWidgetItem* parent);
  RawPetRecordHandle record_;
  QJsonObject value_;
  bool dirty_ = true;
};
