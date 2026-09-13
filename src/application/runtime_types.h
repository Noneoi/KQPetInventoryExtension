#pragma once

#include "contracts/operation_types.h"
#include "storage_types.h"
#include "../domain/shop_actionability.h"
#include "../domain/catalog_types.h"
#include "../contracts/observation_types.h"
#include "../contracts/cultivation_material_inventory.h"
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QMetaType>

struct RuntimeStatus {
  QString account;
  quint64 epoch = 0;
  QString module;
  QString message;
};
struct RuntimeShopState {
  QString account;
  quint64 epoch = 0;
  quint64 publication = 0;
  QJsonObject packet;
  QHash<QString, qint64> materialCounts;
  QHash<QString, qint64> cachedMaterialCounts;
  QJsonObject unverifiedPackets;
  bool hasPacket = false;
  bool hasMaterialCounts = false;
  bool running = false;
  QString status;
  QString freshnessSummary;
  quint64 freshnessRevision = 0;
  QHash<QString, ShopCondition> quotaValidity;
  std::shared_ptr<const ShopCatalogSnapshot> catalog;
  QDate catalogDate;
  MaterialInventorySnapshot cultivationMaterials;
};
struct RuntimeRoutineState {
  QString account;
  quint64 epoch = 0;
  quint64 publication = 0;
  QJsonObject dailyPacket;
  QSet<int> activeRedPoints;
  QJsonObject opportunityPackets;
  QJsonObject cachedOpportunityPackets;
  QJsonObject unverifiedPackets;
  bool hasDailyPacket = false;
  bool hasRedPointPacket = false;
  bool running = false;
  QString status;
  QString freshnessSummary;
  std::shared_ptr<const RoutineCatalogSnapshot> catalog;
  QHash<QString, ObservationValidity> periodValidity;
};
struct RuntimeDetailProgress {
  QString account;
  quint64 epoch = 0;
  bool running = false;
  bool paused = false;
  int completed = 0;
  int total = 0;
  int succeeded = 0;
  int failed = 0;
  qint64 instanceId = 0;
  int estimatedSeconds = 0;
};
struct RuntimeMoveState {
  QString account;
  quint64 epoch = 0;
  quint64 moveTaskId = 0;
  QString operationId;
  bool running = false;
  MoveOutcome outcome = MoveOutcome::NotSent;
  QString message;
};
struct RuntimePersistenceState {
  QString account;
  quint64 epoch = 0;
  QString record;
  quint64 revision = 0;
  StorageStatus status = StorageStatus::Queued;
  int pendingWrites = 0;
  QString error;
  bool hasSaved = false;
  int failedRecords = 0;
  bool failuresTruncated = false;
};

Q_DECLARE_METATYPE(RuntimeStatus)
Q_DECLARE_METATYPE(RuntimeShopState)
Q_DECLARE_METATYPE(RuntimeRoutineState)
Q_DECLARE_METATYPE(RuntimeDetailProgress)
Q_DECLARE_METATYPE(RuntimeMoveState)
Q_DECLARE_METATYPE(RuntimePersistenceState)
