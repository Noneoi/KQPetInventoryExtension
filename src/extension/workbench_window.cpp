#include "workbench_window.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QCloseEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPalette>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QTime>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QShortcut>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTableView>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

namespace {

const std::array<QString, 4>& titles() {
  static const std::array<QString, 4> values{
      QStringLiteral("精灵"), QStringLiteral("商店"), QStringLiteral("日常"), QStringLiteral("资产")};
  return values;
}

int pageIndex(WorkbenchPage page) {
  const int value = static_cast<int>(page);
  return value >= 0 && value < 4 ? value : -1;
}

QIcon pageIcon(int page) {
  QIcon icon;
  for (int scale : {1, 2}) {
    QPixmap image(22 * scale, 22 * scale);
    image.setDevicePixelRatio(scale);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#35638d")), 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (page == 0) {
      painter.drawEllipse(QRectF(6, 10, 10, 8));
      for (const QPointF center : {QPointF(5, 8), QPointF(9, 5), QPointF(14, 5), QPointF(18, 8)})
        painter.drawEllipse(center, 1.5, 2);
    } else if (page == 1) {
      painter.drawRoundedRect(QRectF(4, 7, 14, 12), 2, 2);
      painter.drawArc(QRectF(8, 3, 6, 9), 0, 180 * 16);
    } else if (page == 2) {
      painter.drawRoundedRect(QRectF(3, 5, 16, 14), 2, 2);
      painter.drawLine(3, 9, 19, 9);
      painter.drawLine(7, 3, 7, 7);
      painter.drawLine(15, 3, 15, 7);
      painter.drawLine(7, 14, 10, 17);
      painter.drawLine(10, 17, 16, 12);
    } else {
      painter.drawLine(3, 19, 19, 19);
      painter.drawRect(QRectF(5, 11, 2, 6));
      painter.drawRect(QRectF(10, 7, 2, 10));
      painter.drawRect(QRectF(15, 3, 2, 14));
    }
    painter.end();
    icon.addPixmap(image);
  }
  return icon;
}

QLabel* plainLabel(QWidget* parent, const QString& objectName = {}) {
  auto* label = new QLabel(parent);
  label->setTextFormat(Qt::PlainText);
  if (!objectName.isEmpty()) label->setObjectName(objectName);
  return label;
}

}  // namespace

