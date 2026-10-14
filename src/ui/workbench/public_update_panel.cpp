#include "public_update_panel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextEdit>
#include <QVBoxLayout>
#include <limits>

namespace {
const QList<QPair<QString, QString>>& parts() {
  static const QList<QPair<QString, QString>> values{
      {QStringLiteral("pets"), QStringLiteral("精灵与养成")},
      {QStringLiteral("skills"), QStringLiteral("技能资料")},
      {QStringLiteral("battleReplay"), QStringLiteral("回放资料")},
      {QStringLiteral("shop"), QStringLiteral("兑换商店")},
      {QStringLiteral("images"), QStringLiteral("精灵图片")},
      {QStringLiteral("icons"), QStringLiteral("公共图标")},
      {QStringLiteral("routines"), QStringLiteral("日常与活动")}};
  return values;
}
QLabel* label(const QString& text, QWidget* parent) {
  auto* value = new QLabel(text, parent);
  value->setTextFormat(Qt::PlainText);
  value->setWordWrap(true);
  return value;
}
QPushButton* action(const QString& text, const QString& name, QWidget* parent) {
  auto* value = new QPushButton(text, parent);
  value->setObjectName(name); value->setAutoDefault(false);
  value->setMinimumHeight(30);
  return value;
}
QString componentStatus(const PublicUpdateComponentState& component) {
  if (component.status == QStringLiteral("retained")) return QStringLiteral("沿用原资料");
  if (component.status == QStringLiteral("failed")) return QStringLiteral("未完成");
  if (component.status == QStringLiteral("updated")) return QStringLiteral("已准备更新");
  if (component.status == QStringLiteral("unchanged")) return QStringLiteral("内容未变化");
  return QStringLiteral("本次未检查");
}
QString changeKind(const QString& kind) {
  if (kind == QStringLiteral("added")) return QStringLiteral("新增");
  if (kind == QStringLiteral("removed")) return QStringLiteral("已移除或停止显示");
  return QStringLiteral("内容修订");
}
QString displayText(const QString& value) {
  QTextDocument document;
  document.setHtml(value);
  return document.toPlainText();
}
QString paragraph(const QString& value) {
  return displayText(value).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
}
QString dependencyText(const QString& component) {
  if (component == QStringLiteral("routines"))
    return QStringLiteral("日常与活动使用独立官方模块，只检查这一组资料。");
  return QStringLiteral("这些资料共用精灵定义和明确关联，将同时检查精灵、技能、商店、回放、图片及图标。图片问题不会阻止已通过检查的核心资料应用。");
}
}

