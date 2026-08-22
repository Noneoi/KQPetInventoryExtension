#pragma once

#include <QObject>
#include <QList>
#include <QPointer>

class OriginalBridge;
class PetRefreshController;
class PetRepository;
class PetWindow;
class ShopExchangeController;
class ShopWindow;
class RoutineOverviewController;
class RoutineOverviewWindow;
class QPushButton;
class QWidget;

class ExtensionContext final : public QObject {
  Q_OBJECT

public:
  static void start();

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
  void attachToOriginalWindow();
  void showPetWindow();
  void showShopWindow();
  void showRoutineWindow();
  void showSettings();

private:
  enum class MoveUiOrigin { None, PetWindow, ShopWindow };
  explicit ExtensionContext(QObject* parent = nullptr);
  void positionButton();
  void routeMoveReplacement(qint64 incomingInstanceId,
                            const QList<qint64>& eligibleBackpackIds);

  static ExtensionContext* instance_;
  OriginalBridge* bridge_ = nullptr;
  PetRefreshController* refreshController_ = nullptr;
  ShopExchangeController* shopController_ = nullptr;
  RoutineOverviewController* routineController_ = nullptr;
  PetRepository* repository_ = nullptr;
  QPointer<QWidget> originalWindow_;
  QPointer<QPushButton> openButton_;
  QPointer<QPushButton> shopButton_;
  QPointer<QPushButton> routineButton_;
  QPointer<PetWindow> petWindow_;
  QPointer<ShopWindow> shopWindow_;
  QPointer<RoutineOverviewWindow> routineWindow_;
  MoveUiOrigin moveUiOrigin_ = MoveUiOrigin::None;
};
