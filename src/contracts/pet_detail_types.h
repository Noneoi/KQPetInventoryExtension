#pragma once
#include "pet_derivation_types.h"
#include "../domain/pet_cultivation_requirements.h"
#include "../domain/catalog_types.h"
#include <QJsonObject>
#include <QVector>
#include <QStringList>
#include <memory>

enum class DetailKnowledge { Known, Unknown, Invalid };
enum class DetailSection { Overview, Badges, Astrolabe, EquippedStargods, StargodBackpack, SummonRelations, CarryRelations };
enum class DetailPreparationStatus { Queued, Ready, WaitingInputs, WaitingSummaries, Cancelled, Superseded, InvalidRequest, BudgetExceeded, ComputeUnavailable, Closing };

struct DetailSelection {
  QString account;
  quint64 epoch = 0;
  qint64 instanceId = 0;
  bool operator==(const DetailSelection& other) const {
    return account == other.account && epoch == other.epoch && instanceId == other.instanceId;
  }
};
struct DetailRelatedRevision { qint64 instanceId = 0; quint64 revision = 0; bool present = false; };
struct DetailVersion {
  PetDerivationKey facts;
  quint64 summaryRevision = 0;
  quint64 relatedSummaryRevision = 0;
  int detailVersion = 2;
  QVector<DetailRelatedRevision> related;
};
struct DetailField {
  std::shared_ptr<void> memoryRetention;
  QString label;
  QString text;
  DetailKnowledge state = DetailKnowledge::Unknown;
};
struct DetailEntry {
  std::shared_ptr<void> memoryRetention;
  QString group;
  QString name;
  QVector<DetailField> fields;
  DetailKnowledge state = DetailKnowledge::Known;
  QString problem;
  int quality = 0;
  int imageDefineId = 0;
  bool changeable = false, selected = false, activated = false;
  bool emptySlot = false;
};
struct DetailPage {
  std::shared_ptr<void> memoryRetention;
  DetailSection section = DetailSection::Overview;
  int pageIndex = 0;
  int pageSize = 64;
  quint64 totalItems = 0;
  DetailKnowledge state = DetailKnowledge::Unknown;
  QString problem;
  QVector<DetailEntry> entries;
  bool carryCandidatesExpanded = false, hasCarryCandidates = false, carryCandidatesKnown = false;
  bool hasPrevious() const { return pageIndex > 0; }
  bool hasNext() const { return pageIndex >= 0 && pageSize > 0 && (quint64(pageIndex) + 1) * quint64(pageSize) < totalItems; }
};
struct DetailIdentity {
  std::shared_ptr<void> memoryRetention;
  qint64 instanceId = 0;
  int raceId = 0;
  QString name, originalName, customName, attributes, jobs, era;
  DetailField level;
  QString visualKey;
  QStringList imageCandidateNames;
};
struct PreparedPetDetail {
  // All outward copies must keep this lease, including copies of page/model data.
  std::shared_ptr<void> memoryRetention;
  DetailVersion version;
  DetailIdentity identity;
  bool detailKnown = false, sourceVerified = false, visualMismatch = false;
  PetBattlePowerState battlePower;
  PetCultivationRequirements cultivationRequirements;
  QVector<DetailField> talent;
  QVector<DetailField> sacred;
  QVector<DetailPage> pages;
  quint64 chargedBytes = 0;
};
using PreparedPetDetailHandle = std::shared_ptr<const PreparedPetDetail>;

// Values are frozen by Core. The large payload and facts retain their original
// owners' leases; this service charges only its additional allocations.
struct FrozenDetailInputs {
  RawPetRecordHandle raw;
  PetDerivedFactsHandle facts;
  QJsonObject brief;
  quint64 summaryRevision = 0;
  std::shared_ptr<const PetDetailCatalogSnapshot> metadata;
  bool sourceVerified = false;
};
struct DetailRelatedSummary {
  qint64 instanceId = 0;
  quint64 revision = 0;
  bool present = false;
  QJsonObject fields; // only id/r/ri/fr/n/customName/lv/zdl/xzdl and _meta* identity fields
};
struct DetailSubmission {
  quint64 requestId = 0;
  bool accepted = false;
  DetailPreparationStatus status = DetailPreparationStatus::InvalidRequest;
  QString error;
};
Q_DECLARE_METATYPE(DetailSelection)
Q_DECLARE_METATYPE(DetailSection)
Q_DECLARE_METATYPE(DetailPreparationStatus)
Q_DECLARE_METATYPE(PreparedPetDetailHandle)
Q_DECLARE_METATYPE(FrozenDetailInputs)
Q_DECLARE_METATYPE(DetailRelatedSummary)
Q_DECLARE_METATYPE(QVector<DetailRelatedSummary>)
