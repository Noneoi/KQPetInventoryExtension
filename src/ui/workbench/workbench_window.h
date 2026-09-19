#pragma once

#include "workbench_types.h"

#include <QMainWindow>
#include <QPointer>
#include <array>
#include <functional>

class QLabel;
class QProgressBar;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QScrollArea;
class QCloseEvent;
class QResizeEvent;
class QShowEvent;

class WorkbenchWindow final : public QMainWindow {
  Q_OBJECT
public:
  using PageFactory = std::function<QWidget*(WorkbenchPage, QWidget*)>;
  using TargetValidator = std::function<bool(const NavigationTarget&)>;
  using TargetHandler = std::function<bool(QWidget*, const NavigationTarget&)>;

  explicit WorkbenchWindow(PageFactory factory, QWidget* parent = nullptr,
                           const WorkbenchOptions& options = {});
  QWidget* page(WorkbenchPage page) const;
  WorkbenchPage currentPage() const { return currentPage_; }
  int creationCount(WorkbenchPage page) const;
  bool sidebarCollapsed() const { return collapsed_; }
  void setTargetValidator(TargetValidator validator);
  void setTargetHandler(TargetHandler handler);
  void setAvailableLogicalSize(const QSize& size);

public slots:
  bool showPage(WorkbenchPage page);
  bool navigate(const NavigationTarget& target);
  void focusSearch();
  void setSession(const WorkbenchSession& session);
  void setTask(const WorkbenchTask& task);
  void setPersistence(const WorkbenchPersistence& persistence);
  void setPageStatus(const QString& text);
  void setDetailTask(const QString& text, bool running, int completed, int total);

signals:
  void settingsRequested();
  void diagnosticsRequested();
  void pageChanged(WorkbenchPage page);
  void navigationRejected(const QString& reason);

protected:
  void closeEvent(QCloseEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;

private:
  QWidget* ensurePage(WorkbenchPage page);
  void updateLayoutMode();
  void updateSessionLabels();
  void constrainToScreen();
  void rejectNavigation(const QString& reason);
  void resetPage(QWidget* page);

  PageFactory factory_;
  TargetValidator validator_;
  TargetHandler targetHandler_;
  WorkbenchOptions options_;
  WorkbenchSession session_;
  quint64 sessionRevision_ = 0;
  WorkbenchPage currentPage_ = WorkbenchPage::Pets;
  std::array<QPointer<QWidget>, 4> pages_;
  std::array<QScrollArea*, 4> holders_{};
  std::array<int, 4> creations_{};
  std::array<QPushButton*, 4> navigation_{};
  QWidget* sidebar_ = nullptr;
  QStackedWidget* stack_ = nullptr;
  QLabel* account_ = nullptr;
  QLabel* source_ = nullptr;
  QLabel* version_ = nullptr;
  QLabel* notice_ = nullptr;
  QLabel* task_ = nullptr;
  QLabel* persistence_ = nullptr;
  QProgressBar* progress_ = nullptr;
  QLabel* detailTask_ = nullptr;
  QProgressBar* detailProgress_ = nullptr;
  QPlainTextEdit* log_ = nullptr;
  QString lastLog_;
  QPushButton* settings_ = nullptr;
  QPushButton* diagnostics_ = nullptr;
  bool collapsed_ = false;
};
