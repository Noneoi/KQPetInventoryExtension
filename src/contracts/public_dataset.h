#pragma once

#include "domain/catalog_types.h"
#include "contracts/battle_replay_catalog.h"

#include <QMap>
#include <QMetaType>
#include <memory>

// Values only: neither this contract nor a retained snapshot performs I/O.
struct PublicDatasetObject {
  QString sha256;
  qint64 bytes = 0;
  QString format;
  QJsonObject origin;
  QString path; // Verified absolute immutable object path, assigned by the store.
};

struct PublicDatasetSnapshot {
  QString revision;
  QJsonObject manifest;
  QMap<QString, PublicDatasetObject> components;
  QMap<QString, PublicDatasetObject> attachments;
  std::shared_ptr<const PetDetailCatalogSnapshot> detail;
  std::shared_ptr<const PetSkillCatalogSnapshot> skill;
  std::shared_ptr<const ShopCatalogSnapshot> shop;
  std::shared_ptr<const RoutineCatalogSnapshot> routine;
  QJsonObject imageIndex;
  QJsonObject iconIndex;
  PublicDatasetObject replayObject;
  BattleReplayCatalogHandle replay; // May be deferred; replayObject remains pinned.
};
using PublicDatasetSnapshotHandle = std::shared_ptr<const PublicDatasetSnapshot>;
Q_DECLARE_METATYPE(PublicDatasetSnapshotHandle)