PublicUpdatePanel::PublicUpdatePanel(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("KQPublicUpdatePanel"));
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(10);
  auto* card = new QFrame(this);
  card->setObjectName(QStringLiteral("KQPublicUpdateCard"));
  card->setStyleSheet(QStringLiteral(
      "QFrame#KQPublicUpdateCard { background: palette(base); border: 1px solid #cbd9eb; border-radius: 10px; }"
      "QLabel#KQPublicUpdateTitle { font-size: 17px; font-weight: 600; }"
      "QPushButton#KQCheckDataUpdate { background: #2368b8; color: white; border: 0; border-radius: 6px; padding: 6px 18px; font-weight: 600; }"
      "QPushButton#KQCheckDataUpdate:disabled { background: #8ba5c2; }"));
  auto* content = new QVBoxLayout(card); content->setContentsMargins(16, 14, 16, 14);
  title_ = label(QStringLiteral("共享官方资料"), card);
  title_->setObjectName(QStringLiteral("KQPublicUpdateTitle"));
  content->addWidget(title_);
  summary_ = label(QStringLiteral("正在读取最近检查与当前资料…"), card);
  summary_->setObjectName(QStringLiteral("KQDataUpdateStatus"));
  summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  content->addWidget(summary_);
  operationNotice_ = label({}, card);
  operationNotice_->setObjectName(QStringLiteral("KQPublicUpdateNotice"));
  operationNotice_->setStyleSheet(QStringLiteral("color:#8d580b;"));
  operationNotice_->hide(); content->addWidget(operationNotice_);
  auto* facts = new QHBoxLayout;
  checked_ = label(QStringLiteral("最近检查：正在读取"), card);
  checked_->setObjectName(QStringLiteral("KQLastPublicCheck"));
  applied_ = label(QStringLiteral("当前资料：正在读取"), card);
  applied_->setObjectName(QStringLiteral("KQAppliedPublicData"));
  facts->addWidget(checked_, 1); facts->addWidget(applied_, 1);
  content->addLayout(facts);
  auto* mainActions = new QHBoxLayout;
  check_ = action(QStringLiteral("检查并更新"), QStringLiteral("KQCheckDataUpdate"), card);
  check_->setToolTip(QStringLiteral("使用同一个共享资料集检查全部七类官方资料，通过完整检查后一起应用"));
  retry_ = action(QStringLiteral("重试未完成项"), QStringLiteral("KQRetryPublicData"), card);
  cancel_ = action(QStringLiteral("取消检查"), QStringLiteral("KQCancelPublicData"), card);
  reload_ = action(QStringLiteral("载入已保存资料"), QStringLiteral("KQReloadPublicData"), card);
  mainActions->addWidget(check_); mainActions->addWidget(retry_);
  mainActions->addWidget(reload_); mainActions->addStretch(); mainActions->addWidget(cancel_);
  content->addLayout(mainActions);
  connect(check_, &QPushButton::clicked, this, [this] { emit checkRequested({}); });
  connect(retry_, &QPushButton::clicked, this, &PublicUpdatePanel::retryRequested);
  connect(cancel_, &QPushButton::clicked, this, &PublicUpdatePanel::cancelRequested);
  connect(reload_, &QPushButton::clicked, this, &PublicUpdatePanel::reloadRequested);

  progressArea_ = new QWidget(card);
  auto* progressLayout = new QVBoxLayout(progressArea_); progressLayout->setContentsMargins(0, 4, 0, 4);
  auto* stepRow = new QHBoxLayout;
  for (const auto& title : {QStringLiteral("准备工具"), QStringLiteral("获取资源"), QStringLiteral("解析资料"),
                           QStringLiteral("检查资料"), QStringLiteral("应用版本")}) {
    auto* step = label(title, progressArea_); step->setAlignment(Qt::AlignCenter);
    steps_.append(step); stepRow->addWidget(step, 1);
  }
  progressLayout->addLayout(stepRow);
  progressText_ = label({}, progressArea_); progressText_->setObjectName(QStringLiteral("KQPublicUpdateProgressText"));
  progressLayout->addWidget(progressText_);
  progress_ = new QProgressBar(progressArea_); progress_->setMaximumHeight(18);
  progress_->setObjectName(QStringLiteral("KQPublicUpdateProgress"));
  progressLayout->addWidget(progress_); content->addWidget(progressArea_);
  auto* statuses = new QGridLayout;
  int index = 0;
  for (const auto& part : parts()) {
    auto* value = label(part.second + QStringLiteral(" · 等待读取"), card);
    value->setObjectName(QStringLiteral("KQPublicComponent-%1").arg(part.first));
    value->setContentsMargins(4, 3, 4, 3);
    components_.insert(part.first, value); statuses->addWidget(value, index / 3, index % 3); ++index;
  }
  content->addLayout(statuses);
  layout->addWidget(card);
  auto* secondary = new QHBoxLayout;
  changes_ = action(QStringLiteral("查看变化"), QStringLiteral("KQPublicDataChanges"), this);
  restore_ = action(QStringLiteral("恢复上一份资料"), QStringLiteral("KQRestorePublicData"), this);
  auto* details = action(QStringLiteral("结果详情"), QStringLiteral("KQPublicDataDetails"), this);
  export_ = action(QStringLiteral("导出诊断"), QStringLiteral("KQExportPublicDataDiagnostics"), this);
  auto* advanced = action(QStringLiteral("高级更新 ▾"), QStringLiteral("KQAdvancedPublicData"), this);
  advanced->setCheckable(true);
  secondary->addWidget(changes_); secondary->addWidget(details); secondary->addWidget(restore_);
  secondary->addStretch(); secondary->addWidget(export_); secondary->addWidget(advanced);
  layout->addLayout(secondary);
  connect(changes_, &QPushButton::clicked, this, &PublicUpdatePanel::showChanges);
  connect(details, &QPushButton::clicked, this, &PublicUpdatePanel::showDetails);
  connect(restore_, &QPushButton::clicked, this, &PublicUpdatePanel::restoreRequested);
  connect(export_, &QPushButton::clicked, this, &PublicUpdatePanel::diagnosticsRequested);
  advanced_ = new QWidget(this);
  auto* advancedLayout = new QVBoxLayout(advanced_); advancedLayout->setContentsMargins(4, 3, 4, 3);
  dependency_ = label(QStringLiteral("单独检查时会同步它需要的关联资料；七类资料始终作为完整版本使用。"), advanced_);
  advancedLayout->addWidget(dependency_);
  auto* partial = new QGridLayout;
  index = 0;
  for (const auto& part : parts()) {
    auto* value = action(part.second, QStringLiteral("KQDataUpdate-%1").arg(part.first), advanced_);
    value->setToolTip(dependencyText(part.first));
    partial->addWidget(value, index / 4, index % 4); ++index; partial_.append(value);
    connect(value, &QPushButton::clicked, this, [this, key = part.first] {
      dependency_->setText(dependencyText(key)); emit checkRequested({key});
    });
  }
  advancedLayout->addLayout(partial); layout->addWidget(advanced_); advanced_->hide();
  connect(advanced, &QPushButton::toggled, this, [this, advanced](bool visible) {
    advanced_->setVisible(visible); advanced->setText(visible ? QStringLiteral("高级更新 ▴") : QStringLiteral("高级更新 ▾"));
  });
  render();
}

