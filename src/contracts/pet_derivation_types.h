#pragma once
#include "pet_record_types.h"
#include "domain/pet_analysis_facts.h"

struct PetDerivationKey {
  PetRecordKey record;
  quint64 metadataRevision = 0;
  QByteArray metadataDigest;
  int analysisVersion = AssetAnalysisVersion::kCurrentAnalysis;
  bool operator==(const PetDerivationKey& other) const {
    return record == other.record && metadataRevision == other.metadataRevision &&
        metadataDigest == other.metadataDigest && analysisVersion == other.analysisVersion;
  }
};
inline size_t qHash(const PetDerivationKey& key, size_t seed = 0) noexcept {
  return qHashMulti(seed, key.record, key.metadataRevision, key.metadataDigest, key.analysisVersion);
}
struct PetDerivedFactsRecord {
  // A handle also keeps its key allocation alive until after payload teardown.
  std::shared_ptr<void> memoryRetention;
  PetDerivationKey key;
  PetAnalysisFacts facts;
  quint64 chargedBytes = 0;
};
using PetDerivedFactsHandle = std::shared_ptr<const PetDerivedFactsRecord>;

Q_DECLARE_METATYPE(PetDerivationKey)
Q_DECLARE_METATYPE(PetDerivedFactsHandle)