WorkbenchWindow::WorkbenchWindow(PageFactory factory, QWidget* parent, const WorkbenchOptions& options)
    : QMainWindow(parent), factory_(std::move(factory)), options_(options) {
  setObjectName(QStringLiteral("KQWorkbench"));
  setWindowTitle(QStringLiteral("精灵工作台"));
  setAttribute(Qt::WA_DeleteOnClose, false);
  // This auxiliary window must not participate in the host application's
  // last-primary-window decision. Closing the workbench itself still hides it.
  setAttribute(Qt::WA_QuitOnClose, false);
  setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 9));
  QPalette localPalette = palette();
  localPalette.setColor(QPalette::Window, QColor(QStringLiteral("#f4f7fb")));
  localPalette.setColor(QPalette::Base, Qt::white);
  localPalette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#f8fbff")));
  localPalette.setColor(QPalette::WindowText, QColor(QStringLiteral("#26374a")));
  localPalette.setColor(QPalette::Text, QColor(QStringLiteral("#26374a")));
  setPalette(localPalette);
  setStyleSheet(QStringLiteral(
      "#KQWorkbench{background:#f4f7fb;color:#203044;}"
      "#KQWorkbench QWidget{color:#26374a;}"
      "#KQWorkbench QPushButton{min-height:24px;padding:2px 10px;border:1px solid #d4dfeb;"
      "border-radius:5px;background:#ffffff;}"
      "#KQWorkbench QPushButton:hover{background:#edf4fc;border-color:#91b4da;}"
      "#KQWorkbench QPushButton:disabled{color:#95a2b2;background:#f4f6f8;}"
      "#KQWorkbench QPushButton:checked{background:#e3effc;border-color:#accbea;color:#174e89;font-weight:600;}"
      "#KQWorkbench QLineEdit,#KQWorkbench QComboBox{min-height:28px;border:1px solid #d6e0ea;"
      "border-radius:4px;background:white;padding:0 6px;}"
      "#KQWorkbench QTableView,#KQWorkbench QTreeView,#KQWorkbench QTextBrowser,"
      "#KQWorkbench QPlainTextEdit{background:white;border:1px solid #e0e6ee;gridline-color:#edf1f5;}"
      "#KQWorkbench QHeaderView::section{background:#f4f7fb;color:#53677f;border:0;"
      "border-bottom:1px solid #dbe4ee;padding:6px;font-weight:600;}"
      "#KQWorkbench QTabWidget::pane{border:1px solid #dfe7ef;background:white;}"
      "#KQWorkbench QTabBar::tab{min-height:28px;padding:2px 10px;background:#edf2f7;border:0;}"
      "#KQWorkbench QTabBar::tab:selected{background:white;color:#215d9a;}"
      "#KQWorkbench QProgressBar{max-height:7px;border:0;border-radius:3px;background:#e3eaf3;}"
      "#KQWorkbench QProgressBar::chunk{background:#397bc1;border-radius:3px;}"));

  auto* center = new QWidget(this);
  auto* outer = new QHBoxLayout(center);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  sidebar_ = new QWidget(center);
  sidebar_->setObjectName(QStringLiteral("KQWorkbenchSidebar"));
  sidebar_->setStyleSheet(QStringLiteral("#KQWorkbenchSidebar{background:#eef3f9;border-right:1px solid #dce5f0;}"));
  auto* side = new QVBoxLayout(sidebar_);
  side->setContentsMargins(12, 18, 12, 14);
  side->setSpacing(8);
  auto* brand = plainLabel(sidebar_);
  brand->setText(QStringLiteral("KQ"));
  brand->setAlignment(Qt::AlignCenter);
  brand->setStyleSheet(QStringLiteral("font-size:22px;font-weight:700;color:#326496;padding:8px 0;"));
  side->addWidget(brand);
  side->addSpacing(18);
  for (int index = 0; index < 4; ++index) {
    auto* button = new QPushButton(titles()[index], sidebar_);
    button->setObjectName(QStringLiteral("KQWorkbenchPage%1").arg(index));
    button->setIcon(pageIcon(index));
    button->setIconSize(QSize(22, 22));
    button->setCheckable(true);
    button->setMinimumHeight(40);
    button->setAccessibleName(titles()[index]);
    button->setToolTip(QStringLiteral("%1 · Ctrl+%2").arg(titles()[index]).arg(index + 1));
    navigation_[index] = button;
    side->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, index] { showPage(static_cast<WorkbenchPage>(index)); });
    auto* shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(index + 1)), this);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, this, [this, index] {
      if (isActiveWindow()) showPage(static_cast<WorkbenchPage>(index));
    });
  }
  auto* infoScroll = new QScrollArea(sidebar_);
  infoScroll->setObjectName(QStringLiteral("KQWorkbenchInfoScroll"));
  infoScroll->setFrameShape(QFrame::NoFrame);
  infoScroll->setWidgetResizable(true);
  infoScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  infoScroll->setMinimumWidth(0);
  auto* info = new QWidget(infoScroll);
  info->setObjectName(QStringLiteral("KQWorkbenchInfo"));
  auto* infoLayout = new QVBoxLayout(info);
  infoLayout->setContentsMargins(0, 12, 0, 0);
  infoLayout->setSpacing(10);
  account_ = plainLabel(info, QStringLiteral("KQWorkbenchAccount"));
  source_ = plainLabel(info, QStringLiteral("KQWorkbenchSource"));
  version_ = plainLabel(info, QStringLiteral("KQWorkbenchVersion"));
  persistence_ = plainLabel(info, QStringLiteral("KQWorkbenchPersistence"));
  task_ = plainLabel(info, QStringLiteral("KQWorkbenchTask"));
  detailTask_ = plainLabel(info, QStringLiteral("KQWorkbenchDetailTask"));
  notice_ = plainLabel(info, QStringLiteral("KQWorkbenchNotice"));
  for (QLabel* label : {account_, version_, source_, persistence_, task_, detailTask_, notice_}) {
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    infoLayout->addWidget(label);
  }
  version_->setStyleSheet(QStringLiteral("color:#7b8c9f;font-size:11px;"));
  notice_->setStyleSheet(QStringLiteral("color:#a0601d;"));
  notice_->hide();
  detailTask_->hide();
  progress_ = new QProgressBar(info);
  detailProgress_ = new QProgressBar(info);
  for (QProgressBar* bar : {progress_, detailProgress_}) {
    bar->setTextVisible(false);
    bar->hide();
    infoLayout->addWidget(bar);
  }
  auto* logTitle = plainLabel(info);
  logTitle->setText(QStringLiteral("最近状态"));
  logTitle->setStyleSheet(QStringLiteral("color:#7b8c9f;font-size:11px;"));
  infoLayout->addWidget(logTitle);
  log_ = new QPlainTextEdit(info);
  log_->setObjectName(QStringLiteral("KQWorkbenchStatusLog"));
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(20);
  log_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  log_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  log_->setMinimumSize(0, 90);
  log_->setMaximumHeight(210);
  log_->setStyleSheet(QStringLiteral("background:transparent;border:0;font-size:11px;color:#65788f;"));
  infoLayout->addWidget(log_, 1);
  infoLayout->addStretch(1);
  infoScroll->setWidget(info);
  side->addWidget(infoScroll, 1);
  settings_ = new QPushButton(QStringLiteral("设置"), sidebar_);
  diagnostics_ = new QPushButton(QStringLiteral("诊断"), sidebar_);
  settings_->setObjectName(QStringLiteral("KQWorkbenchSettings"));
  diagnostics_->setObjectName(QStringLiteral("KQWorkbenchDiagnostics"));
  settings_->setAccessibleName(QStringLiteral("设置"));
  diagnostics_->setAccessibleName(QStringLiteral("诊断"));
  side->addWidget(settings_);
  side->addWidget(diagnostics_);
  connect(settings_, &QPushButton::clicked, this, &WorkbenchWindow::settingsRequested);
  connect(diagnostics_, &QPushButton::clicked, this, &WorkbenchWindow::diagnosticsRequested);
  outer->addWidget(sidebar_);

  auto* content = new QWidget(center);
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(12, 10, 12, 10);
  layout->setSpacing(0);
  stack_ = new QStackedWidget(content);
  stack_->setObjectName(QStringLiteral("KQWorkbenchStack"));
  stack_->setMinimumSize(0, 0);
  layout->addWidget(stack_, 1);
  outer->addWidget(content, 1);
  setCentralWidget(center);
  auto* searchShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+F")), this);
  searchShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(searchShortcut, &QShortcut::activated, this, [this] { if (isActiveWindow()) focusSearch(); });
  constrainToScreen();
  const QSize available = options_.availableLogicalSize.isValid() ? options_.availableLogicalSize
      : screen() ? screen()->availableGeometry().size() : QSize(1280, 800);
  resize(options_.preferredSize.boundedTo(available - QSize(16, 16)));
  setSession({});
  setTask({});
  setPersistence({});
  updateLayoutMode();
}

