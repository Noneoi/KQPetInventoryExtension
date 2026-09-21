#include "pet_settings_dialog.h"

#include "diagnostics/build_info.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSet>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVersionNumber>
#include <QXmlStreamReader>

#include <tuple>

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
QString softwareClientRoot() {
  const QString configured = qEnvironmentVariable("KQPET_CLIENT_ROOT");
  return QDir::cleanPath(configured.isEmpty() ? QCoreApplication::applicationDirPath() : configured);
}
QString releaseVersionText(const QString& tag) {
  const QString trimmed = tag.trimmed();
  return trimmed.startsWith(QLatin1Char('v'), Qt::CaseInsensitive) ? trimmed.mid(1) : trimmed;
}
bool officialUpdateAssetsReady(const QJsonArray& assets) {
  static const QRegularExpression archiveName(QStringLiteral(
      "^KQPetInventory-[0-9]+\\.[0-9]+\\.[0-9]+-[0-9A-Fa-f]{12}-[0-9]{8}T[0-9]{6}Z-win-x64-copy-ready\\.zip$"));
  QString archive;
  QSet<QString> uploaded;
  for (const auto& value : assets) {
    const auto asset = value.toObject();
    if (asset.value(QStringLiteral("state")).toString() != QStringLiteral("uploaded")) continue;
    const QString name = asset.value(QStringLiteral("name")).toString();
    const QUrl url(asset.value(QStringLiteral("browser_download_url")).toString());
    if (name.isEmpty() || url.scheme() != QStringLiteral("https") ||
        url.host().compare(QStringLiteral("github.com"), Qt::CaseInsensitive) != 0) continue;
    uploaded.insert(name);
    if (archiveName.match(name).hasMatch()) archive = name;
  }
  return !archive.isEmpty() && uploaded.contains(archive + QStringLiteral(".sha256"));
}
}