void PublicUpdatePanel::setState(const PublicUpdateState& state) {
  state_ = state; hasState_ = true; externalDataBusy_ = state.busy;
  operationNotice_->clear(); operationNotice_->hide(); render();
}
void PublicUpdatePanel::setExternalBusy(bool dataBusy, bool imagesBusy) {
  externalDataBusy_ = dataBusy; imagesBusy_ = imagesBusy; render();
}
void PublicUpdatePanel::setLegacyStatus(const QString& message, bool busy) {
  legacyStatus_ = message; externalDataBusy_ = busy;
  // Admission/queue failures have no transaction receipt. Keep their feedback
  // visible without replacing the latest durable result or exposing raw paths.
  static const QRegularExpression technical(QStringLiteral("[A-Za-z]:[\\\\/]|[a-f0-9]{32,}|\\b[A-Za-z_]+\\.(?:json|py|exe|dll)\\b"));
  operationNotice_->setText(technical.match(message).hasMatch()
      ? QStringLiteral("本次操作未完全完成，现有资料保留。技术原因可在结果详情或诊断中查看。") : message);
  operationNotice_->setVisible(hasState_ && !busy && !message.isEmpty()); render();
}
void PublicUpdatePanel::render() {
  const bool checking = externalDataBusy_ || state_.busy;
  const bool blocked = checking || imagesBusy_;
  QString headline = QStringLiteral("共享官方资料");
  if (!state_.readable) headline = QStringLiteral("暂时无法读取检查记录");
  else if (checking) headline = QStringLiteral("正在检查并更新");
  else switch (state_.outcome) {
    case PublicUpdateOutcome::Unchanged: headline = QStringLiteral("资料已是当前版本"); break;
    case PublicUpdateOutcome::Applied: headline = QStringLiteral("资料更新完成"); break;
    case PublicUpdateOutcome::Partial: headline = QStringLiteral("检查完成，部分内容需留意"); break;
    case PublicUpdateOutcome::KeptPrevious: headline = QStringLiteral("本次未完成，保留原资料"); break;
    case PublicUpdateOutcome::Cancelled: headline = QStringLiteral("本次检查已取消"); break;
    case PublicUpdateOutcome::PendingApply: headline = state_.candidatePassed && !state_.diskCommitted
        ? QStringLiteral("资料已检查，等待应用") : QStringLiteral("资料已保存，等待载入"); break;
    case PublicUpdateOutcome::Restored: headline = QStringLiteral("已恢复完整资料版本"); break;
    default: break;
  }
  title_->setText(headline);
  summary_->setText(!state_.readable ? state_.error : !hasState_ && !legacyStatus_.isEmpty() ? legacyStatus_ : !state_.summary.isEmpty() ? state_.summary :
      hasState_ ? QStringLiteral("工作台与战斗回放共用这一份官方资料，检查失败时保留可用版本。")
                : QStringLiteral("正在读取最近检查与当前资料…"));
  QDateTime time = QDateTime::fromString(state_.checkedUtc, Qt::ISODateWithMs);
  if (!time.isValid()) time = QDateTime::fromString(state_.checkedUtc, Qt::ISODate);
  checked_->setText(time.isValid() ? QStringLiteral("最近检查：%1").arg(time.toLocalTime().toString(QStringLiteral("MM-dd HH:mm")))
                                 : QStringLiteral("最近检查：尚无记录"));
  applied_->setText(state_.sessionApplied ? QStringLiteral("当前窗口：已载入现用资料")
      : !state_.diskRevision.isEmpty() ? QStringLiteral("当前窗口：等待载入已保存资料")
      : QStringLiteral("当前资料：正在准备"));
  check_->setEnabled(!blocked);
  retry_->setVisible(!state_.retryComponents.isEmpty()); retry_->setEnabled(!blocked);
  cancel_->setVisible(checking); cancel_->setEnabled(checking && state_.canCancel);
  cancel_->setToolTip(state_.canCancel ? QStringLiteral("停止本次准备，现用资料保留") : QStringLiteral("已进入完整版本应用，等待本次提交结束"));
  const bool prepared = state_.readable && state_.outcome == PublicUpdateOutcome::PendingApply && state_.candidatePassed && !state_.diskCommitted;
  reload_->setText(prepared ? QStringLiteral("应用已检查资料") : QStringLiteral("载入已保存资料"));
  reload_->setVisible(prepared || (!state_.diskRevision.isEmpty() && !state_.sessionApplied));
  reload_->setEnabled(!blocked);
  restore_->setEnabled(!blocked && state_.canRestorePrevious);
  restore_->setToolTip(state_.canRestorePrevious ? QStringLiteral("完整恢复上一份公共资料；账号与个人商店选择保持") : QStringLiteral("目前没有可恢复的上一份公共资料"));
  export_->setEnabled(hasState_ && !checking);
  changes_->setEnabled(hasState_);
  for (auto* part : partial_) part->setEnabled(!blocked);
  progressArea_->setVisible(checking);
  const int phase = int(state_.progress.phase) - 1;
  for (int index = 0; index < steps_.size(); ++index)
    steps_[index]->setStyleSheet(index == phase ? QStringLiteral("background:#e6f0fc;color:#174f8e;border-radius:4px;padding:4px;font-weight:600;")
                                              : QStringLiteral("color:palette(mid);padding:4px;"));
  progressText_->setText(!state_.progress.message.isEmpty() ? state_.progress.message
      : !legacyStatus_.isEmpty() ? legacyStatus_ : QStringLiteral("正在准备，请稍候…"));
  if (state_.progress.total > 0 && state_.progress.completed >= 0) {
    const qint64 total = state_.progress.total;
    progress_->setRange(0, int(qMin<qint64>(total, std::numeric_limits<int>::max())));
    progress_->setValue(int(qBound(qint64(0), state_.progress.completed, qint64(progress_->maximum()))));
    progress_->setFormat(QStringLiteral("%1 / %2").arg(state_.progress.completed).arg(total));
  } else { progress_->setRange(0, 0); progress_->setFormat(QString{}); }
  for (const auto& part : parts()) {
    auto* value = components_.value(part.first);
    QString status = hasState_ ? QStringLiteral("本次未检查") : QStringLiteral("等待读取");
    bool warning = false;
    for (const auto& component : state_.components) if (component.key == part.first) {
      status = componentStatus(component); warning = component.hasWarnings || component.retryable;
      if (component.status == QStringLiteral("updated") && state_.diskCommitted)
        status = state_.sessionApplied ? QStringLiteral("已更新") : QStringLiteral("已保存");
      value->setToolTip(component.message); break;
    }
    value->setText(part.second + QStringLiteral(" · ") + status);
    value->setStyleSheet(warning ? QStringLiteral("color:#8d580b;") : QString{});
  }
}