void WorkbenchWindow::constrainToScreen() {
  const QSize available = options_.availableLogicalSize.isValid() ? options_.availableLogicalSize
      : screen() ? screen()->availableGeometry().size() : QSize(1280, 800);
  const QMargins frame = windowHandle() ? windowHandle()->frameMargins() : QMargins(8, 32, 8, 8);
  const QSize usable(qMax(1, available.width() - frame.left() - frame.right() - 8),
                     qMax(1, available.height() - frame.top() - frame.bottom() - 8));
  setMinimumSize(qMin(760, usable.width()), qMin(520, usable.height()));
  if (size().width() > usable.width() || size().height() > usable.height()) resize(size().boundedTo(usable));
}

void WorkbenchWindow::setAvailableLogicalSize(const QSize& size) {
  options_.availableLogicalSize = size;
  constrainToScreen();
  updateLayoutMode();
}

QWidget* WorkbenchWindow::page(WorkbenchPage page) const {
  const int index = pageIndex(page);
  return index >= 0 ? pages_[index].data() : nullptr;
}
int WorkbenchWindow::creationCount(WorkbenchPage page) const {
  const int index = pageIndex(page);
  return index >= 0 ? creations_[index] : 0;
}
void WorkbenchWindow::setTargetValidator(TargetValidator value) { validator_ = std::move(value); }
void WorkbenchWindow::setTargetHandler(TargetHandler value) { targetHandler_ = std::move(value); }
void WorkbenchWindow::setSearchHandler(PageHandler value) { searchHandler_ = std::move(value); }
void WorkbenchWindow::setSessionResetHandler(PageHandler value) { resetHandler_ = std::move(value); }

