#pragma once

#include "inventory_read_view.h"
#include "../domain/asset_derivation.h"

namespace PetFactsUi {

// This read-side check uses already validated, immutable facts. It never
// derives cultivation or loads raw JSON. A frozen model batch supplies its
// exact metadata snapshot so late facts cannot mix metadata generations.
inline PetDerivedFactsHandle current(const InventoryReadView* view, qint64 id,
    const std::shared_ptr<const PetDetailCatalogSnapshot>& frozenMetadata = {}) {
  if (!view || id <= 0) return {};
  const auto version = view->recordVersion(id);
  const auto metadata = frozenMetadata ? frozenMetadata : view->metadataSnapshot();
  const auto facts = view->derivedFactsFor(id);
  if (!version.valid() || !version.complete || version.key.account != view->accountKey() ||
      !metadata || !facts || facts->key.record != version.key || facts->key.record.instanceId != id ||
      facts->key.metadataRevision != metadata->revision || facts->key.metadataDigest != metadata->contentDigest ||
      facts->key.analysisVersion != AssetAnalysisVersion::kCurrentAnalysis ||
      facts->facts.analysisVersion != AssetAnalysisVersion::kCurrentAnalysis ||
      facts->facts.asset.instanceId != id || !facts->facts.asset.detailAvailable ||
      !facts->facts.eligibility.hasFullDetail) return {};
  return facts;
}

// Rows own a small display record even when a legacy caller provides raw
// detail. Selected original JSON remains owned by its RawPetRecordHandle.
inline QJsonObject rowSummary(const QJsonObject& value) {
  auto summary = AssetDerivation::identityFields(value);
  for (const QString& key : {QStringLiteral("rt"),QStringLiteral("zdl"),QStringLiteral("xzdl"),
      QStringLiteral("_inFormation"),QStringLiteral("inFormation"),QStringLiteral("isInFormation"),
      QStringLiteral("inTeam"),QStringLiteral("isTeamPet")}) {
    const auto field = value.value(key);
    if (field.isBool() || field.isDouble() || field.isString()) summary.insert(key,field);
  }
  return summary;
}
}
