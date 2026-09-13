#pragma once

#include <QObject>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QWidget>


// Tracks the client's real main window across delayed creation and replacement.
// It never shows or activates the host and does not access account state.
class OriginalWindowLocator final : public QObject {
  Q_OBJECT

 public:
  explicit OriginalWindowLocator(QObject* parent = nullptr);
  QWidget* window() const { return window_.data(); }
  static QWidget* selectMainWindow(const QWidgetList& windows);
  static QRect singleEntryGeometry(QWidget* window);
  void start();

 public slots:
  void refresh();

 signals:
  void windowChanged(QWidget* window);
  void layoutNeeded();

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void queueLayout();
  QPointer<QWidget> window_;
  QTimer retryTimer_;
  QMetaObject::Connection destroyedConnection_;
  bool layoutQueued_ = false;
  bool stopping_ = false;
};