QWidget* WorkbenchWindow::ensurePage(WorkbenchPage page) {
  const int index = pageIndex(page);
  if (index < 0) return nullptr;
  if (pages_[index]) return pages_[index];
  if (creations_[index] || !factory_) return nullptr;
  auto* holder = new QScrollArea(stack_);
  holder->setFrameShape(QFrame::NoFrame);
  holder->setWidgetResizable(true);
  holder->setMinimumSize(0, 0);
  holder->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  QWidget* created = nullptr;
  try { created = factory_(page, holder->viewport()); }
  catch (...) { created = nullptr; }
  if (!created) { delete holder; return nullptr; }
  ++creations_[index];
  if (created->isWindow()) created->setWindowFlags(Qt::Widget);
  created->setAttribute(Qt::WA_DeleteOnClose, false);
  created->setPalette(palette());
  created->setAutoFillBackground(true);
  created->setMinimumSize(0, 0);
  created->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  holder->setWidget(created);
  holders_[index] = holder;
  pages_[index] = created;
  stack_->addWidget(holder);
  for (QAbstractButton* button : created->findChildren<QAbstractButton*>()) {
    if (button->maximumHeight() < 28) button->setMaximumHeight(QWIDGETSIZE_MAX);
    button->setMinimumHeight(28);
  }
  for (QTableView* table : created->findChildren<QTableView*>()) {
    table->verticalHeader()->setMinimumSectionSize(28);
    table->verticalHeader()->setDefaultSectionSize(qMax(28, table->verticalHeader()->defaultSectionSize()));
  }
  if (created->metaObject()->indexOfMethod("setWorkbenchMode(bool,bool)") >= 0)
    QMetaObject::invokeMethod(created, "setWorkbenchMode", Qt::DirectConnection,
                              Q_ARG(bool, true), Q_ARG(bool, collapsed_));
  return created;
}

bool WorkbenchWindow::showPage(WorkbenchPage requested) {
  Q_ASSERT(QThread::currentThread() == thread());
  QWidget* created = ensurePage(requested);
  if (!created) { rejectNavigation(QStringLiteral("该页面暂时不可用，请查看诊断。")); return false; }
  const int index = pageIndex(requested);
  currentPage_ = requested;
  stack_->setCurrentWidget(holders_[index]);
  for (int page = 0; page < 4; ++page) navigation_[page]->setChecked(page == index);
  notice_->hide();
  emit pageChanged(requested);
  return true;
}

void WorkbenchWindow::rejectNavigation(const QString& reason) {
  notice_->setText(reason);
  notice_->show();
  emit navigationRejected(reason);
}

bool WorkbenchWindow::navigate(const NavigationTarget& target) {
  Q_ASSERT(QThread::currentThread() == thread());
  const quint64 revision = sessionRevision_;
  if (target.account != session_.account || target.epoch != session_.epoch ||
      target.account.isEmpty()) {
    rejectNavigation(QStringLiteral("无法定位：目标属于另一账号或已结束的会话。"));
    return false;
  }
  if ((target.page == WorkbenchPage::Pets && target.petInstanceId <= 0) ||
      (target.page == WorkbenchPage::Shop && target.goodKey.isEmpty()) ||
      !validator_ || !validator_(target)) {
    rejectNavigation(QStringLiteral("无法定位：该精灵或商品已不在当前账号的数据中。"));
    return false;
  }
  QWidget* targetPage = ensurePage(target.page);
  if (!targetPage || revision != sessionRevision_ || !targetHandler_ ||
      !targetHandler_(targetPage, target) || revision != sessionRevision_) {
    if (targetPage && revision != sessionRevision_) resetPage(targetPage);
    rejectNavigation(QStringLiteral("当前数据已变化，或该页面暂不支持定位此目标。"));
    return false;
  }
  return showPage(target.page);
}

void WorkbenchWindow::focusSearch() {
  QWidget* current = page(currentPage_);
  if (!current) current = ensurePage(currentPage_);
  if (!current) return;
  if (searchHandler_) { searchHandler_(current); return; }
  for (QLineEdit* search : current->findChildren<QLineEdit*>()) {
    if (search->isVisible() && search->isEnabled()) { search->setFocus(); search->selectAll(); return; }
  }
}

void WorkbenchWindow::resetPage(QWidget* page) {
  if (resetHandler_) { resetHandler_(page); return; }
  if (page->metaObject()->indexOfMethod("resetSessionContext()") >= 0) {
    QMetaObject::invokeMethod(page, "resetSessionContext", Qt::DirectConnection);
    return;
  }
  for (QAbstractItemView* view : page->findChildren<QAbstractItemView*>()) {
    view->clearSelection();
    if (view->selectionModel()) view->selectionModel()->clearCurrentIndex();
  }
}

