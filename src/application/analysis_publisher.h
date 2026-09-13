#pragma once
#include <QObject>
#include <memory>
class PetRepository;
class AssetAnalysisController;
class AnalysisProjection;
struct AnalysisViewSnapshot;

class AnalysisPublisher final : public QObject {
  Q_OBJECT
public:
  AnalysisPublisher(PetRepository* repository, AssetAnalysisController* controller,
                    AnalysisProjection* projection, QObject* parent = nullptr);
private:
  void schedule(bool history = false);
  void publish();
  PetRepository* repository_;
  AssetAnalysisController* controller_;
  AnalysisProjection* projection_;
  std::shared_ptr<const AnalysisViewSnapshot> last_;
  bool scheduled_ = false;
  bool historyDirty_ = true;
  bool running_ = false;
};