PetSettingsDialog::PetSettingsDialog(const RefreshTimings& timings, QWidget* parent)
    : QDialog(parent) {
  setWindowTitle(QStringLiteral("设置 · 精灵工作台 %1").arg(BuildInfo::buildLabel()));
  setModal(true);
  resize(750, 670);
  setMinimumSize(650, 590);
  auto* root = new QVBoxLayout(this);
  auto* tabs = new QTabWidget(this);
  tabs->setObjectName(QStringLiteral("KQSettingsTabs"));
  tabs->addTab(createCachePage(), QStringLiteral("本地缓存"));
  tabs->addTab(createDataPage(), QStringLiteral("数据更新"));
  tabs->addTab(createSoftwareUpdatePage(), QStringLiteral("软件更新"));
  tabs->addTab(createTimingPage(), QStringLiteral("刷新参数"));
  root->addWidget(tabs);
  root->addWidget(explanation(QStringLiteral("缓存管理和数据更新立即执行；刷新参数点击“保存”后生效。"), this));
  dialogButtons_ = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel |
                                        QDialogButtonBox::RestoreDefaults, this);
  dialogButtons_->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
  dialogButtons_->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("关闭"));
  dialogButtons_->button(QDialogButtonBox::RestoreDefaults)->setText(QStringLiteral("重置刷新参数"));
  connect(dialogButtons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(dialogButtons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(dialogButtons_->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked,
          this, [this] { applyTimings(RefreshTimings{}); });
  root->addWidget(dialogButtons_);
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
  catalogLayout->addWidget(explanation(QStringLiteral(
      "游戏更新后，点击“全部检查更新”即可；只有官方版本变化的部分才会重新下载和解析。"
      "也可以只更新某一类数据，其余数据保持不变。首次检查需要准备解析工具，可能需要几分钟。"), catalogs));
  dataUpdateButton_ = button(QStringLiteral("全部检查更新"), catalogs);
  dataUpdateButton_->setObjectName(QStringLiteral("KQCheckDataUpdate"));
  dataUpdateButton_->setToolTip(QStringLiteral("依次检查下面全部六类数据"));
  catalogLayout->addWidget(dataUpdateButton_, 0, Qt::AlignLeft);
  auto* partial = new QGridLayout;
  partial->addWidget(new QLabel(QStringLiteral("单独更新："), catalogs), 0, 0);
  const QList<std::tuple<QString, QString, QString>> parts{
      {QStringLiteral("pets"), QStringLiteral("精灵与养成资料"),
       QStringLiteral("精灵字典、星神、星轮、元魂、源兽规则，以及属性/职业/货币名称")},
      {QStringLiteral("skills"), QStringLiteral("精灵技能资料"),
       QStringLiteral("普通技、超杀技、神运/灵初技能、英雄/通灵/元素技、词条、召唤与契约关系")},
      {QStringLiteral("shop"), QStringLiteral("兑换商店"), QStringLiteral("常驻商店、活动商店与钻石兑换活动")},
      {QStringLiteral("images"), QStringLiteral("精灵图片索引"),
       QStringLiteral("新精灵的图片来源；已缓存图片版本变化时一并更新")},
      {QStringLiteral("icons"), QStringLiteral("星神与属性图标"), QStringLiteral("星神和属性的小图标")},
      {QStringLiteral("routines"), QStringLiteral("日常与活动"), QStringLiteral("日常/周常任务目录和当前活动列表")}};
  for (int index = 0; index < parts.size(); ++index) {
    const auto& [component, title, tip] = parts[index];
    auto* part = button(title, catalogs);
    part->setObjectName(QStringLiteral("KQDataUpdate-%1").arg(component));
    part->setToolTip(tip);
    partial->addWidget(part, index / 3, index % 3 + 1);
    connect(part, &QPushButton::clicked, this, [this, component = component, title = title] {
      setDataUpdateStatus(QStringLiteral("正在检查%1…").arg(title), true);
      emit dataUpdateRequested({component});
    });
    partialUpdateButtons_.append(part);
  }
  partial->setColumnStretch(4, 1);
  catalogLayout->addLayout(partial);
  dataUpdateStatus_ = explanation(QStringLiteral("尚未检查。更新失败时保留现有本地数据。"), catalogs);
  dataUpdateStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  catalogLayout->addWidget(dataUpdateStatus_);
  connect(dataUpdateButton_, &QPushButton::clicked, this, [this] {
    setDataUpdateStatus(QStringLiteral("正在检查全部官方数据…"), true);
    emit dataUpdateRequested({});
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

QWidget* PetSettingsDialog::createSoftwareUpdatePage() {
  auto* page = new QWidget(this);
  page->setObjectName(QStringLiteral("KQSoftwareUpdatePage"));
  page->setStyleSheet(QStringLiteral(
      "QGroupBox#KQSoftwareUpdateCard{font-weight:600;border:1px solid #d8e3ee;border-radius:9px;"
      "margin-top:12px;padding:14px;background:#fbfdff;}"
      "QGroupBox#KQSoftwareUpdateCard::title{subcontrol-origin:margin;left:14px;padding:0 6px;color:#234d72;}"
      "QPushButton#KQSoftwareUpdateInstall{background:#2866a8;color:white;border:0;border-radius:5px;"
      "padding:8px 18px;font-weight:600;}"
      "QPushButton#KQSoftwareUpdateInstall:hover{background:#367bc1;}"
      "QPushButton#KQSoftwareUpdateInstall:disabled{background:#b7c4d0;color:#eef2f5;}"));
  auto* root = new QVBoxLayout(page);
  root->setContentsMargins(16, 14, 16, 14);
  root->setSpacing(12);

  auto* card = new QGroupBox(QStringLiteral("精灵工作台更新"), page);
  card->setObjectName(QStringLiteral("KQSoftwareUpdateCard"));
  auto* layout = new QVBoxLayout(card);
  layout->setSpacing(10);
  softwareUpdateTitle_ = new QLabel(
      QStringLiteral("当前版本  %1").arg(BuildInfo::buildLabel()), card);
  softwareUpdateTitle_->setObjectName(QStringLiteral("KQSoftwareUpdateTitle"));
  softwareUpdateTitle_->setWordWrap(true);
  softwareUpdateTitle_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  QFont titleFont = softwareUpdateTitle_->font();
  titleFont.setPointSize(titleFont.pointSize() + 2);
  titleFont.setWeight(QFont::DemiBold);
  softwareUpdateTitle_->setFont(titleFont);
  layout->addWidget(softwareUpdateTitle_);
  layout->addWidget(explanation(QStringLiteral(
      "检查 GitHub 上最新的正式发行版。检测到新版本后会先展示发行说明；只有点击更新并通过完整性校验后才会安装。"), card));

  auto* actions = new QHBoxLayout;
  softwareUpdateCheck_ = button(QStringLiteral("检查更新"), card);
  softwareUpdateCheck_->setObjectName(QStringLiteral("KQSoftwareUpdateCheck"));
  softwareUpdateInstall_ = button(QStringLiteral("更新到此版本"), card);
  softwareUpdateInstall_->setObjectName(QStringLiteral("KQSoftwareUpdateInstall"));
  softwareUpdateInstall_->setEnabled(false);
  softwareUpdateReleasePage_ = button(QStringLiteral("查看 GitHub 发行页"), card);
  softwareUpdateReleasePage_->setObjectName(QStringLiteral("KQSoftwareUpdateReleasePage"));
  softwareUpdateReleasePage_->setEnabled(false);
  actions->addWidget(softwareUpdateCheck_);
  actions->addWidget(softwareUpdateInstall_);
  actions->addWidget(softwareUpdateReleasePage_);
  actions->addStretch();
  layout->addLayout(actions);

  softwareUpdateProgress_ = new QProgressBar(card);
  softwareUpdateProgress_->setObjectName(QStringLiteral("KQSoftwareUpdateProgress"));
  softwareUpdateProgress_->setRange(0, 0);
  softwareUpdateProgress_->setTextVisible(false);
  softwareUpdateProgress_->hide();
  layout->addWidget(softwareUpdateProgress_);
  softwareUpdateStatus_ = explanation(QStringLiteral("尚未检查更新。"), card);
  softwareUpdateStatus_->setObjectName(QStringLiteral("KQSoftwareUpdateStatus"));
  softwareUpdateStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(softwareUpdateStatus_);
  root->addWidget(card);

  auto* notesGroup = new QGroupBox(QStringLiteral("新版本更新内容"), page);
  auto* notesLayout = new QVBoxLayout(notesGroup);
  softwareUpdateNotes_ = new QTextBrowser(notesGroup);
  softwareUpdateNotes_->setObjectName(QStringLiteral("KQSoftwareUpdateNotes"));
  softwareUpdateNotes_->setOpenExternalLinks(false);
  softwareUpdateNotes_->setPlaceholderText(QStringLiteral("点击“检查更新”后在这里查看发行说明。"));
  softwareUpdateNotes_->setMinimumHeight(250);
  notesLayout->addWidget(softwareUpdateNotes_);
  root->addWidget(notesGroup, 1);

  connect(softwareUpdateCheck_, &QPushButton::clicked, this, &PetSettingsDialog::checkSoftwareUpdate);
  connect(softwareUpdateInstall_, &QPushButton::clicked, this, &PetSettingsDialog::installSoftwareUpdate);
  connect(softwareUpdateReleasePage_, &QPushButton::clicked, this, [this] {
    if (!softwareUpdatePageUrl_.isEmpty()) QDesktopServices::openUrl(QUrl(softwareUpdatePageUrl_));
  });
  return page;
}

void PetSettingsDialog::checkSoftwareUpdate() {
  if (softwareUpdateReply_ || (softwareUpdateProcess_ && softwareUpdateProcess_->state() != QProcess::NotRunning)) return;
  softwareUpdateAvailable_ = false;
  softwareUpdateTag_.clear();
  softwareUpdatePageUrl_.clear();
  softwareUpdateInstall_->setEnabled(false);
  softwareUpdateReleasePage_->setEnabled(false);
  softwareUpdateNotes_->clear();
  setSoftwareUpdateBusy(true, QStringLiteral("正在连接 GitHub 检查最新正式版…"));
  if (!softwareUpdateNetwork_) softwareUpdateNetwork_ = new QNetworkAccessManager(this);
  QNetworkRequest request{QUrl(QStringLiteral("https://api.github.com/repos/Noneoi/KQPetInventoryExtension/releases/latest"))};
  request.setRawHeader("Accept", "application/vnd.github+json");
  request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
  request.setRawHeader("User-Agent", "KQPetInventoryExtension-SettingsUpdater");
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setTransferTimeout(15000);
  softwareUpdateReply_ = softwareUpdateNetwork_->get(request);
  connect(softwareUpdateReply_, &QNetworkReply::finished, this, [this] {
    QNetworkReply* reply = softwareUpdateReply_;
    softwareUpdateReply_ = nullptr;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray payload = reply->readAll();
    const QString networkError = reply->error() == QNetworkReply::NoError ? QString{} : reply->errorString();
    reply->deleteLater();
    if (status == 403) {
      checkSoftwareUpdateFallback(networkError);
      return;
    }
    if (!networkError.isEmpty() || status != 200 || payload.size() > 2 * 1024 * 1024) {
      setSoftwareUpdateBusy(false, networkError.isEmpty()
          ? QStringLiteral("GitHub 返回了无效响应（HTTP %1）。请稍后重试。").arg(status)
          : QStringLiteral("检查失败：%1。当前版本不受影响。").arg(networkError));
      return;
    }
    QJsonParseError parseError;
    const auto release = QJsonDocument::fromJson(payload, &parseError).object();
    const QString tag = release.value(QStringLiteral("tag_name")).toString().trimmed();
    const QString versionText = releaseVersionText(tag);
    const QVersionNumber latest = QVersionNumber::fromString(versionText);
    const QVersionNumber current = QVersionNumber::fromString(BuildInfo::version());
    if (parseError.error != QJsonParseError::NoError || release.value(QStringLiteral("draft")).toBool() ||
        release.value(QStringLiteral("prerelease")).toBool() || latest.isNull() || current.isNull()) {
      setSoftwareUpdateBusy(false, QStringLiteral("GitHub 最新发行版的版本信息无法识别，未执行任何更新。"));
      return;
    }
    softwareUpdateTag_ = tag;
    softwareUpdatePageUrl_ = release.value(QStringLiteral("html_url")).toString();
    softwareUpdateReleasePage_->setEnabled(QUrl(softwareUpdatePageUrl_).isValid());
    const QString releaseName = release.value(QStringLiteral("name")).toString(tag);
    const QDateTime published = QDateTime::fromString(release.value(QStringLiteral("published_at")).toString(), Qt::ISODate);
    softwareUpdateTitle_->setText(QStringLiteral("%1  ·  %2%3")
        .arg(releaseName, tag, published.isValid()
             ? QStringLiteral("  ·  %1 发布").arg(published.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")))
             : QString{}));
    QString notes = release.value(QStringLiteral("body")).toString().trimmed();
    if (notes.isEmpty()) notes = QStringLiteral("本次发行没有填写更新说明。");
    softwareUpdateNotes_->document()->setMarkdown(notes, QTextDocument::MarkdownDialectGitHub);
    const bool newer = QVersionNumber::compare(latest, current) > 0;
    const bool assetsReady = officialUpdateAssetsReady(release.value(QStringLiteral("assets")).toArray());
    softwareUpdateAvailable_ = newer && assetsReady;
    softwareUpdateInstall_->setEnabled(softwareUpdateAvailable_);
    if (!newer) {
      setSoftwareUpdateBusy(false, QStringLiteral("当前已是最新正式版（%1）。").arg(tag));
    } else if (!assetsReady) {
      setSoftwareUpdateBusy(false, QStringLiteral("发现 %1，但发行附件尚未准备完整，暂不能更新。").arg(tag));
    } else {
      setSoftwareUpdateBusy(false, QStringLiteral("发现新版本 %1。请阅读更新内容后决定是否安装。").arg(tag));
    }
  });
}

void PetSettingsDialog::checkSoftwareUpdateFallback(const QString& reason) {
  Q_UNUSED(reason);
  softwareUpdateStatus_->setText(QStringLiteral("GitHub API 配额暂时不可用，正在改用官方发行页检查…"));
  QNetworkRequest request{QUrl(QStringLiteral("https://github.com/Noneoi/KQPetInventoryExtension/releases/latest"))};
  request.setRawHeader("User-Agent", "KQPetInventoryExtension-SettingsUpdater");
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
  request.setTransferTimeout(15000);
  softwareUpdateReply_ = softwareUpdateNetwork_->get(request);
  connect(softwareUpdateReply_, &QNetworkReply::finished, this, [this] {
    QNetworkReply* reply = softwareUpdateReply_;
    softwareUpdateReply_ = nullptr;
    QUrl releaseUrl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if (releaseUrl.isRelative()) releaseUrl = reply->url().resolved(releaseUrl);
    if (releaseUrl.isEmpty() && reply->url().path().contains(QStringLiteral("/releases/tag/"))) releaseUrl = reply->url();
    const QString error = reply->error() == QNetworkReply::NoError ? QString{} : reply->errorString();
    reply->deleteLater();
    static const QRegularExpression releasePath(QStringLiteral(
        "^/Noneoi/KQPetInventoryExtension/releases/tag/(v[0-9]+\\.[0-9]+\\.[0-9]+)$"));
    const auto pathMatch = releasePath.match(QUrl::fromPercentEncoding(releaseUrl.path().toUtf8()));
    if ((!error.isEmpty() && releaseUrl.isEmpty()) || releaseUrl.scheme() != QStringLiteral("https") ||
        releaseUrl.host().compare(QStringLiteral("github.com"), Qt::CaseInsensitive) != 0 || !pathMatch.hasMatch()) {
      setSoftwareUpdateBusy(false, QStringLiteral("GitHub 暂时限制了检查频率，请稍后重试或打开发行页查看。"));
      softwareUpdatePageUrl_ = QStringLiteral("https://github.com/Noneoi/KQPetInventoryExtension/releases/latest");
      softwareUpdateReleasePage_->setEnabled(true);
      return;
    }
    softwareUpdateTag_ = pathMatch.captured(1);
    softwareUpdatePageUrl_ = releaseUrl.toString(QUrl::FullyEncoded);
    softwareUpdateReleasePage_->setEnabled(true);
    softwareUpdateStatus_->setText(QStringLiteral("已确认最新版本 %1，正在读取发行说明…").arg(softwareUpdateTag_));
    QNetworkRequest feedRequest{QUrl(QStringLiteral("https://github.com/Noneoi/KQPetInventoryExtension/releases.atom"))};
    feedRequest.setRawHeader("User-Agent", "KQPetInventoryExtension-SettingsUpdater");
    feedRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    feedRequest.setTransferTimeout(15000);
    softwareUpdateReply_ = softwareUpdateNetwork_->get(feedRequest);
    connect(softwareUpdateReply_, &QNetworkReply::finished, this, [this] {
      QNetworkReply* feedReply = softwareUpdateReply_;
      softwareUpdateReply_ = nullptr;
      const QByteArray payload = feedReply->readAll();
      const bool networkOk = feedReply->error() == QNetworkReply::NoError && payload.size() <= 2 * 1024 * 1024;
      feedReply->deleteLater();
      QString releaseName = softwareUpdateTag_;
      QString releaseHtml;
      QDateTime published;
      bool matched = false;
      if (networkOk) {
        QXmlStreamReader xml(payload);
        bool inEntry = false;
        QString title, link, content, updated;
        while (!xml.atEnd()) {
          const auto token = xml.readNext();
          if (token == QXmlStreamReader::StartElement && xml.name() == QStringLiteral("entry")) {
            inEntry = true; title.clear(); link.clear(); content.clear(); updated.clear();
          } else if (inEntry && token == QXmlStreamReader::StartElement && xml.name() == QStringLiteral("title")) {
            title = xml.readElementText();
          } else if (inEntry && token == QXmlStreamReader::StartElement && xml.name() == QStringLiteral("link")) {
            link = xml.attributes().value(QStringLiteral("href")).toString();
          } else if (inEntry && token == QXmlStreamReader::StartElement && xml.name() == QStringLiteral("content")) {
            content = xml.readElementText(QXmlStreamReader::IncludeChildElements);
          } else if (inEntry && token == QXmlStreamReader::StartElement && xml.name() == QStringLiteral("updated")) {
            updated = xml.readElementText();
          } else if (inEntry && token == QXmlStreamReader::EndElement && xml.name() == QStringLiteral("entry")) {
            inEntry = false;
            const QUrl entryUrl(link);
            if (entryUrl.path() == QUrl(softwareUpdatePageUrl_).path()) {
              releaseName = title.isEmpty() ? softwareUpdateTag_ : title;
              releaseHtml = content;
              published = QDateTime::fromString(updated, Qt::ISODate);
              matched = true;
              break;
            }
          }
        }
      }
      softwareUpdateTitle_->setText(QStringLiteral("%1  ·  %2%3")
          .arg(releaseName, softwareUpdateTag_, published.isValid()
               ? QStringLiteral("  ·  %1 发布").arg(published.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")))
               : QString{}));
      if (matched && !releaseHtml.trimmed().isEmpty()) softwareUpdateNotes_->setHtml(releaseHtml);
      else softwareUpdateNotes_->setPlainText(QStringLiteral("发行说明暂时无法读取，可点击“查看 GitHub 发行页”查看。"));
      const QVersionNumber latest = QVersionNumber::fromString(releaseVersionText(softwareUpdateTag_));
      const QVersionNumber current = QVersionNumber::fromString(BuildInfo::version());
      const bool newer = !latest.isNull() && !current.isNull() && QVersionNumber::compare(latest, current) > 0;
      softwareUpdateAvailable_ = newer;
      softwareUpdateInstall_->setEnabled(newer);
      setSoftwareUpdateBusy(false, newer
          ? QStringLiteral("发现新版本 %1。已通过 GitHub 官方发行页确认，下载时还会执行完整校验。").arg(softwareUpdateTag_)
          : QStringLiteral("当前已是最新正式版（%1）。").arg(softwareUpdateTag_));
    });
  });
}

void PetSettingsDialog::installSoftwareUpdate() {
  if (!softwareUpdateAvailable_ || softwareUpdateTag_.isEmpty() ||
      (softwareUpdateProcess_ && softwareUpdateProcess_->state() != QProcess::NotRunning)) return;
  if (QMessageBox::question(this, QStringLiteral("更新精灵工作台"),
      QStringLiteral("下载并验证 %1？\n\n当前程序正在运行，因此新版会先安全暂存；关闭氪奇并重新启动后完成切换。")
          .arg(softwareUpdateTag_), QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes) return;
  const QString client = softwareClientRoot();
  const QString tools = QDir(client).filePath(QStringLiteral("KQPetQuickStart/package/tools"));
  const QString script = QDir(tools).filePath(QStringLiteral("auto-update.ps1"));
  const QString checker = QDir(tools).filePath(QStringLiteral("KQPetReleaseCheck.exe"));
  if (!QFile::exists(script) || !QFile::exists(checker)) {
    setSoftwareUpdateBusy(false, QStringLiteral("更新组件不完整。请重新下载最新 copy-ready 安装包后再试。"));
    return;
  }
  const QString windows = qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"));
  const QString powershell = QDir(windows).filePath(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"));
  softwareUpdateProcess_ = new QProcess(this);
  softwareUpdateProcess_->setProgram(powershell);
  softwareUpdateProcess_->setArguments({QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"),
      QStringLiteral("-NonInteractive"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
      QStringLiteral("-File"), script, QStringLiteral("-ClientRoot"), client,
      QStringLiteral("-ReleaseCheck"), checker, QStringLiteral("-Force")});
  softwareUpdateProcess_->setWorkingDirectory(client);
  softwareUpdateProcess_->setProcessChannelMode(QProcess::MergedChannels);
  connect(softwareUpdateProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      softwareUpdateProcess_->deleteLater();
      softwareUpdateProcess_ = nullptr;
      setSoftwareUpdateBusy(false, QStringLiteral("更新程序无法启动，请检查系统 PowerShell 是否可用。"));
    }
  });
  connect(softwareUpdateProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          this, [this](int, QProcess::ExitStatus) { finishSoftwareUpdate(); });
  setSoftwareUpdateBusy(true, QStringLiteral("正在下载、校验并准备 %1，请不要关闭此窗口…").arg(softwareUpdateTag_));
  softwareUpdateProcess_->start();
}

void PetSettingsDialog::finishSoftwareUpdate() {
  if (!softwareUpdateProcess_) return;
  softwareUpdateProcess_->deleteLater();
  softwareUpdateProcess_ = nullptr;
  const QString statePath = QDir(softwareClientRoot()).filePath(QStringLiteral("KQPetData/updates/github-release.json"));
  QFile stateFile(statePath);
  QJsonObject state;
  if (stateFile.open(QIODevice::ReadOnly)) state = QJsonDocument::fromJson(stateFile.readAll()).object();
  const QString status = state.value(QStringLiteral("status")).toString();
  if (status == QStringLiteral("staged")) {
    softwareUpdateAvailable_ = false;
    setSoftwareUpdateBusy(false, QStringLiteral("新版已下载并通过校验。请关闭氪奇，再双击“启动精灵工作台”完成更新。"));
  } else if (status == QStringLiteral("updated") || status == QStringLiteral("current")) {
    softwareUpdateAvailable_ = false;
    setSoftwareUpdateBusy(false, QStringLiteral("更新已经完成。重新启动后将使用新版本。"));
  } else {
    const QString message = state.value(QStringLiteral("message")).toString();
    setSoftwareUpdateBusy(false, message.isEmpty()
        ? QStringLiteral("更新未完成。当前版本没有变化，请稍后重试。")
        : QStringLiteral("更新未完成：%1").arg(message));
  }
}

void PetSettingsDialog::setSoftwareUpdateBusy(bool busy, const QString& status) {
  softwareUpdateProgress_->setVisible(busy);
  softwareUpdateStatus_->setText(status);
  softwareUpdateCheck_->setEnabled(!busy);
  softwareUpdateInstall_->setEnabled(!busy && softwareUpdateAvailable_);
  if (dialogButtons_) dialogButtons_->setEnabled(!busy);
}

void PetSettingsDialog::reject() {
  if (softwareUpdateProcess_ && softwareUpdateProcess_->state() != QProcess::NotRunning) {
    softwareUpdateStatus_->setText(QStringLiteral("正在准备更新，请等待完成后再关闭设置窗口。"));
    return;
  }
  QDialog::reject();
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
  for (QPushButton* part : partialUpdateButtons_) part->setEnabled(!busy && !imageBatchRunning_);
  missingImagesButton_->setEnabled(!busy && !imageBatchRunning_);
}
void PetSettingsDialog::setImageBatchProgress(int completed, int total, int failed) {
  imageBatchRunning_ = true;
  dataUpdateButton_->setEnabled(false);
  for (QPushButton* part : partialUpdateButtons_) part->setEnabled(false);
  missingImagesButton_->setEnabled(false);
  imagePause_->setEnabled(true); imageCancel_->setEnabled(true);
  imageProgress_->setRange(0, qMax(1, total)); imageProgress_->setValue(qBound(0, completed, qMax(1, total)));
  imageStatus_->setText(QStringLiteral("已处理 %1 / %2 张；失败 %3 张。").arg(completed).arg(total).arg(failed));
}
void PetSettingsDialog::finishImageBatch(bool cancelled, int failed) {
  imageBatchRunning_ = false;
  dataUpdateButton_->setEnabled(!dataUpdateBusy_);
  for (QPushButton* part : partialUpdateButtons_) part->setEnabled(!dataUpdateBusy_);
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
