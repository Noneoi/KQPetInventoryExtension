#pragma once
#include "pet_derivation_types.h"
#include "domain/pet_cultivation_requirements.h"
#include "domain/catalog_types.h"
#include <QJsonObject>
#include <QVector>
#include <QStringList>
#include <memory>

enum class DetailKnowledge { Known, Unknown, Invalid };
// Missing means absent from the currently loaded local rosters, not proof of
// non-ownership. Unknown means no roster identity was consulted for this row.
enum class DetailOwnership { Unknown, Missing, Backpack, WarehouseNormal, WarehouseElite, WarehouseGoodbye,
                             OwnedLocationUnknown, WarehouseUnknown };
enum class DetailCarryKind { Unknown, Companion, Almighty, Swallow };
// Independent detail slots the preparation service serves at once:
// 0 = the selected pet, 1 = the shop preview, 2 = the related-pet popup.
inline constexpr int kDetailConsumerCount = 3;
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
  quint64 skillRevision = 0, relationRosterRevision = 0;
  QByteArray skillDigest;
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
  // Relation entries only: the related pet's own instance, its original name
  // and where this account keeps it. Zero/empty/Unknown for every other entry.
  qint64 relatedInstanceId = 0;
  QString originalName;
  DetailOwnership ownership = DetailOwnership::Unknown;
  DetailCarryKind carryKind = DetailCarryKind::Unknown;
  int relationType = 0;
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
  // Number of entries the client reported in the candidate list, or -1 when the
  // field was absent or not a list. Counted without resolving each candidate.
  int carryCandidateCount = -1;
  bool summonCandidatesKnown = false, hasSummonCandidates = false;
  int summonCandidateCount = -1;
  bool officialRelationCandidates = false;
  QString relationCatalogProblem;
  DetailCarryKind carryKind = DetailCarryKind::Unknown;
  // Relation sections only describe pets that actually take part in a summon or
  // stargod-envoy contract. The client reports -1 (or 0) when there is none, so
  // an unrelated pet must not be shown an empty relation section at all. Stays
  // true while the detail itself is unconfirmed, because nothing is known yet.
  bool relationsApplicable = true;
  // Forward and reverse roles are independent observations. Show each role
  // only when public eligibility or a current relation establishes it.
  bool contractCarrierRole = false;
  bool contractEnvoyRole = false;
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
  QVector<DetailField> proficient;
  QVector<DetailField> equipment;
  QVector<DetailField> legendStone;
  QVector<DetailField> sacred;
  QVector<DetailPage> pages;
  quint64 chargedBytes = 0;
};
using PreparedPetDetailHandle = std::shared_ptr<const PreparedPetDetail>;

// Relation group labels are written by the preparation and read back by its
// renderer. They are shared here so the two sides cannot drift apart.
namespace DetailRelationGroup {
inline QString carriedEnvoy() { return QStringLiteral("已契约神使"); }
inline QString carrierOwner() { return QStringLiteral("被契约（携带者）"); }
inline QString carryCandidate() { return QStringLiteral("可契约候选"); }
inline QString summoned() { return QStringLiteral("已召唤精灵"); }
inline QString summonList() { return QStringLiteral("可召唤对象"); }
inline QString summoner() { return QStringLiteral("召唤者"); }
}

// Values are frozen by Core. The large payload and facts retain their original
// owners' leases; this service charges only its additional allocations.
struct DetailRelationInstance {
  qint64 instanceId = 0;
  int raceId = 0;
  DetailOwnership ownership = DetailOwnership::Unknown;
  bool operator==(const DetailRelationInstance& other) const {
    return instanceId == other.instanceId && raceId == other.raceId && ownership == other.ownership;
  }
};
struct DetailRelationRoster {
  QString account;
  quint64 epoch = 0, revision = 0;
  QHash<int,QVector<DetailRelationInstance>> byRace;
};
struct FrozenDetailInputs {
  RawPetRecordHandle raw;
  PetDerivedFactsHandle facts;
  QJsonObject brief;
  quint64 summaryRevision = 0;
  std::shared_ptr<const PetDetailCatalogSnapshot> metadata;
  std::shared_ptr<const PetSkillCatalogSnapshot> skills;
  std::shared_ptr<const DetailRelationRoster> relationRoster;
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
