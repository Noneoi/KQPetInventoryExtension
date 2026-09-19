#pragma once
#include "contracts/pet_detail_types.h"
#include <atomic>
#include <memory>

struct DetailPreparationLimits {
  quint64 workingBytes = 16ULL * 1024 * 1024;
  quint64 resultBytes = 1024ULL * 1024;
  quint64 expandedCarryResultBytes = 8ULL * 1024 * 1024;
  int maximumExpandedCarryItems = 16384;
  int maximumTextUnits = 32768;
  int sliceMilliseconds = 8;
  int sliceItems = 128;
};
enum class DetailStep { More, NeedSummaries, Complete, Cancelled, Failed };
struct DetailPreparationProgress {
  quint64 workItems = 0, retainedWorkingBytes = 0, resultBytes = 0;
  qint64 maximumSliceNanoseconds = 0, activeNanoseconds = 0;
};

// Pure resumable preparation. No QObject, Repository, runtime Catalog or I/O.
class PetDetailPreparation final {
public:
  PetDetailPreparation(FrozenDetailInputs inputs, DetailSection section = DetailSection::Overview,
                       int pageIndex = 0, DetailPreparationLimits limits = {});
  ~PetDetailPreparation();
  DetailStep step(const std::atomic_bool& cancelled);
  QVector<qint64> requestedSummaries() const;
  bool provideSummaries(QVector<DetailRelatedSummary> summaries);
  std::unique_ptr<PreparedPetDetail> takeResult();
  DetailPreparationProgress progress() const;
  QString error() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

quint64 preparedPetDetailRetainedBytes(const PreparedPetDetail& detail);
bool detailRelatedSummaryValid(const DetailRelatedSummary& summary, QString* error = nullptr);
