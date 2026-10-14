#include "public_data_view.h"
#include <QJsonArray>

PublicRecordValue PublicDataView::record(const PublicRecordKey& key) const {
  PublicRecordValue result; result.key = key;
  if (key.view != kind_) { result.state = PublicValueState::WrongView; return result; }
  const auto source = root_.value(key.table);
  if (source.isUndefined()) return result;
  if (!source.isObject()) { result.state = PublicValueState::InvalidContainer; return result; }
  result.raw = source.toObject().value(key.id);
  if (result.raw.isUndefined()) return result;
  const bool empty = result.raw.isNull() ||
      (result.raw.isString() && result.raw.toString().isEmpty()) ||
      (result.raw.isObject() && result.raw.toObject().isEmpty()) ||
      (result.raw.isArray() && result.raw.toArray().isEmpty());
  result.state = empty ? PublicValueState::Empty : PublicValueState::Present;
  return result;
}

QJsonObject PublicDataView::object(const QString& table, const QString& id) const {
  return record({kind_, table, id}).raw.toObject();
}