void PublicUpdatePanel::showChanges() {
  if (changesDialog_) { changesDialog_->raise(); changesDialog_->activateWindow(); return; }
  auto* dialog = new QDialog(this); changesDialog_ = dialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowTitle(QStringLiteral("官方资料变化"));
  dialog->resize(850, 570);
  auto* layout = new QVBoxLayout(dialog);
  const PublicUpdateState frozen = state_;
  QDateTime checked = QDateTime::fromString(frozen.checkedUtc, Qt::ISODateWithMs);
  if (!checked.isValid()) checked = QDateTime::fromString(frozen.checkedUtc, Qt::ISODate);
  if (checked.isValid()) layout->addWidget(label(QStringLiteral("检查时间：%1").arg(
      checked.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))), dialog));
  layout->addWidget(label(frozen.totalChanges > 0
      ? QStringLiteral("本次记录 %1 项变化。%2").arg(frozen.totalChanges).arg(frozen.changesTruncated ? QStringLiteral("本页保留部分变化摘要，完整原资料仍保存在对应版本中。") : QString{})
      : frozen.changesRecorded ? QStringLiteral("本次检查没有用户可见的内容变化。")
      : QStringLiteral("此记录没有可展示的逐项变化；较早的检查可能未保存变化明细。"), dialog));
  auto* filter = new QComboBox(dialog); filter->setObjectName(QStringLiteral("KQPublicChangeFilter"));
  filter->addItem(QStringLiteral("全部类别"), QString{});
  for (const auto& part : parts()) filter->addItem(part.second, part.first);
  layout->addWidget(filter);
  auto* split = new QSplitter(dialog);
  auto* list = new QListWidget(split); list->setObjectName(QStringLiteral("KQPublicChangeList"));
  auto* right = new QWidget(split); auto* detailLayout = new QVBoxLayout(right);
  auto* text = new QTextBrowser(right); text->setOpenLinks(false); text->setOpenExternalLinks(false);
  text->setObjectName(QStringLiteral("KQPublicChangeText")); detailLayout->addWidget(text);
  auto* technicalToggle = new QCheckBox(QStringLiteral("技术标识与原始字段"), right);
  auto* technical = new QTextEdit(right); technical->setReadOnly(true); technical->setMaximumHeight(140); technical->hide();
  detailLayout->addWidget(technicalToggle); detailLayout->addWidget(technical);
  connect(technicalToggle, &QCheckBox::toggled, technical, &QWidget::setVisible);
  split->setStretchFactor(0, 1); split->setStretchFactor(1, 2); layout->addWidget(split, 1);
  const auto populate = [list, filter, frozen] {
    list->clear(); const auto component = filter->currentData().toString();
    for (int index = 0; index < frozen.changes.size(); ++index) {
      const auto& change = frozen.changes[index];
      if (!component.isEmpty() && change.component != component) continue;
      const QString name = change.name.isEmpty() ? QStringLiteral("资料条目") : change.name;
      auto* item = new QListWidgetItem(changeKind(change.kind) + QStringLiteral(" · ") + name, list);
      item->setData(Qt::UserRole, index);
    }
    if (list->count()) list->setCurrentRow(0);
  };
  connect(list, &QListWidget::currentItemChanged, dialog, [frozen, text, technical](QListWidgetItem* item) {
    if (!item) { text->clear(); technical->clear(); return; }
    const auto& change = frozen.changes[item->data(Qt::UserRole).toInt()];
    const QString empty = QStringLiteral("未提供官方文字说明");
    text->setHtml(QStringLiteral("<h2>%1</h2><p>%2</p><h3>原资料</h3><p>%3</p><p>%4</p><h3>更新后</h3><p>%5</p><p>%6</p>")
        .arg(paragraph(change.name), changeKind(change.kind), paragraph(change.beforeName),
             paragraph(change.beforeText.isEmpty() ? empty : change.beforeText), paragraph(change.afterName),
             paragraph(change.afterText.isEmpty() ? empty : change.afterText)));
    technical->setPlainText(QString::fromUtf8(QJsonDocument(change.technical).toJson(QJsonDocument::Indented)));
  });
  connect(filter, qOverload<int>(&QComboBox::currentIndexChanged), dialog, populate);
  populate();
  auto* close = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
  close->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
  connect(close, &QDialogButtonBox::rejected, dialog, &QDialog::reject); layout->addWidget(close); dialog->show();
}

