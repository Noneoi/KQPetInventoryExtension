#pragma once

#include "analysis_worker.h"
#include <QJsonObject>
#include <QStringList>

struct PerformanceCase {
  int pets = 2000;
  int goods = 200;
  bool actualCatalog = false;
  QString match = QStringLiteral("normal");
  int detailPercent = 100;
  int detailKiB = 8;
  int costItems = 3;
  int cultivationItems = 3;
  quint32 seed = 20260909;
  QString id() const;
  QJsonObject json() const;
  QString validationError() const;
};

struct PerformanceDataset {
  PerformanceCase spec;
  std::shared_ptr<ShopCatalogSnapshot> catalog;
  QList<ShopExchangeGood> goods;
  ShopPetMetadataSnapshot metadata;
  std::shared_ptr<const PetDetailCatalogSnapshot> metadataCatalog;
  QByteArray catalogContentDigest;
  QByteArray fixtureContentChainDigest;
  QJsonObject materials;
  QJsonObject shopPacket;
  QHash<QString, qint64> balances;
  QHash<int, int> matchingGoodsByRace;
  QList<int> selectedRaces;
  QList<PetAssetRecord> summaries;
  QList<int> detailedIndexes;
  QString directory;
  QString error;
  qint64 fixtureBytes = 0;
  qint64 fullDetailBytes = 0;
  qint64 fullDetailMinimumBytes = 0;
  qint64 fullDetailMaximumBytes = 0;
  qint64 durableFileBytes = 0;
  qint64 expectedPairs = 0;
  qint64 expectedQualifiedPairs = 0;
  qint64 raceAssociations = 0;
  int nextPet = 0;
  int minimumCandidates = 0;
  int maximumCandidates = 0;
  QJsonObject description() const;
};

QList<PerformanceCase> performanceMatrix();
std::shared_ptr<PerformanceDataset> createPerformanceDataset(const PerformanceCase& spec, const QString& directory);
// Fixture setup only, called on the existing I/O executor in bounded batches.
bool writePerformanceFixtureBatch(PerformanceDataset& data, int maximumPets = 64);
QJsonObject performanceFixturePet(const PerformanceDataset& data, int index);
AnalysisWorkInput performanceInputTemplate(const PerformanceDataset& data, const AnalysisJobKey& key);
QJsonObject pipelineStatistics(const AlgorithmPipelineStats& stats);
// Independent probes of the exact production pure APIs. These timings do not
// sum to Worker end-to-end time, and are never used as a substitute for it.
QJsonObject performancePhaseProbes(const AnalysisWorkInput& input, const QList<ShopExchangeGood>& goods);
