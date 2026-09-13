#pragma once
#include "../domain/pet_detail_preparation.h"
#include <QObject>
#include <QSet>
#include <functional>
#include <memory>

struct DetailServiceLimits {
  DetailPreparationLimits preparation;
  quint64 retainedResultBytes = 32ULL * 1024 * 1024;
  quint64 descriptorBytes = 16ULL * 1024;
};
struct DetailServiceStats {
  int selectedConsumers = 0, activeTasks = 0, waitingDescriptors = 0;
  quint64 workingBytes = 0, retainedResultBytes = 0, reservedResultBytes = 0;
  quint64 peakRetainedBytes = 0, liveResultLeases = 0, completedTasks = 0, cancelledTasks = 0;
  quint64 postedSlices = 0, workItems = 0;
  qint64 completedComputeNanoseconds = 0, maximumSliceNanoseconds = 0;
  bool closing = false;
};

namespace DetailServiceInternal { struct State; struct Completion; }
class PetDetailPreparationService final : public QObject {
  Q_OBJECT
public:
  using ComputeExecutor = std::function<bool(std::function<void()>)>;
  explicit PetDetailPreparationService(ComputeExecutor executor, DetailServiceLimits limits = {}, QObject* parent = nullptr);
  ~PetDetailPreparationService() override;
  DetailSubmission request(int consumer, const DetailSelection& selection);
  DetailSubmission requestPage(int consumer, DetailSection section, int pageIndex);
  void provideInputs(quint64 requestId, FrozenDetailInputs inputs);
  void rejectInputs(quint64 requestId, const QString& reason);
  void provideRelatedSummaries(quint64 requestId, QVector<DetailRelatedSummary> summaries);
  void invalidateMetadata(quint64 revision, const QByteArray& digest);
  void summariesChanged(const QSet<qint64>& ids);
  void release(int consumer);
  void bindSession(const QString& account, quint64 epoch);
  bool shutdown();
  DetailServiceStats stats() const;
signals:
  void inputsNeeded(quint64 requestId, int consumer, const DetailSelection& selection);
  void relatedSummariesNeeded(quint64 requestId, const QVector<qint64>& ids);
  void ready(int consumer, quint64 requestId, const PreparedPetDetailHandle& detail);
  void failed(int consumer, quint64 requestId, DetailPreparationStatus status, const QString& error);
  void stateChanged();
private:
  friend struct DetailServiceInternal::State;
  void pump();
  void receive(std::shared_ptr<DetailServiceInternal::Completion> completion);
  void releasedBudget();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
