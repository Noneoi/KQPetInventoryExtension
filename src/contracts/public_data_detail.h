#pragma once

#include "contracts/public_data_contract.h"
#include "contracts/public_dataset.h"
#include <QVector>
#include <QStringList>

// A navigation history entry retains these values. A replay-only context can
// never borrow the currently published inventory/skill catalog.
struct PublicDataDetailContext {
  PublicDatasetSnapshotHandle dataset;
  BattleReplayCatalogHandle replay;
};

struct PublicDataDetailField { QString label, text; };
struct PublicDataDetailRelation {
  PublicRecordKey key;
  QString label;
  QString evidence; // Exact source field/table, shown only in source details.
};
struct PublicDataDetail {
  PublicRecordKey key;
  PublicValueState state = PublicValueState::Missing;
  QString title, kind, description, formName, revision;
  QStringList conditions;
  QVector<PublicDataDetailField> fields;
  QVector<PublicDataDetailRelation> relations;
  QJsonObject source;
  QJsonValue raw = QJsonValue(QJsonValue::Undefined);
  // This contract contains definitions, never observed combat events/stacks.
  bool replayReference = false;
};