void WorkbenchWindow::setSession(const WorkbenchSession& session) {
  Q_ASSERT(QThread::currentThread() == thread());
  const bool changed = session_.account != session.account || session_.epoch != session.epoch;
  session_ = session;
  if (changed) {
    ++sessionRevision_;
    notice_->hide();
    for (const auto& page : pages_) if (page) resetPage(page);
    setTask({});
    setPersistence({});
    setDetailTask({}, false, 0, 0);
    log_->clear();
    lastLog_.clear();
  }
  updateSessionLabels();
}
void WorkbenchWindow::updateSessionLabels() {
  const QString account = session_.account.isEmpty() ? QStringLiteral("未选择账号")
      : QStringLiteral("账号  %1").arg(session_.account);
  const QString source = session_.source.isEmpty() ? QStringLiteral("等待来源状态")
      : session_.source;
  account_->setText(account);
  source_->setText(source);
  account_->setToolTip(account);
  source_->setToolTip(source + (session_.sourceVerified ? QString{}
      : QStringLiteral("\n连接来源尚未核验；新读取的数据仅供本次查看，不提交精灵移动操作。")));
  source_->setStyleSheet(session_.sourceVerified ? QStringLiteral("color:#28745b;") : QStringLiteral("color:#9b742b;"));
  version_->setText(session_.clientVersion.isEmpty() ? QStringLiteral("客户端未确认")
      : QStringLiteral("氪奇 %1").arg(session_.clientVersion));
}

void WorkbenchWindow::setTask(const WorkbenchTask& task) {
  Q_ASSERT(QThread::currentThread() == thread());
  task_->setText(task.text.isEmpty() ? QStringLiteral("任务 · 就绪") : task.text);
  task_->setToolTip(task.text);
  if (!task.text.isEmpty() && !task.running) setPageStatus(task.text);
  progress_->setVisible(task.running);
  progress_->setRange(0, task.total > 0 ? task.total : 0);
  if (task.total > 0) progress_->setValue(qBound(0, task.completed, task.total));
}

void WorkbenchWindow::setPageStatus(const QString& text) {
  if (text.isEmpty() || text == lastLog_) return;
  lastLog_ = text;
  log_->appendPlainText(QTime::currentTime().toString(QStringLiteral("HH:mm ")) + text);
}

void WorkbenchWindow::setDetailTask(const QString& text, bool running, int completed, int total) {
  detailTask_->setText(text);
  detailTask_->setVisible(total > 0 || running);
  detailProgress_->setVisible(running);
  detailProgress_->setRange(0, qMax(0, total));
  if (total > 0) detailProgress_->setValue(qBound(0, completed, total));
}

void WorkbenchWindow::setPersistence(const WorkbenchPersistence& state) {
  Q_ASSERT(QThread::currentThread() == thread());
  QString text;
  switch (state.state) {
    case PersistenceState::Unknown: text = QStringLiteral("保存 · 待确认"); break;
    case PersistenceState::Pending: text = QStringLiteral("保存 · 内存已更新，等待写入"); break;
    case PersistenceState::Saved: text = QStringLiteral("保存 · 已保存"); break;
    case PersistenceState::Failed: text = QStringLiteral("保存失败 · 查看诊断"); break;
  }
  persistence_->setText(text);
  persistence_->setToolTip(state.detail);
  persistence_->setStyleSheet(state.state == PersistenceState::Failed
      ? QStringLiteral("color:#b34236;") : QStringLiteral("color:#75869a;"));
}

void WorkbenchWindow::updateLayoutMode() {
  collapsed_ = width() < 1080;
  sidebar_->setFixedWidth(collapsed_ ? 146 : 180);
  for (int index = 0; index < 4; ++index) {
    navigation_[index]->setText(collapsed_ ? QString{} : titles()[index]);
    navigation_[index]->setStyleSheet(collapsed_ ? QStringLiteral("padding:3px 4px;") : QString{});
  }
  settings_->setText(collapsed_ ? QStringLiteral("设") : QStringLiteral("设置"));
  diagnostics_->setText(collapsed_ ? QStringLiteral("诊") : QStringLiteral("诊断"));
  settings_->setToolTip(QStringLiteral("设置"));
  diagnostics_->setToolTip(QStringLiteral("诊断"));
  for (const auto& page : pages_) {
    if (page && page->metaObject()->indexOfMethod("setWorkbenchMode(bool,bool)") >= 0)
      QMetaObject::invokeMethod(page, "setWorkbenchMode", Qt::DirectConnection,
                                Q_ARG(bool, true), Q_ARG(bool, collapsed_));
  }
  updateSessionLabels();
}

void WorkbenchWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  if (sidebar_) updateLayoutMode();
}
void WorkbenchWindow::showEvent(QShowEvent* event) {
  QMainWindow::showEvent(event);
  constrainToScreen();
  if (!page(currentPage_)) showPage(currentPage_);
}
void WorkbenchWindow::closeEvent(QCloseEvent* event) { event->ignore(); hide(); }