void PublicUpdatePanel::showDetails() {
  if (detailsDialog_) { detailsDialog_->raise(); detailsDialog_->activateWindow(); return; }
  auto* dialog = new QDialog(this); detailsDialog_ = dialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowTitle(QStringLiteral("资料检查详情")); dialog->resize(710, 510);
  auto* layout = new QVBoxLayout(dialog); auto* text = new QTextBrowser(dialog);
  text->setOpenLinks(false); text->setOpenExternalLinks(false);
  QString html = QStringLiteral("<h2>%1</h2>").arg(paragraph(state_.summary));
  for (const auto& part : state_.components)
    html += QStringLiteral("<h3>%1 · %2</h3><p>%3</p>").arg(paragraph(part.label), componentStatus(part), paragraph(part.message));
  text->setHtml(html); layout->addWidget(text, 1);
  auto* toggle = new QCheckBox(QStringLiteral("技术记录与原始错误"), dialog);
  auto* raw = new QTextEdit(dialog); raw->setReadOnly(true); raw->setMaximumHeight(170); raw->hide();
  QJsonObject technical = state_.technical;
  technical.insert(QStringLiteral("diskRevision"), state_.diskRevision);
  technical.insert(QStringLiteral("sessionRevision"), state_.sessionRevision);
  technical.insert(QStringLiteral("latestAttempt"), state_.latestAttempt);
  technical.insert(QStringLiteral("error"), state_.error);
  QJsonObject rows;
  for (const auto& part : state_.components) rows.insert(part.key, part.technical);
  technical.insert(QStringLiteral("components"), rows);
  raw->setPlainText(QString::fromUtf8(QJsonDocument(technical).toJson(QJsonDocument::Indented)));
  layout->addWidget(toggle); layout->addWidget(raw); connect(toggle, &QCheckBox::toggled, raw, &QWidget::setVisible);
  auto* close = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
  close->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
  connect(close, &QDialogButtonBox::rejected, dialog, &QDialog::reject); layout->addWidget(close); dialog->show();
}
