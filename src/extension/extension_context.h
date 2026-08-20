#pragma once

#include <QObject>
#include <QPointer>

class OriginalBridge;
class PetRefreshController;
class PetRepository;
class PetWindow;
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
  void showSettings();

private:
  explicit ExtensionContext(QObject* parent = nullptr);
  void positionButton();

  static ExtensionContext* instance_;
  OriginalBridge* bridge_ = nullptr;
  PetRefreshController* refreshController_ = nullptr;
  PetRepository* repository_ = nullptr;
  QPointer<QWidget> originalWindow_;
  QPointer<QPushButton> openButton_;
  QPointer<PetWindow> petWindow_;
};
