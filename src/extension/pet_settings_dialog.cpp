#include "pet_settings_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {
QSpinBox* makeSpinBox(int minimum, int maximum, const QString& suffix, QWidget* parent) {
  auto* box = new QSpinBox(parent);
  box->setRange(minimum, maximum);
  box->setSuffix(suffix);
  box->setAccelerated(true);
  box->setMinimumWidth(160);
  return box;
}
QLabel* explanation(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  label->setWordWrap(true);
  label->setTextFormat(Qt::PlainText);
  return label;
}
QPushButton* button(const QString& text, QWidget* parent) {
  auto* value = new QPushButton(text, parent);
  value->setAutoDefault(false);
  return value;
}
QString sizeText(qint64 bytes) {
  if (bytes >= 1024LL * 1024 * 1024) return QStringLiteral("%1 GB").arg(double(bytes) / (1024.0 * 1024 * 1024), 0, 'f', 2);
  if (bytes >= 1024 * 1024) return QStringLiteral("%1 MB").arg(double(bytes) / (1024.0 * 1024), 0, 'f', 1);
  return QStringLiteral("%1 KB").arg(double(bytes) / 1024, 0, 'f', 1);
}
}

PetSettingsDialog::PetSettingsDialog(const RefreshTimings& timings, QWidget* parent)
    : QDialog(parent) {
  setWindowTitle(QStringLiteral("设置"));
  setModal(true);
  resize(750, 670);
  setMinimumSize(650, 590);
  auto* root = new QVBoxLayout(this);
  auto* tabs = new QTabWidget(this);
  tabs->setObjectName(QStringLiteral("KQSettingsTabs"));
  tabs->addTab(createCachePage(), QStringLiteral("本地缓存"));
  tabs->addTab(createDataPage(), QStringLiteral("数据更新"));
  tabs->addTab(createTimingPage(), QStringLiteral("刷新参数"));
  root->addWidget(tabs);
  root->addWidget(explanation(QStringLiteral("缓存管理和数据更新立即执行；刷新参数点击“保存”后生效。"), this));
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel |
                                      QDialogButtonBox::RestoreDefaults, this);
  buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
  buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("关闭"));
  buttons->button(QDialogButtonBox::RestoreDefaults)->setText(QStringLiteral("重置刷新参数"));
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked,
          this, [this] { applyTimings(RefreshTimings{}); });
  root->addWidget(buttons);
  applyTimings(timings);
}

QWidget* PetSettingsDialog::createCachePage() {
  auto* page = new QWidget(this);
  auto* layout = new QVBoxLayout(page);
  layout->addWidget(explanation(QStringLiteral("详情和图片长期保存在本地。最新有效数据覆盖同一项；不会自动清理。"), page));
  auto* directory = new QHBoxLayout;
  cacheRootEdit_ = new QLineEdit(page);
  cacheRootEdit_->setReadOnly(true);
  cacheRootEdit_->setObjectName(QStringLiteral("KQCacheRoot"));
  auto* open = button(QStringLiteral("打开目录"), page);
  directory->addWidget(cacheRootEdit_, 1); directory->addWidget(open);
  layout->addLayout(directory);
  connect(open, &QPushButton::clicked, this, [this] {
    if (!cacheRoot_.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(cacheRoot_));
  });
  cacheActions_ = new QWidget(page);
  auto* actions = new QVBoxLayout(cacheActions_);
  actions->setContentsMargins(0, 0, 0, 0);
  auto* stats = new QHBoxLayout;
  cacheSummary_ = explanation(QStringLiteral("点击“刷新统计”查看本地缓存。"), cacheActions_);
  cacheSummary_->setObjectName(QStringLiteral("KQCacheSummary"));
  auto* inspect = button(QStringLiteral("刷新统计"), cacheActions_);
  stats->addWidget(cacheSummary_, 1); stats->addWidget(inspect);
  actions->addLayout(stats);
  connect(inspect, &QPushButton::clicked, this, [this] { requestCacheAction(QStringLiteral("inspect")); });

  auto* selected = new QGroupBox(QStringLiteral("按项目管理"), cacheActions_);
  auto* form = new QFormLayout(selected);
  auto* accountRow = new QHBoxLayout;
  account_ = new QComboBox(selected);
  account_->setEditable(true);
  account_->setObjectName(QStringLiteral("KQCacheAccount"));
  account_->lineEdit()->setPlaceholderText(QStringLiteral("选择或输入账号"));
  account_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[A-Za-z0-9_-]*")), account_));
  auto* clearAccount = button(QStringLiteral("删除此账号缓存"), selected);
  accountRow->addWidget(account_, 1); accountRow->addWidget(clearAccount);
  form->addRow(QStringLiteral("账号"), accountRow);
  connect(clearAccount, &QPushButton::clicked, this, [this] {
    const QString account = selectedAccount();
    if (account.isEmpty()) { cacheStatus_->setText(QStringLiteral("请先选择账号。")); return; }
    if (confirmRemoval(QStringLiteral("删除账号 %1 的本地详情、列表、商店、日常和历史快照缓存？").arg(account)))
      requestCacheAction(QStringLiteral("clear-account"), {{QStringLiteral("account"), account}});
  });
  auto* detailRow = new QHBoxLayout;
  instanceId_ = new QLineEdit(selected);
  instanceId_->setObjectName(QStringLiteral("KQCacheInstanceId"));
  instanceId_->setPlaceholderText(QStringLiteral("精灵实例 ID"));
  instanceId_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[1-9][0-9]*")), instanceId_));
  auto* clearDetail = button(QStringLiteral("删除这只详情"), selected);
  detailRow->addWidget(instanceId_, 1); detailRow->addWidget(clearDetail);
  form->addRow(QStringLiteral("精灵详情"), detailRow);
  connect(clearDetail, &QPushButton::clicked, this, [this] {
    const QString account = selectedAccount(), id = instanceId_->text().trimmed();
    if (account.isEmpty() || id.isEmpty()) { cacheStatus_->setText(QStringLiteral("请选择账号并输入精灵实例 ID。")); return; }
    if (confirmRemoval(QStringLiteral("删除账号 %1 中实例 %2 的本地详情？").arg(account, id)))
      requestCacheAction(QStringLiteral("clear-detail"), {{QStringLiteral("account"), account}, {QStringLiteral("instanceId"), id}});
  });
  auto* imageRow = new QHBoxLayout;
  imageFile_ = new QLineEdit(selected);
  imageFile_->setReadOnly(true);
  imageFile_->setPlaceholderText(QStringLiteral("选择图片缓存文件"));
  auto* chooseImage = button(QStringLiteral("选择"), selected);
  auto* clearImage = button(QStringLiteral("删除此图"), selected);
  imageRow->addWidget(imageFile_, 1); imageRow->addWidget(chooseImage); imageRow->addWidget(clearImage);
  form->addRow(QStringLiteral("图片"), imageRow);
  connect(chooseImage, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择本地图片缓存"),
        QDir(cacheRoot_).filePath(QStringLiteral("images")), QStringLiteral("图片 (*.png *.jpg *.jpeg *.webp)"));
    if (path.isEmpty()) return;
    const QString relative = QDir(cacheRoot_).relativeFilePath(path).replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!relative.startsWith(QStringLiteral("images/")) || relative.contains(QStringLiteral("../"))) {
      QMessageBox::information(this, QStringLiteral("选择图片"), QStringLiteral("请选择当前缓存目录 images 文件夹中的图片。")); return;
    }
    imageFile_->setText(relative); imageFile_->setToolTip(path);
  });
  connect(clearImage, &QPushButton::clicked, this, [this] {
    const QString relative = imageFile_->text();
    if (relative.isEmpty()) { cacheStatus_->setText(QStringLiteral("请先选择图片文件。")); return; }
    if (confirmRemoval(QStringLiteral("删除本地图片 %1？下次查看时可重新下载。").arg(relative)))
      requestCacheAction(QStringLiteral("clear-image"), {{QStringLiteral("relativeFile"), relative}});
  });
  actions->addWidget(selected);

  auto* maintenance = new QHBoxLayout;
  auto* cleanup = button(QStringLiteral("整理旧版重复图片"), cacheActions_);
  auto* clearImages = button(QStringLiteral("清空图片缓存"), cacheActions_);
  auto* clearAll = button(QStringLiteral("清空全部缓存"), cacheActions_);
  maintenance->addWidget(cleanup); maintenance->addWidget(clearImages); maintenance->addWidget(clearAll);
  actions->addLayout(maintenance);
  connect(cleanup, &QPushButton::clicked, this, [this] {
    if (confirmRemoval(QStringLiteral("删除已有有效稳定缓存替代的旧版本、旧名称图片？尚无替代图片的文件会保留。")))
      requestCacheAction(QStringLiteral("cleanup-legacy"));
  });
  connect(clearImages, &QPushButton::clicked, this, [this] {
    if (confirmRemoval(QStringLiteral("删除全部本地图片缓存？精灵详情保留，图片需要重新下载。")))
      requestCacheAction(QStringLiteral("clear-images"));
  });
  connect(clearAll, &QPushButton::clicked, this, [this] {
    if (confirmRemoval(QStringLiteral("删除所有账号的可重建缓存、公共数据目录和图片？建议先备份；断网时将无法查看被删除的资料。")))
      requestCacheAction(QStringLiteral("clear-all"));
  });

  auto* backup = new QGroupBox(QStringLiteral("备份、恢复与目录"), cacheActions_);
  auto* backupLayout = new QVBoxLayout(backup);
  auto* backupRow = new QHBoxLayout;
  auto* saveBackup = button(QStringLiteral("备份为 ZIP"), backup);
  auto* restore = button(QStringLiteral("从 ZIP 恢复"), backup);
  auto* changeRoot = button(QStringLiteral("更换缓存目录"), backup);
  backupRow->addWidget(saveBackup); backupRow->addWidget(restore); backupRow->addWidget(changeRoot);
  backupLayout->addLayout(backupRow);
  restoreOverwrite_ = new QCheckBox(QStringLiteral("恢复时覆盖已有文件（默认只补齐缺失文件）"), backup);
  restoreOverwrite_->setChecked(false);
  backupLayout->addWidget(restoreOverwrite_);
  backupLayout->addWidget(explanation(QStringLiteral("更换目录在下次启动时生效。可复制现有缓存，旧目录始终保留。"), backup));
  actions->addWidget(backup);
  connect(saveBackup, &QPushButton::clicked, this, [this] {
    QString target = QFileDialog::getSaveFileName(this, QStringLiteral("备份缓存"),
        QDir::homePath() + QStringLiteral("/KQPetData-") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) + QStringLiteral(".zip"),
        QStringLiteral("ZIP 备份 (*.zip)"));
    if (target.isEmpty()) return;
    if (!target.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) target += QStringLiteral(".zip");
    requestCacheAction(QStringLiteral("backup"), {{QStringLiteral("destination"), target}});
  });
  connect(restore, &QPushButton::clicked, this, [this] {
    const QString source = QFileDialog::getOpenFileName(this, QStringLiteral("恢复缓存"), QDir::homePath(), QStringLiteral("ZIP 备份 (*.zip)"));
    if (source.isEmpty()) return;
    const bool overwrite = restoreOverwrite_->isChecked();
    if (overwrite && QMessageBox::question(this, QStringLiteral("覆盖恢复"),
        QStringLiteral("使用此备份覆盖同名缓存文件？\n%1").arg(source), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    requestCacheAction(QStringLiteral("restore"), {{QStringLiteral("destination"), source}, {QStringLiteral("overwrite"), overwrite}});
  });
  connect(changeRoot, &QPushButton::clicked, this, [this] {
    const QString destination = QFileDialog::getExistingDirectory(this, QStringLiteral("选择新的缓存目录"), cacheRoot_);
    if (destination.isEmpty()) return;
    QMessageBox choice(QMessageBox::Question, QStringLiteral("更换缓存目录"),
        QStringLiteral("新目录：%1\n下次启动生效；原目录保留。\n复制缓存需要选择空目录。已有缓存目录可直接切换。").arg(QDir::toNativeSeparators(destination)),
        QMessageBox::Cancel, this);
    auto* copy = choice.addButton(QStringLiteral("复制缓存并切换"), QMessageBox::AcceptRole);
    auto* direct = choice.addButton(QStringLiteral("直接切换"), QMessageBox::ActionRole);
    choice.exec();
    if (choice.clickedButton() != copy && choice.clickedButton() != direct) return;
    requestCacheAction(QStringLiteral("schedule-root"), {{QStringLiteral("destination"), destination},
        {QStringLiteral("copyExisting"), choice.clickedButton() == copy}});
  });
  cacheActions_->setEnabled(false);
  layout->addWidget(cacheActions_);
  cacheStatus_ = explanation(QStringLiteral("缓存目录尚未载入。"), page);
  cacheStatus_->setObjectName(QStringLiteral("KQCacheStatus"));
  cacheStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(cacheStatus_);
  layout->addStretch();
  return page;
}

QWidget* PetSettingsDialog::createDataPage() {
  auto* page = new QWidget(this);
  auto* layout = new QVBoxLayout(page);
  auto* catalogs = new QGroupBox(QStringLiteral("官方公共数据"), page);
  auto* catalogLayout = new QVBoxLayout(catalogs);
  catalogLayout->addWidget(explanation(QStringLiteral("手动更新精灵与养成资料、属性/职业/货币名称、兑换目录、精灵图片、星神/属性图标，以及日常任务和活动。首次检查可能需要几分钟。"), catalogs));
  dataUpdateButton_ = button(QStringLiteral("检查数据更新"), catalogs);
  dataUpdateButton_->setObjectName(QStringLiteral("KQCheckDataUpdate"));
  catalogLayout->addWidget(dataUpdateButton_, 0, Qt::AlignLeft);
  dataUpdateStatus_ = explanation(QStringLiteral("尚未检查。更新失败时保留现有本地数据。"), catalogs);
  dataUpdateStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  catalogLayout->addWidget(dataUpdateStatus_);
  connect(dataUpdateButton_, &QPushButton::clicked, this, [this] {
    setDataUpdateStatus(QStringLiteral("正在检查官方数据…"), true);
    emit dataUpdateRequested();
  });
  layout->addWidget(catalogs);
  auto* images = new QGroupBox(QStringLiteral("本地图片"), page);
  auto* imageLayout = new QVBoxLayout(images);
  imageLayout->addWidget(explanation(QStringLiteral("已有图片直接从磁盘读取。查看精灵时，只为缺失图片下载；下载失败后可右键重试。\n“补齐缺失图片”会下载当前库存缺少的图片，已有图片不会重复下载。"), images));
  auto* row = new QHBoxLayout;
  missingImagesButton_ = button(QStringLiteral("补齐缺失图片"), images);
  imagePause_ = button(QStringLiteral("暂停"), images);
  imagePause_->setCheckable(true);
  imageCancel_ = button(QStringLiteral("取消"), images);
  row->addWidget(missingImagesButton_); row->addStretch(); row->addWidget(imagePause_); row->addWidget(imageCancel_);
  imageLayout->addLayout(row);
  imageProgress_ = new QProgressBar(images);
  imageProgress_->setRange(0, 1); imageProgress_->setValue(0);
  imageProgress_->setFormat(QStringLiteral("%v / %m"));
  imageLayout->addWidget(imageProgress_);
  imageStatus_ = explanation(QStringLiteral("尚未开始。可以暂停、继续或取消；已保存的图片保留。"), images);
  imageLayout->addWidget(imageStatus_);
  imagePause_->setEnabled(false); imageCancel_->setEnabled(false);
  connect(missingImagesButton_, &QPushButton::clicked, this, [this] {
    setImageBatchProgress(0, 0, 0);
    emit missingImagesRequested();
  });
  connect(imagePause_, &QPushButton::clicked, this, [this](bool paused) {
    imagePause_->setText(paused ? QStringLiteral("继续") : QStringLiteral("暂停"));
    emit imageBatchPauseRequested(paused);
  });
  connect(imageCancel_, &QPushButton::clicked, this, [this] {
    imageCancel_->setEnabled(false); emit imageBatchCancelRequested();
  });
  layout->addWidget(images);
  layout->addStretch();
  return page;
}

QWidget* PetSettingsDialog::createTimingPage() {
  auto* page = new QWidget(this);
  auto* root = new QVBoxLayout(page);
  root->addWidget(explanation(QStringLiteral("背包、仓库不再定时刷新。手动点击刷新或查看某只精灵时查询最新数据；断网或失败时继续显示本地缓存。"), page));
  auto* listGroup = new QGroupBox(QStringLiteral("背包 / 仓库列表"), page);
  auto* listForm = new QFormLayout(listGroup);
  listRequestGapMs_ = makeSpinBox(0, 10000, QStringLiteral(" 毫秒"), listGroup);
  listTimeoutSeconds_ = makeSpinBox(1, 120, QStringLiteral(" 秒"), listGroup);
  listForm->addRow(QStringLiteral("背包到仓库请求间隔"), listRequestGapMs_);
  listForm->addRow(QStringLiteral("单个列表请求超时"), listTimeoutSeconds_);
  root->addWidget(listGroup);
  auto* detailGroup = new QGroupBox(QStringLiteral("仓库详情队列"), page);
  auto* detailForm = new QFormLayout(detailGroup);
  detailRequestGapMs_ = makeSpinBox(0, 10000, QStringLiteral(" 毫秒"), detailGroup);
  detailBatchSize_ = makeSpinBox(1, 100, QStringLiteral(" 只 / 批"), detailGroup);
  detailBatchRestSeconds_ = makeSpinBox(0, 120, QStringLiteral(" 秒"), detailGroup);
  detailTimeoutSeconds_ = makeSpinBox(1, 120, QStringLiteral(" 秒"), detailGroup);
  detailMaxRetries_ = makeSpinBox(0, 10, QStringLiteral(" 次"), detailGroup);
  detailForm->addRow(QStringLiteral("每只请求间隔"), detailRequestGapMs_);
  detailForm->addRow(QStringLiteral("每批数量"), detailBatchSize_);
  detailForm->addRow(QStringLiteral("每批完成后休息"), detailBatchRestSeconds_);
  detailForm->addRow(QStringLiteral("单只详情请求超时"), detailTimeoutSeconds_);
  detailForm->addRow(QStringLiteral("失败重试次数"), detailMaxRetries_);
  root->addWidget(detailGroup);
  auto* moveGroup = new QGroupBox(QStringLiteral("精灵移动"), page);
  auto* moveForm = new QFormLayout(moveGroup);
  moveRequestTimeoutSeconds_ = makeSpinBox(1, 120, QStringLiteral(" 秒"), moveGroup);
  moveForm->addRow(QStringLiteral("移动写请求响应超时"), moveRequestTimeoutSeconds_);
  root->addWidget(moveGroup);
  root->addStretch();
  return page;
}

QString PetSettingsDialog::selectedAccount() const { return account_->currentText().trimmed(); }
bool PetSettingsDialog::confirmRemoval(const QString& description) {
  return QMessageBox::question(this, QStringLiteral("确认删除本地缓存"), description,
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
}
void PetSettingsDialog::requestCacheAction(const QString& action, const QJsonObject& options) {
  if (cacheRoot_.isEmpty()) return;
  cacheActions_->setEnabled(false);
  cacheStatus_->setText(action == QStringLiteral("inspect") ? QStringLiteral("正在读取缓存统计…") : QStringLiteral("正在处理，请稍候…"));
  emit cacheActionRequested(action, options);
}
void PetSettingsDialog::setCacheRoot(const QString& root) {
  cacheRoot_ = root.isEmpty() ? QString{} : QDir::cleanPath(root);
  cacheRootEdit_->setText(QDir::toNativeSeparators(cacheRoot_));
  cacheRootEdit_->setToolTip(cacheRoot_);
  cacheActions_->setEnabled(!root.isEmpty());
  if (!root.isEmpty()) cacheStatus_->setText(QStringLiteral("缓存目录已载入。"));
}
void PetSettingsDialog::applyCacheResult(const QString& action, const QJsonObject& result) {
  cacheActions_->setEnabled(!cacheRoot_.isEmpty());
  cacheStatus_->setText(result.value(QStringLiteral("message")).toString(
      result.value(QStringLiteral("ok")).toBool() ? QStringLiteral("操作完成。") : QStringLiteral("操作未完成，请重试。")));
  if (!result.value(QStringLiteral("ok")).toBool()) return;
  const auto summary = result.value(QStringLiteral("summary")).toObject();
  if (summary.isEmpty()) return;
  const QString selected = selectedAccount();
  account_->clear();
  for (const auto& entry : summary.value(QStringLiteral("accounts")).toArray()) {
    const auto value = entry.toObject();
    const QString name = value.value(QStringLiteral("account")).toString();
    account_->addItem(name);
    account_->setItemData(account_->count() - 1,
        QStringLiteral("%1 只详情 · %2").arg(value.value(QStringLiteral("details")).toInteger())
          .arg(sizeText(value.value(QStringLiteral("bytes")).toInteger())), Qt::ToolTipRole);
  }
  account_->setCurrentText(selected.isEmpty() && account_->count() ? account_->itemText(0) : selected);
  const QDateTime latest = QDateTime::fromString(summary.value(QStringLiteral("latest")).toString(), Qt::ISODateWithMs);
  cacheSummary_->setText(QStringLiteral("占用 %1 · 详情 %2 只 · 图片 %3 张 · 缺图 %4 张\n旧版图片文件 %5 个 · 最近更新 %6")
      .arg(sizeText(summary.value(QStringLiteral("bytes")).toInteger()))
      .arg(summary.value(QStringLiteral("details")).toInteger())
      .arg(summary.value(QStringLiteral("images")).toInteger())
      .arg(summary.value(QStringLiteral("missingImages")).toInteger())
      .arg(summary.value(QStringLiteral("legacyImageFiles")).toInteger())
      .arg(latest.isValid() ? latest.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QStringLiteral("暂无")));
  Q_UNUSED(action);
}
void PetSettingsDialog::setDataUpdateStatus(const QString& message, bool busy) {
  dataUpdateBusy_ = busy;
  dataUpdateStatus_->setText(message);
  dataUpdateButton_->setEnabled(!busy && !imageBatchRunning_);
  missingImagesButton_->setEnabled(!busy && !imageBatchRunning_);
}
void PetSettingsDialog::setImageBatchProgress(int completed, int total, int failed) {
  imageBatchRunning_ = true;
  dataUpdateButton_->setEnabled(false);
  missingImagesButton_->setEnabled(false);
  imagePause_->setEnabled(true); imageCancel_->setEnabled(true);
  imageProgress_->setRange(0, qMax(1, total)); imageProgress_->setValue(qBound(0, completed, qMax(1, total)));
  imageStatus_->setText(QStringLiteral("已处理 %1 / %2 张；失败 %3 张。").arg(completed).arg(total).arg(failed));
}
void PetSettingsDialog::finishImageBatch(bool cancelled, int failed) {
  imageBatchRunning_ = false;
  dataUpdateButton_->setEnabled(!dataUpdateBusy_);
  imagePause_->setChecked(false); imagePause_->setText(QStringLiteral("暂停"));
  imagePause_->setEnabled(false); imageCancel_->setEnabled(false);
  missingImagesButton_->setEnabled(!dataUpdateBusy_);
  imageStatus_->setText(cancelled ? QStringLiteral("已取消，成功保存的图片保留。")
      : failed ? QStringLiteral("补图结束；%1 张未完成，可稍后重试。").arg(failed) : QStringLiteral("补图完成，图片已保存到本地。"));
}

void PetSettingsDialog::applyTimings(const RefreshTimings& timings) {
  listRequestGapMs_->setValue(timings.listRequestGapMs);
  listTimeoutSeconds_->setValue(qMax(1, (timings.listTimeoutMs + 999) / 1000));
  detailRequestGapMs_->setValue(timings.detailRequestGapMs);
  detailBatchSize_->setValue(timings.detailBatchSize);
  detailBatchRestSeconds_->setValue((timings.detailBatchRestMs + 999) / 1000);
  detailTimeoutSeconds_->setValue(qMax(1, (timings.detailTimeoutMs + 999) / 1000));
  detailMaxRetries_->setValue(timings.detailMaxRetries);
  moveRequestTimeoutSeconds_->setValue(qMax(1, (timings.moveRequestTimeoutMs + 999) / 1000));
}
RefreshTimings PetSettingsDialog::timings() const {
  RefreshTimings result;
  result.automaticIntervalMs = 0;
  result.listRequestGapMs = listRequestGapMs_->value();
  result.listTimeoutMs = listTimeoutSeconds_->value() * 1000;
  result.detailRequestGapMs = detailRequestGapMs_->value();
  result.detailBatchSize = detailBatchSize_->value();
  result.detailBatchRestMs = detailBatchRestSeconds_->value() * 1000;
  result.detailTimeoutMs = detailTimeoutSeconds_->value() * 1000;
  result.detailMaxRetries = detailMaxRetries_->value();
  result.moveRequestTimeoutMs = moveRequestTimeoutSeconds_->value() * 1000;
  return result;
}
