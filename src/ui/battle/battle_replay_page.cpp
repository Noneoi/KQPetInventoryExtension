#include "ui/battle/battle_replay_page.h"
#include "ui/battle/battle_replay_paths.h"
#include "ui/battle/battle_replay_buff_presentation.h"
#include "ui/battle/battle_replay_buff_history.h"
#include "ui/battle/battle_replay_identity_presentation.h"
#include "ui/battle/battle_replay_inspection.h"
#include "ui/battle/battle_replay_reference_library.h"
#include "ui/detail/public_data_detail_dialog.h"
#include "domain/battle_replay_presentation.h"
#include "domain/battle_replay_dragon.h"
#include "ui/battle/battle_replay_cell.h"
#include "ui/battle/battle_replay_spirit.h"
#include "ui/battle/battle_replay_links.h"
#include "ui/battle/battle_replay_meter_overlay.h"
#include "ui/battle/battle_replay_action_meters.h"
#include "ui/battle/battle_replay_meter_sequence.h"
#include "ui/common/ui_preferences.h"
#include "domain/battle_replay_empowerment.h"

#include <QAbstractTableModel>
#include <QAction>
#include <QComboBox>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include "battle_replay_life_timeline.h"
#include "battle_replay_timeline_events.h"
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QToolButton>
#include <QTableView>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <functional>

using namespace BattleReplay;
namespace {
QString esc(const QString& s) { return s.toHtmlEscaped(); }
QString publicReplayLabel(QString label) {
  static const QRegularExpression channel(QStringLiteral("（现有被动通道 (recivedata|cutdata)）$"));
  label.remove(channel);
  return label;
}
bool sameCatalogContent(const BattleReplayCatalogHandle& a, const BattleReplayCatalogHandle& b) {
  return a == b || (a && b && !a->sha256.isEmpty() && a->sha256 == b->sha256);
}
QString catalogSourceName(const BattleReplayCatalogHandle& snapshot) {
  if (!snapshot) return QStringLiteral("资料未就绪");
  return snapshot->sourceLabel.isEmpty()
    ? snapshot->embedded ? QStringLiteral("内置回放资料") : QStringLiteral("本地回放资料")
    : snapshot->sourceLabel;
}
class TeamSourceCard final : public QWidget {
public:
  explicit TeamSourceCard(QWidget* parent, std::function<void()> inspect) : QWidget(parent), inspect_(std::move(inspect)) {
    setFixedSize(154, 46); setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  }
  void display(const QString& kind, const QString& name, const QString& owner,
               const QString& instance, const QString& skill, const QString& detail, bool animate) {
    kind_ = kind; name_ = name; animate_ = animate;
    const int available = parentWidget() ? std::max(154, parentWidget()->width() - 146) : 220;
    setFixedWidth(std::clamp(42 + static_cast<int>(name.size()) * 6, 154, std::min(220, available)));
    setProperty("sourceKind", kind); setProperty("sourceName", name); setProperty("ownerKey", owner);
    setProperty("instanceId", instance); setProperty("skillId", skill); setProperty("animated", animate);
    setProperty("rawEvidence", detail);
    setAccessibleName((kind == QStringLiteral("dragon") ? QStringLiteral("小神龙来源：") : QStringLiteral("队伍效果来源：")) + name);
    setToolTip(accessibleName() + QStringLiteral("\n点击查看来源详情"));
    setAccessibleDescription(toolTip()); update();
  }
  void setProgress(qreal value) { progress_ = std::clamp(value, qreal(0), qreal(1)); setProperty("playbackProgress", progress_); update(); }
protected:
  void mousePressEvent(QMouseEvent* event) override { if (event->button() == Qt::LeftButton && inspect_) inspect_(); else QWidget::mousePressEvent(event); }
  void paintEvent(QPaintEvent*) override {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    const QColor accent(kind_ == QStringLiteral("dragon") ? QStringLiteral("#a96721") : QStringLiteral("#7554ac"));
    const qreal pulse = animate_ ? std::sin(progress_ * 3.141592653589793) : 0;
    p.setPen(QPen(accent, 1.5 + pulse)); p.setBrush(kind_ == QStringLiteral("dragon") ? QColor("#fff4df") : QColor("#f3edfb"));
    p.drawRoundedRect(QRectF(1, 1, width() - 2, height() - 2), 7, 7);
    p.save(); p.translate(18, 23); p.scale(1 + 0.12 * pulse, 1 + 0.12 * pulse); p.setPen(QPen(accent, 2));
    if (kind_ == QStringLiteral("dragon")) {
      QPainterPath head; head.moveTo(-9, 8); head.lineTo(-8, -2); head.lineTo(-11, -9); head.lineTo(-3, -5);
      head.lineTo(3, -10); head.lineTo(4, -3); head.lineTo(10, 0); head.lineTo(8, 5); head.lineTo(0, 8); head.closeSubpath();
      p.setBrush(QColor("#f2d190")); p.drawPath(head); p.setBrush(accent); p.drawEllipse(QPointF(3, 0), 1.3, 1.3);
    } else {
      p.setBrush(QColor("#d9cbed")); p.drawEllipse(QPointF(-5, -4), 3, 3); p.drawEllipse(QPointF(5, -4), 3, 3);
      p.drawArc(QRectF(-11, 1, 12, 12), 0, 180 * 16); p.drawArc(QRectF(-1, 1, 12, 12), 0, 180 * 16);
    }
    p.restore();
    auto titleFont = font(); titleFont.setPixelSize(10); titleFont.setWeight(QFont::DemiBold); p.setFont(titleFont); p.setPen(accent);
    p.drawText(QRect(36, 3, width() - 42, 12), Qt::AlignLeft | Qt::AlignVCenter,
      kind_ == QStringLiteral("dragon") ? QStringLiteral("小神龙来源") : QStringLiteral("队伍效果来源"));
    auto nameFont = font(); nameFont.setPixelSize(12); nameFont.setWeight(QFont::DemiBold);
    const QRect textRect(36, 16, width() - 42, 27);
    while (nameFont.pixelSize() > 9 && QFontMetrics(nameFont).boundingRect(textRect, Qt::TextWordWrap, name_).height() > textRect.height())
      nameFont.setPixelSize(nameFont.pixelSize() - 1);
    p.setFont(nameFont); p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, name_);
  }
private:
  QString kind_, name_; bool animate_ = false; qreal progress_ = 1;
  std::function<void()> inspect_;
};
struct TeamSourceIdentity { QString kind, name, instance, detail; };
QString recordedIntegerId(const QJsonValue& value);
TeamSourceIdentity teamSourceIdentity(const Step& step, const Fighter& owner, const DragonRoster& roster, const QJsonObject& catalog) {
  TeamSourceIdentity result;
  const auto action = step.raw.toObject();
  const auto buff = valueText(action[QStringLiteral("epbi")]);
  result.kind = QStringLiteral("team-effect");
  result.name = catalog[QStringLiteral("buffs")].toObject()[buff].toObject()[QStringLiteral("nm")].toString();
  if (result.name.isEmpty()) result.name = buff.isEmpty() ? QStringLiteral("队伍记录") : replayPublicReferenceName(catalog, QStringLiteral("buffs"), buff);
  result.detail = QStringLiteral("%1\n已记录执行 Buff：%2\n记录：%3/epbi").arg(result.name, buff, step.path);
  if (step.playbackKind != QStringLiteral("team-skill") || !action[QStringLiteral("eps")].isArray()) return result;
  const auto ids = action[QStringLiteral("eps")].toArray();
  if (ids.isEmpty()) return result;
  const auto id = recordedIntegerId(ids.first());
  const DragonInfo* match = nullptr; int matches = 0;
  for (const auto& dragon : roster.dragons) if (!id.isEmpty() && id.toLongLong() > 0 && dragon.id == id) { match = &dragon; ++matches; }
  if (matches != 1 || !match) { result.detail += QStringLiteral("\n小神龙实例未唯一匹配；不推定具体来源。"); return result; }
  result.kind = QStringLiteral("dragon"); result.name = match->name == QStringLiteral("小神龙 [%1]").arg(match->id) ? QStringLiteral("小神龙名称待确认") : match->name; result.instance = id;
  result.detail = QStringLiteral("%1 · %2\n回包 eps[0] 与当前玩家的小神龙实例唯一匹配。\n出手记录：%3/eps/0\n实例记录：%4\n执行 Buff：%5")
    .arg(owner.player, match->name, step.path, match->path, buff);
  return result;
}
QString number(std::optional<qint64> n) { return n ? QString::number(*n) : QStringLiteral("待核实"); }
bool inactiveTargetRecord(const Effect& e) {
  const auto flags = e.raw.toObject();
  if (!flags[QStringLiteral("isse")].isBool() || flags[QStringLiteral("isse")].toBool() ||
      !flags[QStringLiteral("ishit")].isBool() || flags[QStringLiteral("ishit")].toBool()) return false;
  for (const auto& name : {QStringLiteral("isbl"), QStringLiteral("iscr"), QStringLiteral("issb"), QStringLiteral("isIm")})
    if (flags[name].toBool()) return false;
  return true;
}
bool meaningfulEffect(const Effect& e) {
  return e.changed || (e.recordedDelta && *e.recordedDelta != 0) || e.kind == QStringLiteral("buff") ||
    ((e.kind == QStringLiteral("target") || e.kind == QStringLiteral("team-target")) && !inactiveTargetRecord(e));
}
void addVisualBadges(ReplayUnitOverview& cue, const QStringList& labels) {
  auto current = cue.visualBadge.value_or(QString()).split(QStringLiteral(" · "), Qt::SkipEmptyParts);
  current.append(labels); current.removeDuplicates(); cue.visualBadge = current.join(QStringLiteral(" · "));
}
QString cueSkillName(const QString& id, const QJsonObject& catalog) {
  if (id.isEmpty()) return QStringLiteral("未记录");
  const auto name = catalog[QStringLiteral("skills")].toObject()[id].toObject()[QStringLiteral("name")].toString();
  return name.isEmpty() ? replayPublicReferenceName(catalog, QStringLiteral("skills"), id) : name;
}
QString cueAttributeNames(const QStringList& ids, const QJsonObject& catalog) {
  QStringList names;
  for (const auto& id : ids) {
    const auto name = catalog[QStringLiteral("attributes")].toObject()[id].toObject()[QStringLiteral("nm")].toString();
    names.append(name.isEmpty() ? replayPublicReferenceName(catalog, QStringLiteral("attributes"), id) : name);
  }
  return names.isEmpty() ? QStringLiteral("空属性") : names.join(QStringLiteral("、"));
}
QString recordedIntegerId(const QJsonValue& value) {
  bool ok = false; qint64 id = 0;
  if (value.isString()) id = value.toString().toLongLong(&ok);
  else if (value.isDouble()) {
    const auto number = value.toDouble();
    ok = std::isfinite(number) && std::floor(number) == number && std::abs(number) <= 9007199254740991.0;
    if (ok) id = static_cast<qint64>(number);
  }
  return ok ? QString::number(id) : QString();
}
bool visibleStateChanged(const Fighter& a, const Fighter& b) {
  return a.hp != b.hp || a.maxHp != b.maxHp || a.shield != b.shield || a.power != b.power || a.maxPower != b.maxPower ||
    a.buffs != b.buffs || a.buffsKnown != b.buffsKnown || a.raceId != b.raceId || a.faceId != b.faceId ||
    a.attributes != b.attributes || a.attributesKnown != b.attributesKnown || a.presence != b.presence ||
    a.entered != b.entered || a.displayLayersRestored != b.displayLayersRestored ||
    a.entryKnown != b.entryKnown || a.playerRemoved != b.playerRemoved ||
    a.originRaceId != b.originRaceId || a.originFighterKey != b.originFighterKey || a.originRelation != b.originRelation ||
    a.normalSkill != b.normalSkill || a.uniqueSkill != b.uniqueSkill ||
    a.x != b.x || a.y != b.y || a.playerX != b.playerX || a.playerY != b.playerY || a.lastUnit != b.lastUnit;
}
QStringList stateTransitionBadges(const Fighter& before, const Fighter& after) {
  if (before.key.isEmpty() || after.representer) return {};
  QStringList labels;
  if (before.hp && after.hp && *before.hp > 0 && *after.hp <= 0) labels.append(QStringLiteral("生命归零"));
  if (before.hp && after.hp && *before.hp <= 0 && *after.hp > 0 && presentedOnField(before) && presentedOnField(after))
    labels.append(QStringLiteral("生命恢复"));
  if (before.entryKnown && after.entryKnown && !before.entered && after.entered)
    labels.append(after.clone ? QStringLiteral("分身登场") : after.summoned ? QStringLiteral("召唤登场") : QStringLiteral("登场"));
  if ((!before.raceId.isEmpty() && !after.raceId.isEmpty() && before.raceId != after.raceId) ||
      (!before.faceId.isEmpty() && !after.faceId.isEmpty() && before.faceId != after.faceId)) labels.append(QStringLiteral("变身"));
  return labels;
}
QString attributesText(const Fighter& f, const QJsonObject& catalog) {
  if (!f.attributesKnown) return QStringLiteral("待核实");
  QStringList names; for (const auto& id : f.attributes) names.append(replayPublicReferenceName(catalog, QStringLiteral("attributes"), id));
  return names.isEmpty() ? QStringLiteral("空列表") : names.join(QStringLiteral("、"));
}
QString officialDescription(QString text) {
  // Formatting markers only; retain every word and number from the official text.
  static const QRegularExpression colorStart(QStringLiteral("\\$c\\w?"));
  text.remove(colorStart); text.replace(QStringLiteral("c$"), QString());
  QTextDocument document; document.setHtml(text);
  return document.toPlainText().toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
}
QString pretty(const QJsonValue& v) {
  if (v.isObject()) return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Indented));
  if (v.isArray()) return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Indented));
  return valueText(v);
}
QString summaryValue(const QJsonValue& v) {
  if (v.isObject()) return QStringLiteral("对象 · %1 个字段").arg(v.toObject().size());
  if (v.isArray()) return QStringLiteral("数组 · %1 条").arg(v.toArray().size());
  return valueText(v);
}
QString link(const Fighter& f, const QJsonObject& catalog) {
  return QStringLiteral("<a style='color:inherit;text-decoration:none' href='unit:%1'>%2</a>")
      .arg(QString::fromLatin1(QUrl::toPercentEncoding(f.key)), esc(replayFighterName(f, catalog)));
}
QString referenceLink(const QJsonObject& catalog, const QString& section, const QString& id) {
  return QStringLiteral("<a href='%1:%2'>%3</a>").arg(section == QStringLiteral("skills") ? QStringLiteral("skill") : QStringLiteral("buff"),
    QString::fromLatin1(QUrl::toPercentEncoding(id)), esc(replayPublicReferenceName(catalog, section, id)));
}
QTextBrowser* browser(QWidget* parent) {
  auto* b = new QTextBrowser(parent); b->setOpenLinks(false); b->setOpenExternalLinks(false);
  b->setFrameShape(QFrame::NoFrame); return b;
}
}

class ReplayStepModel final : public QAbstractTableModel {
public:
  explicit ReplayStepModel(QObject* parent) : QAbstractTableModel(parent) {}
  const Replay* source = nullptr;
  void reload(const Replay* value) { beginResetModel(); source = value; endResetModel(); }
  int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() || !source ? 0 : static_cast<int>(source->steps.size()); }
  int columnCount(const QModelIndex& = {}) const override { return 2; }
  QVariant data(const QModelIndex& index, int role) const override {
    if (!source || !index.isValid() || index.row() >= rowCount()) return {};
    const auto& s = source->steps[index.row()];
    if (role == Qt::DisplayRole) return index.column() == 0 ? (s.round < 0 ? QStringLiteral("—") : QString::number(s.round)) : s.title;
    if (role == Qt::ToolTipRole) return s.title + QLatin1Char('\n') + s.path;
    return {};
  }
  QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal) return section == 0 ? QStringLiteral("回合") : QStringLiteral("行动记录");
    return QAbstractTableModel::headerData(section, orientation, role);
  }
};
class ReplayFieldModel final : public QAbstractTableModel {
public:
  explicit ReplayFieldModel(QObject* parent) : QAbstractTableModel(parent) {}
  const Replay* source = nullptr;
  void reload(const Replay* value) { beginResetModel(); source = value; endResetModel(); }
  int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() || !source ? 0 : static_cast<int>(source->fields.size()); }
  int columnCount(const QModelIndex& = {}) const override { return 3; }
  QVariant data(const QModelIndex& index, int role) const override {
    if (!source || !index.isValid() || index.row() >= rowCount()) return {};
    const auto& f = source->fields[index.row()];
    if (role == Qt::DisplayRole) {
      if (index.column() == 0) return f.path;
      if (index.column() == 1) return f.label;
      return summaryValue(f.value);
    }
    if (role == Qt::ToolTipRole) return f.path + QLatin1Char('\n') + f.label + QLatin1Char('\n') + summaryValue(f.value);
    if (role == Qt::ForegroundRole && !f.verified) return QColor(QStringLiteral("#996014"));
    return {};
  }
  QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal)
      return QStringList{QStringLiteral("原始位置"), QStringLiteral("字段解释"), QStringLiteral("原始值")}.value(section);
    return QAbstractTableModel::headerData(section, orientation, role);
  }
};

class ReplayStepFilter final : public QSortFilterProxyModel {
public:
  explicit ReplayStepFilter(QObject* parent) : QSortFilterProxyModel(parent) {}
  QString unit;
  bool important = false;
  void refresh() { invalidateFilter(); }
protected:
  bool filterAcceptsRow(int row, const QModelIndex&) const override {
    const auto* model = static_cast<const ReplayStepModel*>(sourceModel());
    if (!model->source || row >= model->source->steps.size()) return false;
    const auto& step = model->source->steps[row];
    if (!unit.isEmpty() && !step.affected.contains(unit)) return false;
    if (!important) return true;
    if (step.keyChanges.contains(QStringLiteral("玩家换位")) || step.keyChanges.contains(QStringLiteral("玩家移除"))) return true;
    for (const auto& event : step.events) if (unit.isEmpty() || unit == event.key) return true;
    return false;
  }
};

BattleReplayPage::BattleReplayPage(QWidget* parent, BattleReplayCatalogHandle initialCatalog) : QWidget(parent) {
  reportDirectory_ = BattleReplayPaths::reportDirectory();
  setStyleSheet(QStringLiteral(
    "QWidget#BattleReplayPage,QWidget#replayArena,QWidget#replayBoard{background:#f5f7fb;color:#25364d}"
    "QPushButton{background:#ffffff;border:1px solid #d6deeb;border-radius:6px;padding:5px 11px;color:#354b69}"
    "QPushButton:hover{background:#edf4ff;border-color:#9db9e0} QPushButton:disabled{color:#9aa6b6;background:#f1f4f8}"
    "QPushButton#replayPlay{background:#356fbd;border-color:#356fbd;color:white;font-weight:600}"
    "QPushButton#replayPlay:hover{background:#285faa}"
    "QComboBox{background:white;border:1px solid #d8e0eb;border-radius:5px;padding:4px 7px;color:#354b69}"
    "QComboBox::drop-down{border:0;width:18px}"
    "QToolButton{background:#ffffff;border:1px solid #dbe3ef;border-radius:5px;padding:4px 7px;color:#4c637e}"
    "QToolButton:hover,QToolButton:checked{background:#e8f0fc;border-color:#adc5e8}"
    "QMenu{background:white;border:1px solid #d6dfed;padding:5px} QMenu::item{padding:7px 18px} QMenu::item:selected{background:#eaf2ff}"
    "QTextBrowser{selection-background-color:#d8e8ff}"
    "QScrollBar:vertical{background:#edf1f7;width:7px;border:0} QScrollBar::handle:vertical{background:#bdcadb;min-height:20px;border-radius:3px}"
    "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0}"));
  setObjectName(QStringLiteral("BattleReplayPage")); setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 9));
  auto* layout = new QVBoxLayout(this); layout->setContentsMargins(12, 8, 12, 8); layout->setSpacing(6);
  auto* opening = new QHBoxLayout;
  auto* open = new QPushButton(QStringLiteral("打开战斗日志"), this);
  auto* folder = new QPushButton(QStringLiteral("打开战报文件夹"), this);
  files_ = new QComboBox(this); files_->setMinimumContentsLength(24); files_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  side_ = new QComboBox(this); side_->setObjectName(QStringLiteral("replayOwnTeam"));
  side_->setToolTip(QStringLiteral("选择我方视角；双方朝向中央"));
  opening->addWidget(open); opening->addWidget(folder); opening->addWidget(files_, 1); opening->addWidget(side_); layout->addLayout(opening);
  incomingButton_ = new QToolButton(this); incomingButton_->setObjectName(QStringLiteral("replayIncoming"));
  incomingButton_->setText(QStringLiteral("新战报 0")); incomingButton_->setToolTip(QStringLiteral("已接收的新战报保留原文快照；点击选择，不打断当前播放"));
  incomingButton_->hide(); opening->addWidget(incomingButton_);
  connect(incomingButton_, &QToolButton::clicked, this, &BattleReplayPage::showIncomingReplays);
  sourceButton_ = new QToolButton(this); sourceButton_->setObjectName(QStringLiteral("replaySourceStatus"));
  sourceButton_->setText(QStringLiteral("战报来源")); opening->addWidget(sourceButton_);
  setReplaySourceStatus(QStringLiteral("当前为文件回放；工作台可接收客户端新战报，并监听已授权的 brush 目录。"));
  connect(sourceButton_, &QToolButton::clicked, this, [this] {
    unit_->setHtml(QStringLiteral("<h3>战报来源</h3><p>账号视角：%1</p><p>新战报会显示提醒，点击后打开；当前回放不会自动切换。</p><p><a href='source-raw:all'>查看原始接收状态</a></p>")
      .arg(accountReliable_ ? esc(accountName_) : QStringLiteral("手动选择我方")));
    openBoardDetail();
  });
  status_ = new QLabel(QStringLiteral("打开一份战报，查看每次出手、状态变化与死亡复活。"), this);
  status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true); status_->setObjectName(QStringLiteral("replayStatus")); layout->addWidget(status_);
  auto* controls = new QHBoxLayout;
  restart_ = new QPushButton(QStringLiteral("回到开局"), this); previous_ = new QPushButton(QStringLiteral("上一行动"), this);
  play_ = new QPushButton(QStringLiteral("连续播放"), this); next_ = new QPushButton(QStringLiteral("下一行动"), this);
  play_->setObjectName(QStringLiteral("replayPlay")); next_->setObjectName(QStringLiteral("replayNext"));
  slider_ = new ReplayLifeTimeline(this); slider_->setObjectName(QStringLiteral("replaySlider"));
  jump_ = new QSpinBox(this); jump_->setPrefix(QStringLiteral("记录 ")); rounds_ = new QComboBox(this);
  rounds_->setObjectName(QStringLiteral("replayRounds"));
  rounds_->setToolTip(QStringLiteral("跳到该回合第一条行动"));
  for (auto* button : {restart_, previous_, play_, next_}) controls->addWidget(button);
  playbackMode_ = new QComboBox(this); playbackMode_->setObjectName(QStringLiteral("replayPlaybackMode"));
  playbackMode_->addItem(QStringLiteral("连续行动"), QStringLiteral("action"));
  playbackMode_->addItem(QStringLiteral("逐条核对"), QStringLiteral("effect"));
  playbackMode_->setToolTip(QStringLiteral("连续行动：按技能前、出手、反击、技能后的顺序连续播放，同组目标同步变化。逐条核对：查看每条原始作用。")); controls->addWidget(playbackMode_);
  auto* speed = speed_ = new QComboBox(this); speed->setObjectName(QStringLiteral("replaySpeed"));
  for (const auto& value : {QPair<QString, int>{QStringLiteral("0.5×"), 3600}, {QStringLiteral("1×"), 1800}, {QStringLiteral("2×"), 900}, {QStringLiteral("4×"), 450}})
    speed->addItem(value.first, value.second);
  speed->setCurrentIndex(1); speed->setToolTip(QStringLiteral("播放速度；只改变展示节奏，不改日志顺序或数值")); controls->addWidget(speed);
  controls->addStretch();
  timelineView_ = new QComboBox(this); timelineView_->setObjectName(QStringLiteral("replayTimelineView"));
  timelineView_->addItem(QStringLiteral("回合总览"), QStringLiteral("round"));
  timelineView_->addItem(QStringLiteral("按精灵"), QStringLiteral("units"));
  timelineView_->setAccessibleName(QStringLiteral("死亡复活进度视图"));
  timelineView_->setToolTip(QStringLiteral("回合总览：按大回合查看敌我死亡、复活。按精灵：每只精灵独立一行，只显示死亡、复活记录。\n切换视图不改变播放位置。"));
  controls->addWidget(timelineView_);
  smallTurnLabel_ = new QLabel(this); smallTurnLabel_->setObjectName(QStringLiteral("replaySmallTurn"));
  smallTurnLabel_->setTextFormat(Qt::PlainText);
  smallTurnLabel_->setStyleSheet(QStringLiteral("color:#234780;font-weight:600"));
  smallTurnLabel_->setToolTip(QStringLiteral("小回合按当前大回合内的完整行动从 1 编号；不把技能内部小步骤算成小回合。\n死亡 / 复活标记只依据已记录的生命归零与恢复，不推断触发原因。"));
  controls->addWidget(smallTurnLabel_);
  auto* lifeLegend = new QLabel(QStringLiteral("<span style='color:#b23e51'>× 死亡</span>　<span style='color:#268559'>+ 复活</span>"), this);
  lifeLegend->setStyleSheet(QStringLiteral("font-size:11px")); controls->addWidget(lifeLegend);
  auto* microToggle = new QToolButton(this); microToggle->setObjectName(QStringLiteral("replayMicroToggle"));
  microToggle->setText(QStringLiteral("动作细节")); microToggle->setCheckable(true); controls->addWidget(microToggle); layout->addLayout(controls);
  layout->addWidget(slider_);
  microControls_ = new QWidget(this); microControls_->setObjectName(QStringLiteral("replayMicroControls"));
  auto* innerControls = new QHBoxLayout(microControls_); innerControls->setContentsMargins(0, 0, 0, 0);
  innerControls->addWidget(new QLabel(QStringLiteral("动作内部"), this));
  previousMoment_ = new QPushButton(QStringLiteral("上一小步"), this); previousMoment_->setObjectName(QStringLiteral("replayPreviousMoment"));
  nextMoment_ = new QPushButton(QStringLiteral("下一小步"), this); nextMoment_->setObjectName(QStringLiteral("replayNextMoment"));
  moments_ = new QComboBox(this); moments_->setObjectName(QStringLiteral("replayMoments"));
  innerControls->addWidget(previousMoment_); innerControls->addWidget(moments_, 1); innerControls->addWidget(nextMoment_);
  innerControls->addWidget(rounds_); innerControls->addWidget(jump_);
  auto* previousEffect = new QPushButton(QStringLiteral("上一作用"), this); auto* nextEffect = new QPushButton(QStringLiteral("下一作用"), this);
  nextEffect->setObjectName(QStringLiteral("replayNextEffect"));
  innerControls->addWidget(previousEffect); innerControls->addWidget(nextEffect);
  evidence_ = new QCheckBox(QStringLiteral("显示解析依据"), this); innerControls->addWidget(evidence_);
  evidence_->hide();
  animations_ = new QCheckBox(QStringLiteral("动态提示"), this); animations_->setChecked(true);
  animations_->setObjectName(QStringLiteral("replayAnimations")); opening->addWidget(animations_);
  auto* detailToggle = new QToolButton(this); detailToggle->setText(QStringLiteral("原始记录"));
  detailToggle->setObjectName(QStringLiteral("replayDetailsToggle")); detailToggle->setCheckable(true); opening->addWidget(detailToggle);
  layout->addWidget(microControls_); microControls_->hide();
  connect(microToggle, &QToolButton::toggled, microControls_, &QWidget::setVisible);
  auto* focusPanel = new QFrame(this); focusPanel->setObjectName(QStringLiteral("replayFocusPanel"));
  focusPanel->setStyleSheet(QStringLiteral("QFrame#replayFocusPanel{background:#ffffff;border:1px solid #dde5f0;border-radius:9px}"));
  auto* focusLayout = new QVBoxLayout(focusPanel); focusLayout->setContentsMargins(6, 2, 6, 2); focusLayout->setSpacing(0);
  castingPanel_ = new QFrame; castingPanel_->setObjectName(QStringLiteral("replayCastingPanel"));
  castingPanel_->setFixedHeight(34);
  castingPanel_->setStyleSheet(QStringLiteral("QFrame#replayCastingPanel{background:#f1f6ff;border:1px solid #8baee2;border-radius:4px}"));
  auto* castingLayout = new QVBoxLayout(castingPanel_); castingLayout->setContentsMargins(5, 1, 5, 1); castingLayout->setSpacing(0);
  auto* skillRow = new QHBoxLayout; skillRow->setSpacing(6);
  skillSide_ = new QLabel(castingPanel_); skillSide_->setObjectName(QStringLiteral("replaySkillSide"));
  skillSide_->setFixedSize(62, 23); skillSide_->setAlignment(Qt::AlignCenter); skillRow->addWidget(skillSide_);
  skillTitle_ = new QLabel(castingPanel_); skillTitle_->setObjectName(QStringLiteral("replaySkillTitle"));
  skillTitle_->setWordWrap(true); skillTitle_->setTextFormat(Qt::RichText); skillTitle_->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
  skillTitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  skillTitle_->setStyleSheet(QStringLiteral("font-size:14px;font-weight:700;background:transparent;border:0")); skillRow->addWidget(skillTitle_, 1);
  castingLayout->addLayout(skillRow, 1);
  auto* captionColumn = new QVBoxLayout; captionColumn->setSpacing(2);
  actionPhase_ = new QLabel(focusPanel); actionPhase_->setObjectName(QStringLiteral("replayActionPhase"));
  actionPhase_->setAlignment(Qt::AlignRight); actionPhase_->setStyleSheet(QStringLiteral("color:#52657e;font-size:11px")); captionColumn->addWidget(actionPhase_);
  auto* sourceLegend = new QLabel(QStringLiteral("箭头：<b style='color:#c93d50'>对手</b> · <b style='color:#278657'>队友/自身</b>　Buff 来源：<b style='color:#246bb0'>我方施加</b> · <b style='color:#b4425c'>敌方施加</b> · <span style='color:#697583'>来源未记</span>"), focusPanel);
  sourceLegend->setObjectName(QStringLiteral("replayBuffSourceLegend")); sourceLegend->setStyleSheet(QStringLiteral("font-size:10px")); captionColumn->addWidget(sourceLegend);
  auto* contextRow = new QHBoxLayout;
  configurationButton_ = new QToolButton(focusPanel); configurationButton_->setObjectName(QStringLiteral("replayConfiguration"));
  configurationButton_->setText(QStringLiteral("资料版本与原始配置")); configurationButton_->setToolTip(QStringLiteral("查看本场使用的原始配置及版本依据"));
  catalogReloadButton_ = new QToolButton(focusPanel); catalogReloadButton_->setObjectName(QStringLiteral("replayCatalogReload"));
  catalogReloadButton_->setText(QStringLiteral("重新读取资料"));
  catalogReloadButton_->setToolTip(QStringLiteral("只重新读取本地回放资料，不下载或更新官方数据。\n本场保留原资料；新资料就绪后可选择用于本场。"));
  connect(catalogReloadButton_, &QToolButton::clicked, this, &BattleReplayPage::catalogReloadRequested);
  catalogApplyButton_ = new QToolButton(focusPanel); catalogApplyButton_->setObjectName(QStringLiteral("replayCatalogApply"));
  catalogApplyButton_->setText(QStringLiteral("使用更新后的资料"));
  catalogApplyButton_->setToolTip(QStringLiteral("重新解释本场原始战报，并返回当前行动的开始；播放设置保持不变。"));
  connect(catalogApplyButton_, &QToolButton::clicked, this, [this] { useUpdatedCatalog(); });
  diagnosticsButton_ = new QToolButton(focusPanel); diagnosticsButton_->setObjectName(QStringLiteral("replayDiagnostics"));
  diagnosticsButton_->setText(QStringLiteral("本步核对"));
  referencesButton_ = new QToolButton(focusPanel); referencesButton_->setObjectName(QStringLiteral("replayReferences"));
  referencesButton_->setText(QStringLiteral("原始资料库"));
  referencesButton_->setToolTip(QStringLiteral("查询官方召唤、携带、布阵、养成、技能与资源资料"));
  auto* technicalMenu = new QMenu(focusPanel);
  for (auto* button : {configurationButton_, catalogReloadButton_, diagnosticsButton_, referencesButton_}) {
    button->hide();
    const auto label = button == diagnosticsButton_ ? QStringLiteral("解析核对与原始依据") : button->text();
    auto* action = technicalMenu->addAction(label);
    connect(action, &QAction::triggered, button, &QToolButton::click);
    connect(technicalMenu, &QMenu::aboutToShow, action, [action, button] { action->setEnabled(button->isEnabled()); });
  }
  auto* technicalButton = new QToolButton(focusPanel); technicalButton->setObjectName(QStringLiteral("replayTechnicalMenu"));
  technicalButton->setText(QStringLiteral("资料与原文")); technicalButton->setMenu(technicalMenu); technicalButton->setPopupMode(QToolButton::InstantPopup);
  technicalButton->setToolTip(QStringLiteral("按需查看原始资料、读取状态和解析依据")); contextRow->addWidget(technicalButton);
  catalogApplyButton_->setStyleSheet(QStringLiteral("QToolButton{color:#185da6;background:#e8f2ff;border:1px solid #bcd5f2;border-radius:5px;padding:4px 8px}"));
  contextRow->addWidget(catalogApplyButton_);
  numberFormat_ = new QComboBox(focusPanel); numberFormat_->setObjectName(QStringLiteral("replayNumberFormat"));
  numberFormat_->setAccessibleName(QStringLiteral("数字显示"));
  numberFormat_->addItem(QStringLiteral("数字：完整"), QStringLiteral("full"));
  numberFormat_->addItem(QStringLiteral("数字：亿 / 万"), QStringLiteral("yi-wan"));
  numberFormat_->setToolTip(QStringLiteral("切换生命、护盾、气势条和变化浮字的数字格式。\n亿 / 万按整数分段，不四舍五入：123456789 → 1亿2345万6789。\n切换不打断播放，原始日志和逐条记录保持完整数字。"));
  UiPreferences::bindComboBox(numberFormat_, QStringLiteral("battleReplay/numberFormat"));
  contextRow->addWidget(numberFormat_); contextRow->addStretch();
  contextRow->addLayout(captionColumn); focusLayout->addLayout(contextRow);
  catalogStatus_ = new QLabel(focusPanel); catalogStatus_->setObjectName(QStringLiteral("replayCatalogStatus"));
  catalogStatus_->setTextFormat(Qt::PlainText); catalogStatus_->setWordWrap(true);
  catalogStatus_->setStyleSheet(QStringLiteral("color:#617387;font-size:11px"));
  catalogStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse); focusLayout->addWidget(catalogStatus_);
  connect(configurationButton_, &QToolButton::clicked, this, [this] {
    unit_->setHtml(catalogStateHtml() + (replay_.steps.isEmpty() ? QString() : ReplayInspection::configurationHtml(replay_, catalog_)));
    openBoardDetail();
  });
  connect(diagnosticsButton_, &QToolButton::clicked, this, &BattleReplayPage::showCurrentDiagnostics);
  connect(referencesButton_, &QToolButton::clicked, this, [this] { showReferenceLibrary(); });
  phaseTrail_ = new QLabel(focusPanel); phaseTrail_->setObjectName(QStringLiteral("replayPhaseTrail"));
  phaseTrail_->setTextFormat(Qt::RichText); phaseTrail_->setWordWrap(true);
  phaseTrail_->setStyleSheet(QStringLiteral("font-size:11px;background:transparent;border:0"));
  contextRow->insertWidget(2, phaseTrail_, 1);
  phaseProgressBar_ = new QProgressBar(castingPanel_); phaseProgressBar_->setObjectName(QStringLiteral("replayPhaseProgress"));
  phaseProgressBar_->setRange(0, 1000); phaseProgressBar_->setTextVisible(false); phaseProgressBar_->setFixedHeight(3);
  phaseProgressBar_->setStyleSheet(QStringLiteral("QProgressBar{border:0;background:#dde5f3;border-radius:1px}QProgressBar::chunk{background:#3978ce;border-radius:1px}"));
  phaseProgressBar_->hide(); castingLayout->addWidget(phaseProgressBar_);
  stepLabel_ = new QLabel(this); stepLabel_->setTextFormat(Qt::PlainText); stepLabel_->setWordWrap(true);
  stepLabel_->setStyleSheet(QStringLiteral("color:#52657e;font-size:11px")); focusLayout->addWidget(stepLabel_);
  stepLabel_->hide();
  relation_ = new QLabel(focusPanel); relation_->setObjectName(QStringLiteral("replayRelation")); relation_->setTextFormat(Qt::PlainText); relation_->setWordWrap(true);
  relation_->setStyleSheet(QStringLiteral("font-size:12px;font-weight:600;color:#234780")); focusLayout->addWidget(relation_);
  outcome_ = new QLabel(focusPanel); outcome_->setObjectName(QStringLiteral("replayOutcome")); outcome_->setTextFormat(Qt::PlainText); outcome_->setWordWrap(true);
  outcome_->setStyleSheet(QStringLiteral("font-size:12px;color:#233650")); focusLayout->addWidget(outcome_);
  // The on-board cards and meters carry these results. Retain their complete
  // summaries for accessibility and the inspector without repeating two rows.
  relation_->hide(); outcome_->hide();
  connect(skillTitle_, &QLabel::linkActivated, this, [this](const QString& href) {
    const QUrl url(href); const auto id = QUrl::fromPercentEncoding(url.toEncoded().mid(url.scheme().size() + 1));
    if (url.scheme() == QStringLiteral("skill")) showSkill(id);
    else if (url.scheme() == QStringLiteral("buff")) showBuff(replay_.steps[current_].actor, id);
    else return;
    openBoardDetail();
  });
  auto* legend = new QLabel(QStringLiteral("<span style='color:#bf465b'>■ 生命</span>　<span style='color:#2d73b6'>■ 护盾余额</span>　<span style='color:#b7792c'>■ 气势</span>　<span style='color:#7462bb'>■ 通灵之魂</span>　<span style='color:#268554'>□ 召唤</span>　<span style='color:#b1487a'>□ 元能目标</span>"), focusPanel);
  legend->setStyleSheet(QStringLiteral("font-size:10px")); focusLayout->addWidget(legend);
  legend->hide();
  auto* vertical = new QSplitter(Qt::Horizontal, this); layout->addWidget(vertical, 1);
  auto* arenaScroll = arenaScroll_ = new QScrollArea(vertical); arenaScroll->setWidgetResizable(true); arenaScroll->setFrameShape(QFrame::NoFrame);
  auto* arena = new QWidget; arena_ = arena; arena->setObjectName(QStringLiteral("replayArena")); arena->setMinimumWidth(940);
  auto* arenaColumn = new QVBoxLayout(arena); arenaColumn->setContentsMargins(0, 0, 0, 0); arenaColumn->setSpacing(4);
  arenaColumn->addWidget(focusPanel);
  auto* board = new QWidget(arena); board->setObjectName(QStringLiteral("replayBoard")); arenaColumn->addWidget(board, 1);
  castingPanel_->setParent(board); castingPanel_->hide();
  auto* arenaLayout = new QHBoxLayout(board); arenaLayout->setContentsMargins(3, 0, 3, 0); arenaLayout->setSpacing(26);
  arenaScroll->setMinimumWidth(720);
  for (int side = 0; side < 2; ++side) {
    auto* panel = new QWidget(board); auto* box = new QVBoxLayout(panel); box->setContentsMargins(16, 0, 16, 0); box->setSpacing(3);
    teamLabels_[side] = new QLabel(panel); teamLabels_[side]->setTextFormat(Qt::PlainText); teamLabels_[side]->setWordWrap(true);
    teamLabels_[side]->setStyleSheet(side ? QStringLiteral("font-size:14px;font-weight:700;color:#a83957;padding:5px 0") : QStringLiteral("font-size:14px;font-weight:700;color:#2364aa;padding:5px 0"));
    auto* teamHeader = new QHBoxLayout;
    teamHeader->addWidget(teamLabels_[side], 1);
    players_[side] = new QComboBox(panel); players_[side]->setObjectName(QStringLiteral("replayPlayer%1").arg(side));
    players_[side]->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); players_[side]->setMinimumContentsLength(6);
    players_[side]->setToolTip(QStringLiteral("每次显示一位玩家的阵容；切换行动时跟随当前战斗段"));
    teamHeader->addWidget(players_[side]); box->addLayout(teamHeader);
    auto* teamDetails = new QToolButton(panel); teamDetails->setObjectName(QStringLiteral("replayTeamDetails%1").arg(side));
    teamDetails->setText(QStringLiteral("阵容资料")); teamHeader->addWidget(teamDetails);
    connect(teamDetails, &QToolButton::clicked, this, [this, side] { showTeamDetails(side); });
    queues_[side] = new QLabel(panel); queues_[side]->setObjectName(QStringLiteral("replayQueue%1").arg(side));
    queues_[side]->setTextFormat(Qt::RichText); queues_[side]->setWordWrap(true);
    queues_[side]->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    connect(queues_[side], &QLabel::linkActivated, this, [this, side](const QString&) {
      unit_->setHtml(queues_[side]->property("queueDetailHtml").toString()); openBoardDetail();
    });
    queues_[side]->setFixedHeight(20); queues_[side]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    queues_[side]->setStyleSheet(QStringLiteral("font-size:11px;color:#63738b")); box->addWidget(queues_[side]);
    auto* sourceRow = new QHBoxLayout; sourceRow->setSpacing(6);
    spirits_[side] = new ReplaySpiritBar(panel); spirits_[side]->setObjectName(QStringLiteral("replaySpirit%1").arg(side)); sourceRow->addWidget(spirits_[side], 1);
    teamSources_[side] = new TeamSourceCard(panel, [this, side] { showTeamDetails(side); });
    teamSources_[side]->setObjectName(QStringLiteral("replayTeamSource%1").arg(side));
    teamSources_[side]->hide(); sourceRow->addWidget(teamSources_[side]); box->addLayout(sourceRow);
    connect(players_[side], &QComboBox::activated, this, [this] { followPlayers_ = false; render(); });
    auto* grid = new QGridLayout; grid->setHorizontalSpacing(16); grid->setVerticalSpacing(50);
    grid->setContentsMargins(0, 50, 0, 16); grid->setAlignment(Qt::AlignTop);
    for (int cell = 0; cell < 9; ++cell) {
      auto* view = new ReplayCell(side, panel); cells_[side * 9 + cell] = view;
      view->setObjectName(QStringLiteral("replayCell%1").arg(side * 9 + cell));
      grid->addWidget(view, cell / 3, cell % 3);
      connect(view, &ReplayCell::unitActivated, this, [this](const QString& key) { showUnit(key); openBoardDetail(); });
      connect(view, &ReplayCell::diagnosticActivated, this, [this](const QString& key) { showUnit(key); openBoardDetail(); });
      connect(view, &ReplayCell::buffActivated, this, [this](const QString& key, const QString& id) { showBuff(key, id); openBoardDetail(); });
      connect(view, &ReplayCell::buffExpansionChanged, this, [this](bool expanded) { if (expanded) pause(); });
      connect(view, &ReplayCell::unitSelectionChanged, this, [this](const QString&) { pause(); meterOverlay_->update(); });
      connect(view, &ReplayCell::effectActivated, this, [this](int index) {
        // Rendering replaces the clicked card; wait for its mouse event to
        // finish, and ignore the click if another log/action was opened.
        QTimer::singleShot(0, this, [this, index, step = current_, hash = replay_.sha256] {
          if (current_ == step && replay_.sha256 == hash) selectEffect(index);
        });
      });
      connect(view, &ReplayCell::recordedEffectActivated, this, [this](int recordStep, int index) {
        QTimer::singleShot(0, this, [this, recordStep, index, current = current_, hash = replay_.sha256] {
          if (current_ != current || replay_.sha256 != hash || recordStep < 0 || recordStep >= replay_.steps.size() ||
              index < 0 || index >= replay_.steps[recordStep].effects.size()) return;
          seek(recordStep); selectEffect(index);
        });
      });
    }
    box->addLayout(grid, 1);
    dragonBars_[side] = new QLabel(panel); dragonBars_[side]->setObjectName(QStringLiteral("replayDragons%1").arg(side));
    dragonBars_[side]->setTextFormat(Qt::RichText); dragonBars_[side]->setWordWrap(true);
    dragonBars_[side]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); dragonBars_[side]->setFixedHeight(36);
    dragonBars_[side]->setStyleSheet(QStringLiteral("background:#edf5f4;border:1px solid #d3e2df;border-radius:4px;padding:2px;color:#3e6b66;font-size:11px"));
    connect(dragonBars_[side], &QLabel::linkActivated, this, [this, side](const QString&) {
      showTeamDetails(side);
    });
    dragonBars_[side]->hide(); box->addWidget(dragonBars_[side]);
    teamBuffs_[side] = browser(panel); teamBuffs_[side]->setObjectName(QStringLiteral("teamBuffs%1").arg(side));
    teamBuffs_[side]->setFixedHeight(38); teamBuffs_[side]->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    teamBuffs_[side]->document()->setDocumentMargin(2);
    teamBuffs_[side]->setStyleSheet(QStringLiteral("QTextBrowser{background:#f3f5fa;border:1px solid #dce3ef;border-radius:4px;font-size:11px}"));
    connect(teamBuffs_[side], &QTextBrowser::anchorClicked, this, [this, side](const QUrl& url) {
      if (url.scheme() == QStringLiteral("team-effects")) {
        unit_->setHtml(teamBuffs_[side]->property("completeHtml").toString()); openBoardDetail(); return;
      }
      const auto id = QUrl::fromPercentEncoding(url.toEncoded().mid(url.scheme().size() + 1));
      if (url.scheme() == QStringLiteral("skill")) showSkill(id);
      else if (url.scheme() == QStringLiteral("buff")) {
        const auto key = teamBuffs_[side]->property("ownerKey").toString(); showBuff(key, id);
      } else if (url.scheme() == QStringLiteral("unit")) showUnit(id);
      else return;
      openBoardDetail();
    });
    box->addWidget(teamBuffs_[side]);
    teamEffects_[side] = new QLabel(panel); teamEffects_[side]->setTextFormat(Qt::RichText); teamEffects_[side]->setWordWrap(true);
    teamEffects_[side]->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByMouse);
    connect(teamEffects_[side], &QLabel::linkActivated, this, [this](const QString& url) { showUnit(QUrl::fromPercentEncoding(url.mid(5).toLatin1())); openBoardDetail(); });
    box->addWidget(teamEffects_[side]); arenaLayout->addWidget(panel, 1);
  }
  links_ = new ReplayLinks(board);
  meterOverlay_ = new ReplayMeterOverlay(board);
  const auto applyNumberFormat = [this] {
    const bool grouped = numberFormat_->currentData() == QStringLiteral("yi-wan");
    for (auto* cell : cells_) cell->setGroupedNumbers(grouped);
    meterOverlay_->setGroupedNumbers(grouped);
  };
  applyNumberFormat();
  connect(numberFormat_, &QComboBox::currentIndexChanged, this, applyNumberFormat);
  boardInspector_ = new QFrame(arena); boardInspector_->setObjectName(QStringLiteral("replayBoardInspector"));
  boardInspector_->setStyleSheet(QStringLiteral("QFrame#replayBoardInspector{background:#ffffff;border:2px solid #7da2d9;border-radius:8px}"));
  auto* inspectorLayout = new QVBoxLayout(boardInspector_); inspectorLayout->setContentsMargins(12, 8, 12, 10);
  auto* inspectorHeader = new QHBoxLayout;
  inspectorHeader->addWidget(new QLabel(QStringLiteral("战场详情与官方资料"), boardInspector_), 1);
  auto* closeInspector = new QToolButton(boardInspector_); closeInspector->setText(QStringLiteral("关闭 ×"));
  closeInspector->setObjectName(QStringLiteral("replayCloseInspector")); inspectorHeader->addWidget(closeInspector);
  inspectorLayout->addLayout(inspectorHeader);
  referenceControls_ = new QWidget(boardInspector_); referenceControls_->setObjectName(QStringLiteral("replayReferenceControls"));
  auto* referenceLayout = new QHBoxLayout(referenceControls_); referenceLayout->setContentsMargins(0, 0, 0, 0);
  referenceCategory_ = new QComboBox(referenceControls_); referenceCategory_->setObjectName(QStringLiteral("replayReferenceCategory"));
  for (const auto& item : ReplayReference::categories()) referenceCategory_->addItem(item.second, item.first);
  referenceLayout->addWidget(referenceCategory_);
  referenceSearch_ = new QLineEdit(referenceControls_); referenceSearch_->setObjectName(QStringLiteral("replayReferenceSearch"));
  referenceSearch_->setPlaceholderText(QStringLiteral("搜索名称、编号、说明")); referenceSearch_->setClearButtonEnabled(true);
  referenceLayout->addWidget(referenceSearch_, 1); inspectorLayout->addWidget(referenceControls_); referenceControls_->hide();
  connect(referenceCategory_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { showReferenceLibrary(referenceCategory_->currentData().toString()); });
  connect(referenceSearch_, &QLineEdit::textChanged, this, [this] { showReferenceLibrary(referenceCategory_->currentData().toString()); });
  boardDetails_ = browser(boardInspector_);
  boardDetails_->setObjectName(QStringLiteral("replayBoardDetails")); inspectorLayout->addWidget(boardDetails_, 1);
  connect(closeInspector, &QToolButton::clicked, boardInspector_, &QWidget::hide); boardInspector_->hide();
  arenaScroll->setWidget(arena);
  auto* bottom = new QWidget(vertical); auto* bottomLayout = new QVBoxLayout(bottom);
  detailPane_ = bottom; detailPane_->setObjectName(QStringLiteral("replayDetailPane"));
  bottomLayout->setContentsMargins(0, 0, 0, 0); bottomLayout->setSpacing(4);
  bottom->setMinimumWidth(300);
  auto* filters = new QVBoxLayout;
  unitFilter_ = new QComboBox(bottom); unitFilter_->setObjectName(QStringLiteral("replayUnitFilter"));
  unitFilter_->addItem(QStringLiteral("全部单位"), QString()); unitFilter_->setMinimumContentsLength(14);
  unitFilter_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  important_ = new QCheckBox(QStringLiteral("只看关键变化"), bottom); important_->setObjectName(QStringLiteral("replayImportant"));
  important_->setToolTip(QStringLiteral("生命归零、生命从零恢复、登场、变身、属性/生命上限变化、玩家换位或移除"));
  filterCount_ = new QLabel(bottom); filterCount_->setTextFormat(Qt::PlainText);
  eventType_ = new QComboBox(bottom); eventType_->setObjectName(QStringLiteral("replayEventType"));
  eventType_->addItems({QStringLiteral("全部关键事件"), QStringLiteral("生命归零"), QStringLiteral("登场"), QStringLiteral("生命恢复"), QStringLiteral("变身"), QStringLiteral("属性变化"), QStringLiteral("生命上限变化"), QStringLiteral("玩家换位"), QStringLiteral("玩家移除")});
  auto* previousEvent = new QPushButton(QStringLiteral("上个事件"), bottom);
  auto* nextEvent = new QPushButton(QStringLiteral("下个事件"), bottom); nextEvent->setObjectName(QStringLiteral("replayNextEvent"));
  filters->addWidget(unitFilter_);
  auto* filterOptions = new QHBoxLayout; filterOptions->addWidget(important_); filterOptions->addWidget(filterCount_); filters->addLayout(filterOptions);
  auto* jumps = new QHBoxLayout; jumps->addWidget(eventType_, 1); jumps->addWidget(previousEvent); jumps->addWidget(nextEvent); filters->addLayout(jumps);
  bottomLayout->addLayout(filters);
  auto* lower = new QSplitter(Qt::Vertical, bottom); bottomLayout->addWidget(lower, 1);
  timeline_ = new QTableView(lower); timeline_->setObjectName(QStringLiteral("replayTimeline"));
  stepsModel_ = new ReplayStepModel(this); stepsProxy_ = new ReplayStepFilter(this); stepsProxy_->setSourceModel(stepsModel_); timeline_->setModel(stepsProxy_);
  timeline_->setSelectionBehavior(QAbstractItemView::SelectRows); timeline_->setSelectionMode(QAbstractItemView::SingleSelection);
  timeline_->setEditTriggers(QAbstractItemView::NoEditTriggers); timeline_->setAlternatingRowColors(true); timeline_->setWordWrap(false);
  timeline_->verticalHeader()->hide(); timeline_->verticalHeader()->setDefaultSectionSize(29);
  timeline_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents); timeline_->horizontalHeader()->setStretchLastSection(true);
  auto* tabs = new QTabWidget(lower); tabs_ = tabs;
  overview_ = browser(tabs); overview_->setObjectName(QStringLiteral("replayOverview"));
  tabs->addTab(overview_, QStringLiteral("概览"));
  auto* effectPage = new QWidget(tabs); auto* effectLayout = new QVBoxLayout(effectPage); effectLayout->setContentsMargins(3, 3, 3, 3); effectLayout->setSpacing(3);
  auto* effectHelp = new QLabel(QStringLiteral("点击一条作用，定位到发生的小步骤"), effectPage); effectHelp->setWordWrap(true); effectHelp->setStyleSheet(QStringLiteral("color:#586c87;font-size:11px")); effectLayout->addWidget(effectHelp);
  allEffects_ = new QCheckBox(QStringLiteral("包括无变化记录"), effectPage); allEffects_->setObjectName(QStringLiteral("replayAllEffects")); effectLayout->addWidget(allEffects_);
  effectList_ = new QListWidget(effectPage); effectList_->setObjectName(QStringLiteral("replayEffects")); effectList_->setWordWrap(true); effectList_->setSpacing(4);
  effectList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  effectList_->setStyleSheet(QStringLiteral("QListWidget{background:#f7f9fc;border:0} QListWidget::item{background:white;border:1px solid #dae2ef;border-radius:4px;padding:6px} QListWidget::item:selected{background:#e9efff;color:#234780;border:1px solid #88a0d5}"));
  effectLayout->addWidget(effectList_, 1); tabs->addTab(effectPage, QStringLiteral("作用"));
  details_ = browser(tabs); details_->setObjectName(QStringLiteral("replayDetails")); tabs->addTab(details_, QStringLiteral("行动说明"));
  unit_ = browser(tabs); unit_->setObjectName(QStringLiteral("replayUnit")); tabs->addTab(unit_, QStringLiteral("单位详情"));
  rawStep_ = new QPlainTextEdit(tabs); rawStep_->setReadOnly(true); tabs->addTab(rawStep_, QStringLiteral("本条原文"));
  auto* fieldPage = new QWidget(tabs); auto* fieldLayout = new QVBoxLayout(fieldPage); fieldLayout->setContentsMargins(4, 4, 4, 4);
  search_ = new QLineEdit(fieldPage); search_->setPlaceholderText(QStringLiteral("检索全部字段、数值或“待核实”")); search_->setObjectName(QStringLiteral("replaySearch")); fieldLayout->addWidget(search_);
  auto* fields = new QTableView(fieldPage); fields->setObjectName(QStringLiteral("replayFields"));
  fieldsModel_ = new ReplayFieldModel(this); fieldsProxy_ = new QSortFilterProxyModel(this);
  fieldsProxy_->setSourceModel(fieldsModel_); fieldsProxy_->setFilterKeyColumn(-1); fieldsProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
  fields->setModel(fieldsProxy_); fields->setWordWrap(false); fields->setSelectionBehavior(QAbstractItemView::SelectRows);
  fields->verticalHeader()->hide(); fields->verticalHeader()->setDefaultSectionSize(28); fields->setColumnWidth(0, 260); fields->setColumnWidth(1, 200); fields->horizontalHeader()->setStretchLastSection(true);
  auto* fieldValue = new QPlainTextEdit(fieldPage); fieldValue->setReadOnly(true); fieldValue->setMaximumHeight(100); fieldLayout->addWidget(fields, 1); fieldLayout->addWidget(fieldValue);
  connect(fields->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this, fieldValue](const QModelIndex& index) {
    const auto source = fieldsProxy_->mapToSource(index); if (!source.isValid()) return;
    const auto& f = replay_.fields[source.row()]; fieldValue->setPlainText(f.path + QLatin1Char('\n') + pretty(f.value));
  });
  tabs->addTab(fieldPage, QStringLiteral("全部字段"));
  rawAll_ = new QPlainTextEdit(tabs); rawAll_->setReadOnly(true); tabs->addTab(rawAll_, QStringLiteral("完整日志"));
  notes_ = browser(tabs); tabs->addTab(notes_, QStringLiteral("核对与来源"));
  lower->setSizes({180, 430}); vertical->setSizes({840, 320});
  detailPane_->hide();
  connect(detailToggle, &QToolButton::toggled, this, [this, vertical](bool shown) {
    detailPane_->setVisible(shown); evidence_->setVisible(shown);
    if (shown) vertical->setSizes({std::max(720, vertical->width() - 330), 330});
  });
  connect(animations_, &QCheckBox::toggled, this, [this] { render(); updatePlaybackProgress(playbackProgress_); });
  phaseAnimation_ = new QVariantAnimation(this); phaseAnimation_->setObjectName(QStringLiteral("replayPhaseAnimation"));
  phaseAnimation_->setStartValue(0.0); phaseAnimation_->setEndValue(1.0);
  connect(phaseAnimation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) { updatePlaybackProgress(value.toReal()); });
  connect(phaseAnimation_, &QVariantAnimation::finished, this, &BattleReplayPage::finishPlaybackPhase);
  timer_ = new QTimer(this); timer_->setObjectName(QStringLiteral("replayPlaybackTimer")); timer_->setInterval(speed->currentData().toInt());
  connect(speed, &QComboBox::currentIndexChanged, this, [this, speed] {
    timer_->setInterval(speed->currentData().toInt());
    if (playbackPhase_ < 0) return;
    const qreal progress = playbackProgress_;
    // Change the presentation speed without restarting the current phase.
    { QSignalBlocker blocker(phaseAnimation_);
      phaseAnimation_->setDuration(playbackDuration());
      phaseAnimation_->setCurrentTime(qRound(progress * phaseAnimation_->duration()));
    }
    updatePlaybackProgress(progress);
  });
  connect(timer_, &QTimer::timeout, this, [this] {
    if (playbackMode_->currentData() != QStringLiteral("effect")) { timer_->stop(); return; }
    if (!advanceEffect(1)) { pause(); return; }
    timer_->start(); play_->setText(QStringLiteral("暂停"));
  });
  connect(playbackMode_, &QComboBox::currentIndexChanged, this, [this, microToggle] {
    clearPlaybackPhase(); const bool detailed = playbackMode_->currentData() == QStringLiteral("effect");
    microToggle->setChecked(detailed);
    render();
  });
  connect(open, &QPushButton::clicked, this, [this] { const auto file = QFileDialog::getOpenFileName(this, QStringLiteral("打开战斗日志"), reportDirectory_, QStringLiteral("战斗日志 (*.txt *.json);;所有文件 (*)")); if (!file.isEmpty()) loadFile(file); });
  connect(folder, &QPushButton::clicked, this, [this] { const auto path = QFileDialog::getExistingDirectory(this, QStringLiteral("选择 brush 样本文件夹"), reportDirectory_); if (!path.isEmpty()) loadDirectory(path); });
  connect(files_, &QComboBox::activated, this, [this](int index) { loadFile(files_->itemData(index).toString()); });
  connect(side_, &QComboBox::currentIndexChanged, this, [this] { followPlayers_ = true; render(); });
  connect(rounds_, &QComboBox::activated, this, [this](int index) { seekActionStart(rounds_->itemData(index).toInt()); });
  connect(play_, &QPushButton::clicked, this, [this] {
    if (playing_ || timer_->isActive()) { pause(); return; }
    if (stepsProxy_->rowCount() == 0) return;
    const bool detailed = playbackMode_->currentData() == QStringLiteral("effect");
    if (!detailed) { startContinuousPlayback(); return; }
    const bool moreEffects = detailed && nextEffectIndex(1) >= 0;
    if (!moreEffects && nextVisibleStep(1) < 0) seek(stepsProxy_->mapToSource(stepsProxy_->index(0, 0)).row());
    timer_->start(); play_->setText(QStringLiteral("暂停"));
  });
  connect(previous_, &QPushButton::clicked, this, [this] { pause(); const auto n = nextVisibleStep(-1); if (n >= 0) seek(n); });
  connect(next_, &QPushButton::clicked, this, [this] { pause(); const auto n = nextVisibleStep(1); if (n >= 0) seek(n); });
  connect(restart_, &QPushButton::clicked, this, [this] { pause(); seek(0); });
  connect(slider_, &QSlider::valueChanged, this, &BattleReplayPage::seekActionStart);
  connect(slider_, &ReplayLifeTimeline::actionStartRequested, this, &BattleReplayPage::seekActionStart);
  connect(slider_, &ReplayLifeTimeline::inspectionStarted, this, &BattleReplayPage::pause);
  connect(slider_, &ReplayLifeTimeline::layoutModeChanged, this, &BattleReplayPage::fitBoardHeight, Qt::QueuedConnection);
  connect(timelineView_, &QComboBox::currentIndexChanged, this, [this] {
    slider_->setUnitView(timelineView_->currentData() == QStringLiteral("units"));
  });
  connect(jump_, &QSpinBox::valueChanged, this, [this](int n) { pause(); seek(n); });
  connect(moments_, &QComboBox::activated, this, [this](int n) { pause(); seekMoment(moments_->itemData(n).toInt()); });
  connect(previousMoment_, &QPushButton::clicked, this, [this] {
    pause(); seekMoment(moment_ < 0 ? static_cast<int>(replay_.steps[current_].moments.size()) - 1 : moment_ - 1);
  });
  connect(nextMoment_, &QPushButton::clicked, this, [this] { pause(); seekMoment(moment_ + 1); });
  connect(previousEffect, &QPushButton::clicked, this, [this] { pause(); advanceEffect(-1); });
  connect(nextEffect, &QPushButton::clicked, this, [this] { pause(); advanceEffect(1); });
  connect(timeline_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex& index) { const auto original = stepsProxy_->mapToSource(index); if (original.isValid() && original.row() != current_) { pause(); seek(original.row()); } });
  connect(unitFilter_, &QComboBox::currentIndexChanged, this, &BattleReplayPage::updateTimelineFilter);
  connect(important_, &QCheckBox::toggled, this, &BattleReplayPage::updateTimelineFilter);
  connect(evidence_, &QCheckBox::toggled, this, [this] { render(); });
  connect(allEffects_, &QCheckBox::toggled, this, [this](bool showAll) {
    if (!showAll && !replay_.steps.isEmpty() && effect_ >= 0 && !meaningfulEffect(replay_.steps[current_].effects[effect_])) effect_ = -1;
    render();
  });
  connect(effectList_, &QListWidget::currentRowChanged, this, [this](int row) { if (auto* item = effectList_->item(row); item && item->data(Qt::UserRole).isValid()) selectEffect(item->data(Qt::UserRole).toInt()); });
  connect(overview_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
    if (url.scheme() == QStringLiteral("effect")) {
      bool ok = false; const auto index = url.toString().section(QLatin1Char(':'), 1).toInt(&ok);
      if (ok) selectEffect(index);
    }
  });
  connect(previousEvent, &QPushButton::clicked, this, [this] { jumpEvent(-1); });
  connect(nextEvent, &QPushButton::clicked, this, [this] { jumpEvent(1); });
  connect(search_, &QLineEdit::textChanged, fieldsProxy_, &QSortFilterProxyModel::setFilterFixedString);
  connect(details_, &QTextBrowser::anchorClicked, this, [this, tabs](const QUrl& url) {
    if (url.scheme() == QStringLiteral("unit")) { showUnit(QUrl::fromPercentEncoding(url.toString(QUrl::FullyEncoded).mid(5).toLatin1())); tabs->setCurrentWidget(unit_); }
  });
  connect(unit_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
    PublicRecordKey publicKey;
    if (PublicDataDetailLinks::decode(url,&publicKey)) {
      PublicDataDetailDialog::open({{},replayCatalog_ ? replayCatalog_ : latestCatalog_},publicKey,this); return;
    }
    const auto id = QUrl::fromPercentEncoding(url.toString(QUrl::FullyEncoded).section(QLatin1Char(':'), 1).toLatin1());
    if (!url.scheme().startsWith(QStringLiteral("reference"))) referenceControls_->hide();
    if (url.scheme() == QStringLiteral("skill")) showSkill(id);
    else if (url.scheme() == QStringLiteral("buff")) showBuff(selectedUnit_, id);
    else if (url.scheme() == QStringLiteral("buff-raw")) showBuff(selectedUnit_, id, true);
    else if (url.scheme() == QStringLiteral("unit-raw")) showUnit(id, true);
    else if (url.scheme() == QStringLiteral("skill-raw")) showSkill(id, true);
    else if (url.scheme() == QStringLiteral("team-raw")) showTeamDetails(id.toInt(), true);
    else if (url.scheme() == QStringLiteral("team")) showTeamDetails(id.toInt());
    else if (url.scheme() == QStringLiteral("source-raw")) {
      unit_->setHtml(QStringLiteral("<h3>原始接收状态</h3><p>%1</p><p>%2</p>")
        .arg(esc(sourceStatus_).replace(QLatin1Char('\n'), QStringLiteral("<br>")), esc(accountEvidence_)));
      for (const auto& item : incomingReplays_)
        unit_->append(QStringLiteral("<p>%1<br>SHA-256：%2<br>%3 字节</p>").arg(esc(item.label), esc(item.sha256)).arg(item.bytes.size()));
    }
    else if (url.scheme() == QStringLiteral("buff-record")) {
      const auto parts = url.toString().split(QLatin1Char(':'));
      bool stepOk = false, effectOk = false;
      const int step = parts.value(1).toInt(&stepOk), effect = parts.value(2).toInt(&effectOk);
      if (stepOk && effectOk && step >= 0 && step < replay_.steps.size() &&
          (effect == -1 || (effect >= 0 && effect < replay_.steps[step].effects.size()))) {
        pause(); seek(step); if (effect >= 0) selectEffect(effect);
      }
    }
    else if (url.scheme() == QStringLiteral("mechanics")) showPetMechanics(id);
    else if (url.scheme() == QStringLiteral("unit")) showUnit(id);
    else if (url.scheme() == QStringLiteral("incoming")) openIncomingReplay(id);
    else if (url.scheme() == QStringLiteral("reference-list")) {
      { QSignalBlocker block(referenceSearch_); referenceSearch_->clear(); }
      showReferenceLibrary(id);
    } else if (url.scheme() == QStringLiteral("reference-page")) showReferenceLibrary(referenceCategory_->currentData().toString(), id.toInt());
    else if (url.scheme() == QStringLiteral("reference") || url.scheme() == QStringLiteral("reference-raw"))
      showReferenceRecord(id.section(QLatin1Char('/'), 0, 0), id.section(QLatin1Char('/'), 1), url.scheme() == QStringLiteral("reference-raw"));
  });
  connect(boardDetails_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
    PublicRecordKey key;
    if (PublicDataDetailLinks::decode(url,&key)) {
      PublicDataDetailDialog::open({{},replayCatalog_ ? replayCatalog_ : latestCatalog_},key,this); return;
    }
    QMetaObject::invokeMethod(unit_, "anchorClicked", Q_ARG(QUrl, url));
    boardDetails_->setHtml(unit_->toHtml());
  });
  setCatalogState(std::move(initialCatalog), {}, false); updateNotes(); render();
}

void BattleReplayPage::clearReferenceCatalogCache() {
  referenceRows_ = {}; referenceRowsCategory_.clear(); referenceSortedIds_.clear(); referenceMatches_.clear();
  referenceSearchIndex_.clear(); referenceQuery_.clear();
}
void BattleReplayPage::setCatalogState(BattleReplayCatalogHandle snapshot, const QString& message, bool busy) {
  // Failure/progress notifications may carry no snapshot. Never discard the
  // last usable catalog, and never replace the open replay's bound catalog.
  const auto previous = latestCatalog_;
  if (snapshot && !snapshot->root.isEmpty()) latestCatalog_ = std::move(snapshot);
  catalogMessage_ = message; catalogBusy_ = busy;
  if (replay_.steps.isEmpty() && !sameCatalogContent(previous, latestCatalog_)) {
    catalog_ = latestCatalog_ ? latestCatalog_->root : QJsonObject{};
    clearReferenceCatalogCache(); unit_->clear(); boardDetails_->clear(); boardInspector_->hide(); updateNotes();
  }
  updateCatalogStatus();
}
void BattleReplayPage::updateCatalogStatus() {
  QStringList text;
  if (catalogBusy_) text << QStringLiteral("正在读取本地资料…");
  else if (!catalogMessage_.isEmpty() && (!latestCatalog_ || catalogMessage_ != latestCatalog_->sourceLabel + QStringLiteral("已就绪")))
    text << catalogMessage_;
  if (!latestCatalog_) {
    text << QStringLiteral("资料未就绪；请重新读取资料后再打开战报。");
  } else if (replayCatalog_) {
    text << QStringLiteral("本场资料：%1").arg(catalogSourceName(replayCatalog_));
    if (!sameCatalogContent(replayCatalog_, latestCatalog_))
      text << QStringLiteral("新资料已就绪，下次打开战报使用；本场资料保持固定。");
  } else text << QStringLiteral("资料：%1").arg(catalogSourceName(latestCatalog_));
  if (latestCatalog_ && !latestCatalog_->warning.isEmpty()) text << latestCatalog_->warning;
  text.removeDuplicates();
  catalogStatus_->setProperty("rawEvidence", text.join(QStringLiteral(" · ")));
  QString publicStatus = catalogBusy_ ? QStringLiteral("正在读取本地资料…") : !latestCatalog_ ? QStringLiteral("资料未就绪，请重新读取资料。") : QStringLiteral("回放资料已就绪");
  if (!catalogBusy_ && catalogMessage_.contains(QStringLiteral("失败"))) publicStatus = QStringLiteral("资料读取失败；已有资料继续使用。详情见“资料与原文”。");
  if (replayCatalog_ && !sameCatalogContent(replayCatalog_, latestCatalog_)) publicStatus += QStringLiteral(" · 新资料已就绪，可用于本场或下次打开战报");
  if (latestCatalog_ && !latestCatalog_->warning.isEmpty()) publicStatus += QStringLiteral(" · 资料有待核对项，详见原始配置");
  catalogStatus_->setText(publicStatus); catalogStatus_->setToolTip(publicStatus);
  catalogStatus_->setVisible(catalogBusy_ || !latestCatalog_ || publicStatus != QStringLiteral("回放资料已就绪"));
  catalogReloadButton_->setEnabled(!catalogBusy_);
  catalogReloadButton_->setText(catalogBusy_ ? QStringLiteral("读取中…") : QStringLiteral("重新读取资料"));
  catalogApplyButton_->setVisible(replayCatalog_ && !sameCatalogContent(replayCatalog_, latestCatalog_));
  catalogApplyButton_->setEnabled(!catalogBusy_ && latestCatalog_ && !replay_.steps.isEmpty());
  referencesButton_->setEnabled(!catalog_.isEmpty());
}
QString BattleReplayPage::catalogStateHtml() const {
  QString html = QStringLiteral("<h3>回放资料读取状态</h3><p>%1</p><p>“重新读取资料”只读取本地已有资料，不下载或更新官方数据。已打开的战报固定使用打开时的资料，新资料用于下一次打开。</p>")
    .arg(esc(catalogStatus_->property("rawEvidence").toString()));
  const auto describe = [](const BattleReplayCatalogHandle& value, const QString& heading) {
    if (!value) return QString();
    QString result = QStringLiteral("<h4>%1</h4><p>来源：%2<br>路径：%3<br>SHA-256：%4<br>本地读取批次：%5<br>读取时间：%6</p>")
      .arg(esc(heading), esc(catalogSourceName(value)), esc(value->sourcePath),
        esc(value->sha256.isEmpty() ? QStringLiteral("未提供") : value->sha256))
      .arg(value->revision).arg(esc(value->loadedAt.isValid() ? value->loadedAt.toLocalTime().toString(Qt::ISODate) : QStringLiteral("未提供")));
    if (!value->warning.isEmpty()) result += QStringLiteral("<p>%1</p>").arg(esc(value->warning));
    return result;
  };
  html += describe(replayCatalog_ ? replayCatalog_ : latestCatalog_, replayCatalog_ ? QStringLiteral("本场固定资料") : QStringLiteral("当前可用资料"));
  if (replayCatalog_ && latestCatalog_ && !sameCatalogContent(replayCatalog_, latestCatalog_))
    html += describe(latestCatalog_, QStringLiteral("下次打开战报使用的资料"));
  return html;
}

void BattleReplayPage::pause() {
  timer_->stop(); playing_ = false; setProperty("playbackRunning", false);
  if (phaseAnimation_->state() == QAbstractAnimation::Running) phaseAnimation_->pause();
  play_->setText(playbackMode_->currentData() == QStringLiteral("effect") ? QStringLiteral("逐条播放") :
    playbackPhase_ >= 0 && playbackProgress_ < 1 ? QStringLiteral("继续播放") : QStringLiteral("连续播放"));
}
QString BattleReplayPage::playbackPhaseKind() const {
  return playbackPhase_ >= 0 && playbackPhase_ < playbackPhases_.size() ? playbackPhases_[playbackPhase_].kind : QString();
}
void BattleReplayPage::clearPlaybackPhase() {
  pause(); phaseAnimation_->stop(); playbackPhase_ = -1; playbackCompleted_ = false;
  playbackProgress_ = 1;
  for (auto* cell : cells_) cell->setPlaybackDriven(false);
  for (auto* spirit : spirits_) spirit->setPlaybackDriven(false);
  links_->setPlaybackDriven(false); phaseProgressBar_->hide(); phaseTrail_->clear();
  setProperty("playbackPhaseKind", QString());
  setProperty("playbackFirstMoment", -1); setProperty("playbackLastMoment", -1);
  setProperty("playbackActionProgress", 0.0); setProperty("playbackBaseDurationMs", 0);
  setProperty("numericSegment", 0); setProperty("numericSegmentCount", 0);
}
void BattleReplayPage::rebuildPlaybackPhases() {
  playbackPhases_.clear(); meterEventGroups_.clear();
  if (replay_.steps.isEmpty()) return;
  const auto& step = replay_.steps[current_];
  if (step.moments.isEmpty()) {
    PlaybackPhase phase; phase.kind = QStringLiteral("finish"); phase.title = step.title; phase.durationMs = 600;
    playbackPhases_.append(phase); return;
  }
  QList<int> targets;
  for (int i = 0; i < step.effects.size(); ++i)
    if (step.effects[i].kind == QStringLiteral("target") && !inactiveTargetRecord(step.effects[i])) targets.append(i);
  const bool unresolved = std::any_of(step.moments.begin(), step.moments.end(), [](const Moment& m) { return m.phase == QStringLiteral("unresolved"); });
  const auto execution = catalog_[QStringLiteral("execution")].toObject()[valueText(step.raw.toObject()[QStringLiteral("ses")])].toObject();
  const bool attackDisabled = step.playbackKind == QStringLiteral("disabled") || valueText(execution[QStringLiteral("type")]).toInt() == 2;
  const bool teamSimple = step.playbackKind == QStringLiteral("team-simple");
  const bool hasAttack = !targets.isEmpty() || std::any_of(step.moments.begin(), step.moments.end(), [](const Moment& m) { return m.phase == QStringLiteral("attack"); });
  PlaybackPhase prepare; prepare.kind = QStringLiteral("prepare");
  prepare.title = attackDisabled ? (step.executionName.isEmpty() ? QStringLiteral("行动受限") : step.executionName) : teamSimple ? QStringLiteral("队伍触发") : QStringLiteral("准备施放");
  prepare.actor = step.actor; prepare.firstMoment = prepare.lastMoment = prepare.beforeMoment = 0; prepare.durationMs = 140;
  playbackPhases_.append(prepare);
  int cursor = 0;
  bool castAdded = false, attackAdded = false;
  const auto addCast = [&](const QString& actor, bool counter) {
    PlaybackPhase phase; phase.kind = counter ? QStringLiteral("counter-cast") : QStringLiteral("cast");
    phase.title = counter ? QStringLiteral("发起反击") : QStringLiteral("施放技能"); phase.actor = actor;
    phase.firstMoment = phase.lastMoment = phase.beforeMoment = cursor;
    phase.durationMs = counter ? 280 : 260; phase.targets = true; playbackPhases_.append(phase);
  };
  const auto ensureAttack = [&] {
    if (attackAdded || !hasAttack || unresolved || teamSimple) return;
    if (!attackDisabled && !castAdded) { addCast(step.actor, false); castAdded = true; }
    PlaybackPhase phase; phase.kind = attackDisabled ? QStringLiteral("settle") : QStringLiteral("attack");
    phase.title = attackDisabled ? QStringLiteral("目标记录 · 未出手") : QStringLiteral("技能结果");
    phase.actor = step.actor; phase.firstMoment = phase.lastMoment = phase.beforeMoment = cursor;
    phase.effects = targets; phase.targets = !attackDisabled; phase.outcomes = true; phase.durationMs = 850;
    playbackPhases_.append(phase); attackAdded = true;
  };
  for (int i = 1; i < step.moments.size(); ++i) {
    const auto& m = step.moments[i];
    if (m.phase == QStringLiteral("after") || m.phase == QStringLiteral("counter") || m.phase == QStringLiteral("settle")) ensureAttack();
    if (m.phase == QStringLiteral("attack") && !castAdded && !teamSimple && !attackDisabled) { addCast(step.actor, false); castAdded = true; }
    if (m.phase == QStringLiteral("counter") && !teamSimple && !attackDisabled) addCast(m.actor, true);
    bool meaningful = std::any_of(m.effects.begin(), m.effects.end(), [](const Effect& e) { return meaningfulEffect(e) && e.kind != QStringLiteral("target"); });
    meaningful = meaningful || m.phase == QStringLiteral("attack") || m.phase == QStringLiteral("counter") ||
      m.phase == QStringLiteral("unresolved") || (m.phase == QStringLiteral("setup") && !m.details.isEmpty());
    if (!meaningful) {
      const auto& previous = step.moments[cursor].state;
      meaningful = previous.keys() != m.state.keys();
      for (auto it = m.state.begin(); it != m.state.end() && !meaningful; ++it)
        meaningful = visibleStateChanged(previous.value(it.key()), it.value());
    }
    if (!meaningful) continue;
    PlaybackPhase phase; phase.kind = m.phase; phase.actor = m.actor; phase.title = m.title;
    phase.firstMoment = phase.lastMoment = i; phase.beforeMoment = cursor;
    // The official HP/shield callbacks finish before ordinary power/Buff
    // updates. Preserve that boundary even within the same changeset group.
    if (m.phase == QStringLiteral("before") || m.phase == QStringLiteral("after")) {
      phase.title = (m.phase == QStringLiteral("before") ? (teamSimple ? QStringLiteral("触发前") : QStringLiteral("技能前")) :
        (teamSimple ? QStringLiteral("触发后") : QStringLiteral("技能后")));
      if (m.part == QStringLiteral("wait-buff")) phase.title += QStringLiteral(" · Buff 触发");
      else if (m.part == QStringLiteral("wait-tail")) phase.title += QStringLiteral(" · Buff 后续（时机未记）");
      else if (m.part == QStringLiteral("values")) phase.title += QStringLiteral(" · 生命 / 护盾");
      else if (m.part == QStringLiteral("power-buff")) phase.title += QStringLiteral(" · 气势 / Buff");
      else if (m.part == QStringLiteral("objects")) phase.title += QStringLiteral(" · 状态变化");
      else if (m.part == QStringLiteral("calibration")) phase.title += QStringLiteral(" · 数值确认");
      else if (m.part == QStringLiteral("group-end")) phase.title += QStringLiteral(" · 组末记录（Buff 时机待核实）");
      phase.durationMs = m.part == QStringLiteral("objects") ? 620 : m.part == QStringLiteral("wait-buff") ? 440 : 280;
    }
    for (int e = 0; e < step.effects.size(); ++e)
      if (step.effects[e].kind != QStringLiteral("target") && step.effects[e].moment >= phase.firstMoment && step.effects[e].moment <= phase.lastMoment) phase.effects.append(e);
    if (m.phase == QStringLiteral("attack")) {
      phase.effects = targets + phase.effects; phase.targets = phase.outcomes = true;
      phase.title = QStringLiteral("技能作用"); phase.durationMs = 820; attackAdded = true;
    } else if (m.phase == QStringLiteral("counter")) {
      phase.targets = true; phase.title = QStringLiteral("反击作用"); phase.durationMs = 800;
    } else if (m.phase == QStringLiteral("unresolved")) {
      phase.effects = targets + phase.effects; phase.outcomes = true;
      phase.title = QStringLiteral("顺序待核实 · 最终记录"); phase.durationMs = 1000;
    }
    playbackPhases_.append(phase); cursor = phase.lastMoment; i = phase.lastMoment;
  }
  ensureAttack();
  PlaybackPhase finish; finish.kind = QStringLiteral("finish"); finish.title = QStringLiteral("行动完成");
  finish.actor = step.actor; finish.firstMoment = finish.lastMoment = step.moments.size() - 1;
  finish.beforeMoment = cursor; finish.durationMs = 180; playbackPhases_.append(finish);
  for (int i = 0; i < step.effects.size(); ++i) if (step.effects[i].kind == QStringLiteral("team-target")) {
    playbackPhases_.last().effects.append(i); playbackPhases_.last().outcomes = true;
    playbackPhases_.last().durationMs = 560;
  }
  if (teamSimple) {
    // Raw target flags remain inspectable, but do not manufacture an attack.
    playbackPhases_.last().effects.append(targets); playbackPhases_.last().outcomes = true;
  }
  QList<PlaybackPhase> refined;
  for (const auto& phase : playbackPhases_) {
    const auto slices = phase.beforeMoment != phase.lastMoment && phase.kind != QStringLiteral("finish")
      ? ReplayMeterSequence::split(step, phase.lastMoment) : QList<ReplayMeterSequence::Slice>{};
    if (slices.isEmpty()) { refined.append(phase); continue; }
    for (int n = 0; n < slices.size(); ++n) {
      auto part = phase;
      part.numericBefore = slices[n].beforeState; part.numericState = slices[n].state;
      part.numericSegment = n; part.numericSegmentCount = slices.size();
      part.effects = slices[n].effects;
      const bool last = n + 1 == slices.size();
      if (last) for (const int index : phase.effects)
        if (step.effects[index].kind == QStringLiteral("target") || step.effects[index].kind == QStringLiteral("team-target"))
          part.effects.append(index);
      part.targets = phase.targets && last; part.outcomes = phase.outcomes && last;
      part.title += QStringLiteral(" · 变化 %1/%2").arg(n + 1).arg(slices.size());
      part.durationMs = 600;
      refined.append(part);
    }
  }
  playbackPhases_ = refined;
  for (int phase = 0; phase < playbackPhases_.size(); ++phase)
    for (const int effect : playbackPhases_[phase].effects)
      if (step.effects[effect].kind != QStringLiteral("target") && step.effects[effect].kind != QStringLiteral("team-target"))
        meterEventGroups_[effect] = phase;
  // Keep every recorded boundary. Only the time available to read it changes:
  // short ordinary updates flow together, while entry/form/life transitions
  // remain on screen long enough to identify the affected card.
  for (auto& phase : playbackPhases_) {
    QMap<QString, int> recordsPerUnit;
    for (const int index : phase.effects) if (meaningfulEffect(step.effects[index])) ++recordsPerUnit[step.effects[index].target];
    int density = 0;
    for (const int count : recordsPerUnit) density = std::max(density, count);
    if (phase.kind == QStringLiteral("before") || phase.kind == QStringLiteral("after"))
      phase.durationMs = std::max(phase.durationMs, 260 + 70 * std::min(density, 5));
    for (const int index : phase.effects) {
      const auto& event = step.effects[index];
      if ((event.metric == QStringLiteral("生命") || event.metric == QStringLiteral("护盾") || event.metric == QStringLiteral("气势")) &&
          ReplayActionMeters::visibleChange(event)) phase.durationMs = std::max(phase.durationMs, 600);
    }
    if (phase.beforeMoment < 0 || phase.lastMoment < 0 || phase.beforeMoment == phase.lastMoment) continue;
    if (!phase.numericState) for (const auto& event : step.events) if (event.moment >= phase.firstMoment && event.moment <= phase.lastMoment &&
        (event.kind == QStringLiteral("生命归零") || event.kind == QStringLiteral("生命恢复") ||
         event.kind == QStringLiteral("登场") || event.kind == QStringLiteral("变身")))
      phase.durationMs = std::max(phase.durationMs, 1100);
    const auto& previous = phase.numericBefore ? *phase.numericBefore : step.moments[phase.beforeMoment].state;
    const auto& current = phase.numericState ? *phase.numericState : step.moments[phase.lastMoment].state;
    for (auto it = current.begin(); it != current.end(); ++it) {
      if (!stateTransitionBadges(previous.value(it.key()), it.value()).isEmpty()) {
        phase.durationMs = std::max(phase.durationMs, 1100);
      } else if (previous.contains(it.key()) &&
          (previous[it.key()].attributes != it->attributes || previous[it.key()].normalSkill != it->normalSkill ||
           previous[it.key()].uniqueSkill != it->uniqueSkill)) phase.durationMs = std::max(phase.durationMs, 700);
    }
  }
}
int BattleReplayPage::playbackDuration() const {
  if (playbackPhase_ < 0 || playbackPhase_ >= playbackPhases_.size()) return 600;
  // The official protocol determines phase order, not wall-clock durations.
  // These readable text-animation durations never manufacture hit numbers.
  return std::max(50, qRound(playbackPhases_[playbackPhase_].durationMs * speed_->currentData().toInt() / 1800.0));
}
void BattleReplayPage::updatePlaybackProgress(qreal progress) {
  playbackProgress_ = std::clamp(progress, 0.0, 1.0);
  if (playbackPhase_ < 0) return;
  int total = 0, elapsed = 0;
  for (int i = 0; i < playbackPhases_.size(); ++i) {
    total += playbackPhases_[i].durationMs;
    if (i < playbackPhase_) elapsed += playbackPhases_[i].durationMs;
  }
  const qreal actionProgress = total > 0 ?
    (elapsed + playbackPhases_[playbackPhase_].durationMs * playbackProgress_) / total : 1;
  setProperty("playbackActionProgress", actionProgress);
  phaseProgressBar_->setValue(qRound(actionProgress * 1000));
  phaseProgressBar_->setAccessibleName(QStringLiteral("本次行动进度 %1%").arg(qRound(actionProgress * 100)));
  const qreal visualProgress = animations_->isChecked() ? playbackProgress_ : 1;
  for (auto* cell : cells_) cell->setPlaybackProgress(visualProgress);
  for (auto* spirit : spirits_) spirit->setPlaybackProgress(visualProgress);
  for (auto* sourceCard : teamSources_) static_cast<TeamSourceCard*>(sourceCard)->setProgress(visualProgress);
  links_->setPlaybackProgress(visualProgress);
}
void BattleReplayPage::enterPlaybackPhase(int phase, bool play) {
  if (playbackPhases_.isEmpty()) return;
  phaseAnimation_->stop(); timer_->stop(); boardInspector_->hide();
  playbackPhase_ = std::clamp(phase, 0, int(playbackPhases_.size()) - 1); playbackCompleted_ = false;
  playing_ = play; setProperty("playbackRunning", play);
  const auto& stage = playbackPhases_[playbackPhase_];
  moment_ = stage.lastMoment; effect_ = -1; playbackProgress_ = play ? 0 : 1;
  setProperty("playbackPhaseKind", stage.kind);
  setProperty("playbackBaseDurationMs", stage.durationMs);
  setProperty("playbackFirstMoment", stage.firstMoment); setProperty("playbackLastMoment", stage.lastMoment);
  setProperty("numericSegment", stage.numericSegment); setProperty("numericSegmentCount", stage.numericSegmentCount);
  { QSignalBlocker blocker(moments_); moments_->setCurrentIndex(moment_ + 1); }
  for (auto* cell : cells_) cell->setPlaybackDriven(true);
  for (auto* spirit : spirits_) spirit->setPlaybackDriven(true);
  links_->setPlaybackDriven(true); phaseProgressBar_->show();
  render(); updatePlaybackProgress(playbackProgress_);
  // Changing duration with the previous phase's end time can emit a value
  // before setCurrentTime(0). Reset both atomically, then publish one progress.
  { QSignalBlocker blocker(phaseAnimation_);
    phaseAnimation_->setDuration(playbackDuration()); phaseAnimation_->setCurrentTime(0);
  }
  updatePlaybackProgress(playbackProgress_);
  play_->setText(play ? QStringLiteral("暂停") : QStringLiteral("连续播放"));
  if (play) phaseAnimation_->start();
}
void BattleReplayPage::seekPlaybackPhase(int phase) {
  pause(); enterPlaybackPhase(phase, false);
}
void BattleReplayPage::startContinuousPlayback() {
  if (replay_.steps.isEmpty() || stepsProxy_->rowCount() == 0) return;
  timer_->stop();
  if (playbackCompleted_) seek(stepsProxy_->mapToSource(stepsProxy_->index(0, 0)).row());
  if (!stepsProxy_->mapFromSource(stepsModel_->index(current_, 0)).isValid()) {
    const int next = nextPlaybackStep(); seek(next >= 0 ? next : stepsProxy_->mapToSource(stepsProxy_->index(0, 0)).row());
  }
  if (playbackPhase_ < 0 && replay_.steps[current_].actor.isEmpty() && replay_.steps[current_].keyChanges.isEmpty() && replay_.steps[current_].events.isEmpty()) {
    const int next = nextPlaybackStep(); if (next >= 0) seek(next);
  }
  if (playbackPhase_ >= 0 && phaseAnimation_->state() == QAbstractAnimation::Paused) {
    playing_ = true; setProperty("playbackRunning", true); play_->setText(QStringLiteral("暂停")); phaseAnimation_->resume(); return;
  }
  int phase = playbackPhase_ >= 0 ? playbackPhase_ : 0;
  if (playbackPhase_ < 0 && moment_ >= 0) {
    for (int i = 0; i < playbackPhases_.size(); ++i)
      if (playbackPhases_[i].lastMoment >= moment_) { phase = i; break; }
    if (effect_ >= 0 && replay_.steps[current_].effects[effect_].kind == QStringLiteral("target"))
      for (int i = 0; i < playbackPhases_.size(); ++i) if (playbackPhases_[i].outcomes) { phase = i; break; }
  }
  enterPlaybackPhase(phase, true);
}
void BattleReplayPage::finishPlaybackPhase() {
  if (!playing_ || playbackPhase_ < 0) return;
  if (playbackPhase_ + 1 < playbackPhases_.size()) { enterPlaybackPhase(playbackPhase_ + 1, true); return; }
  const int next = nextPlaybackStep();
  if (next >= 0) { seek(next); enterPlaybackPhase(0, true); return; }
  pause(); playbackCompleted_ = true; play_->setText(QStringLiteral("重新播放"));
}
void BattleReplayPage::hideEvent(QHideEvent* event) { pause(); QWidget::hideEvent(event); }
void BattleReplayPage::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  QTimer::singleShot(0, this, &BattleReplayPage::fitBoardHeight);
}
void BattleReplayPage::fitBoardHeight() {
  if (!arena_ || !arenaScroll_ || !cells_.front()) return;
  // Retain full cards on roomy windows; reserve space for both team meters
  // and the action heading before sizing all three formation rows together.
  arena_->layout()->activate();
  const int overhead = arena_->minimumSizeHint().height() - 3 * cells_.front()->height();
  const int height = std::clamp((arenaScroll_->viewport()->height() - overhead - 12) / 3, 174, 204);
  if (height != cells_.front()->height()) for (auto* cell : cells_) cell->setFixedHeight(height);
  arena_->layout()->activate();
  positionCastingPanel();
}
void BattleReplayPage::positionCastingPanel() {
  if (!castingPanel_ || !castingAnchor_ || !castingAnchor_->isVisible()) {
    if (castingPanel_) castingPanel_->hide();
    return;
  }
  auto* board = castingPanel_->parentWidget();
  auto* teamPanel = castingAnchor_->parentWidget();
  const QPoint anchor = board->mapFromGlobal(castingAnchor_->mapToGlobal(QPoint()));
  const QPoint panel = board->mapFromGlobal(teamPanel->mapToGlobal(QPoint()));
  int width = std::min(590, teamPanel->width() - 16);
  auto titleFont = skillTitle_->font(); titleFont.setWeight(QFont::Bold);
  QTextDocument titleDocument; titleDocument.setDocumentMargin(0);
  const auto measure = [&](int pixels, int measuredWidth) {
    titleFont.setPixelSize(pixels); titleDocument.setDefaultFont(titleFont);
    titleDocument.setHtml(skillTitle_->text()); titleDocument.setTextWidth(std::max(80, measuredWidth - skillSide_->width() - 18));
    return titleDocument.size().height();
  };
  bool sharedRow = measure(14, width) > 30;
  if (sharedRow) width = std::min(board->width() - 16, std::max(width, 740));
  int pixels = 14;
  while (pixels > 10 && measure(pixels, width) > 30) --pixels;
  if (measure(pixels, width) > 30 && sharedRow) width = board->width() - 16;
  const bool fits = measure(pixels, width) <= 32;
  if (skillTitle_->property("fittedFontPx").toInt() != pixels) {
    skillTitle_->setStyleSheet(QStringLiteral("font-size:%1px;font-weight:700;background:transparent;border:0").arg(pixels));
    skillTitle_->setProperty("fittedFontPx", pixels);
  }
  skillTitle_->setProperty("textFits", fits); castingPanel_->setProperty("usesSharedRow", sharedRow);
  const int lower = sharedRow ? 8 : panel.x() + 8;
  const int upper = sharedRow ? board->width() - width - 8 : panel.x() + teamPanel->width() - width - 8;
  const int left = std::clamp(anchor.x() + castingAnchor_->width() / 2 - width / 2, lower, std::max(lower, upper));
  const qreal scale = std::max(qreal(1), castingAnchor_->property("visualScale").toReal());
  const int lift = qCeil(castingAnchor_->height() * (scale - 1) / 2);
  castingPanel_->setGeometry(left, std::max(0, anchor.y() - lift - castingPanel_->height() - 2), width, castingPanel_->height());
  castingPanel_->show(); castingPanel_->raise();
}
void BattleReplayPage::focusSearch() { if (auto* toggle = findChild<QToolButton*>(QStringLiteral("replayDetailsToggle"))) toggle->setChecked(true); if (auto* tabs = qobject_cast<QTabWidget*>(search_->parentWidget()->parentWidget()->parentWidget())) tabs->setCurrentWidget(search_->parentWidget()); search_->setFocus(); }
void BattleReplayPage::openBoardDetail(bool reference) {
  pause();
  referenceControls_->setVisible(reference);
  const int w = std::min(reference ? 650 : 500, arena_->width() - 24), h = std::min(reference ? 620 : 460, arena_->height() - 24);
  boardInspector_->setGeometry(std::max(12, (arena_->width() - w) / 2), 12, w, h);
  boardDetails_->setHtml(unit_->toHtml()); boardInspector_->show(); boardInspector_->raise();
}
void BattleReplayPage::showReferenceLibrary(const QString& category, int page) {
  const int categoryIndex = referenceCategory_->findData(category);
  if (categoryIndex < 0) return;
  { QSignalBlocker block(referenceCategory_); referenceCategory_->setCurrentIndex(categoryIndex); }
  if (referenceRowsCategory_ != category) {
    referenceRows_ = ReplayReference::rows(catalog_, category); referenceRowsCategory_ = category;
    referenceSortedIds_ = ReplayReference::sortedIds(referenceRows_); referenceMatches_ = referenceSortedIds_;
    referenceSearchIndex_.clear(); referenceQuery_.clear();
  }
  const auto query = referenceSearch_->text().trimmed().toCaseFolded();
  if (query != referenceQuery_) {
    referenceMatches_.clear();
    const auto terms = query.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (const auto& id : referenceSortedIds_) {
      if (terms.isEmpty()) { referenceMatches_ << id; continue; }
      if (!referenceSearchIndex_.contains(id)) referenceSearchIndex_.insert(id, ReplayReference::searchable(category, id, referenceRows_[id].toObject()).toCaseFolded());
      const auto& haystack = referenceSearchIndex_[id];
      bool match = true;
      for (const auto& term : terms) if (!haystack.contains(term)) { match = false; break; }
      if (match) referenceMatches_ << id;
    }
    referenceQuery_ = query;
  }
  const auto& matches = referenceMatches_;
  constexpr int pageSize = 25;
  const int pages = std::max(1, (static_cast<int>(matches.size()) + pageSize - 1) / pageSize);
  page = std::clamp(page, 0, pages - 1);
  QString html = QStringLiteral("<h3>%1</h3><p>共 %2 条 · 筛选 %3 条 · 第 %4 / %5 页</p>")
    .arg(ReplayReference::categoryLabel(category).toHtmlEscaped()).arg(referenceRows_.size()).arg(matches.size()).arg(page + 1).arg(pages);
  html += QStringLiteral("<p style='color:#66758a'>官方配置与战报事实分开显示。点击条目查看完整内容及来源；没有说明的字段保留原记录。</p>");
  if (category == QStringLiteral("coverage")) {
    const auto counts = catalog_[QStringLiteral("referenceCoverage")].toObject()[QStringLiteral("summary")].toObject()[QStringLiteral("byStatus")].toObject();
    html += QStringLiteral("<p><b>这里统计的是全资料库核对项，不是本场漏播数量。</b></p><table width='100%' cellpadding='4'>");
    for (const auto& status : {QStringLiteral("missing-reference"), QStringLiteral("reference-unclassified"), QStringLiteral("official-text-empty"), QStringLiteral("identity-unresolved"), QStringLiteral("term-unmatched")})
      html += QStringLiteral("<tr><td valign='top'>%1</td><td>%2</td></tr>").arg(counts[status].toInt()).arg(ReplayReference::coverageMeaning(status).toHtmlEscaped());
    html += QStringLiteral("</table>");
  }
  QString paging;
  if (page > 0) paging += QStringLiteral("<a href='reference-page:%1'>上一页</a>　").arg(page - 1);
  if (page + 1 < pages) paging += QStringLiteral("<a href='reference-page:%1'>下一页</a>").arg(page + 1);
  html += QStringLiteral("<p>%1</p>").arg(paging);
  for (int i = page * pageSize; i < std::min((page + 1) * pageSize, static_cast<int>(matches.size())); ++i) {
    const auto id = matches[i]; const auto row = referenceRows_[id].toObject();
    auto description = ReplayReference::description(row);
    if (description.size() > 130) description = description.left(130) + QStringLiteral("…（点击查看全部）");
    html += QStringLiteral("<p><b>%1</b><br><span style='color:#66758a'>%2</span></p>")
      .arg(ReplayReference::link(category, id, ReplayReference::name(category, id, row)), description.toHtmlEscaped());
  }
  if (matches.isEmpty()) html += QStringLiteral("<p>没有匹配记录。</p>");
  html += QStringLiteral("<p>%1</p>").arg(paging);
  unit_->setHtml(html); unit_->setProperty("referenceCategory", category);
  unit_->setProperty("referenceMatchCount", matches.size()); unit_->setProperty("referencePage", page);
  openBoardDetail(true);
}
void BattleReplayPage::showReferenceRecord(const QString& category, const QString& id, bool raw) {
  const int categoryIndex = referenceCategory_->findData(category);
  if (categoryIndex < 0) return;
  { QSignalBlocker block(referenceCategory_); referenceCategory_->setCurrentIndex(categoryIndex); }
  QString html = ReplayReference::detail(catalog_, category, id, raw);
  const int headingEnd=html.indexOf(QStringLiteral("</h3>"));
  html.insert(headingEnd>=0 ? headingEnd+5 : 0,QStringLiteral("<p>%1</p>").arg(PublicDataDetailLinks::anchor(
      {PublicDataViewKind::BattleReplay,category,id},QStringLiteral("查看官方资料与关联"))));
  if (category == QStringLiteral("skills")) html += officialEntriesHtml(id, true) + tableReferencesHtml(id, true) + ReplayReference::context(catalog_, {}, id);
  else if (category == QStringLiteral("buffs")) html += officialEntriesHtml(id, false) + tableReferencesHtml(id, false);
  else if (category == QStringLiteral("pets")) html += QStringLiteral("<p><a href='mechanics:%1'>查看精灵固有技能与机制</a></p>").arg(esc(id)) + ReplayReference::context(catalog_, id);
  unit_->setHtml(html); unit_->setProperty("referenceCategory", category); unit_->setProperty("referenceId", id);
  openBoardDetail(true);
}
void BattleReplayPage::resetSessionContext() {
  pause(); setAccountIdentity({}, false, QStringLiteral("账号已切换，等待本会话昵称记录"));
  incomingReplays_.clear(); droppedIncoming_ = 0; updateIncomingButton();
} // An explicitly opened offline replay remains inspectable after logout.

void BattleReplayPage::setAccountIdentity(const QString& playerName, bool reliable, const QString& evidence) {
  accountName_ = playerName; accountReliable_ = reliable && !playerName.isEmpty(); accountEvidence_ = evidence;
  setProperty("replayIdentityReliable", accountReliable_);
  // Never flip an already-open board when an asynchronous account update arrives.
  side_->setToolTip(QStringLiteral("选择我方视角；新战报会尝试按当前账号自动匹配。"));
}
void BattleReplayPage::setReplaySourceStatus(const QString& status) {
  sourceStatus_ = status; sourceButton_->setToolTip(QStringLiteral("查看战报接收状态；原始来源信息可按需展开")); sourceButton_->setProperty("sourceStatus", status);
}
void BattleReplayPage::enqueueReplay(const QString& sourceLabel, const QByteArray& original, const QString& sha256) {
  if (original.isEmpty() || original.size() > 32 * 1024 * 1024) return;
  const auto hash = QString::fromLatin1(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex());
  if ((!sha256.isEmpty() && hash != sha256) || replay_.sha256 == hash) return;
  for (const auto& item : incomingReplays_) if (item.sha256 == hash) return;
  if (memoryReplays_.contains(QStringLiteral("memory:") + hash)) return;
  incomingReplays_.append({sourceLabel, hash, original});
  qint64 bytes = 0; for (const auto& item : incomingReplays_) bytes += item.bytes.size();
  while (incomingReplays_.size() > 32 || bytes > 64 * 1024 * 1024) {
    bytes -= incomingReplays_.first().bytes.size(); incomingReplays_.removeFirst(); ++droppedIncoming_;
  }
  updateIncomingButton();
}
void BattleReplayPage::updateIncomingButton() {
  incomingButton_->setText(QStringLiteral("新战报 %1").arg(incomingReplays_.size()));
  incomingButton_->setVisible(!incomingReplays_.isEmpty() || droppedIncoming_ > 0);
  incomingButton_->setProperty("pendingCount", incomingReplays_.size());
  incomingButton_->setToolTip(QStringLiteral("新战报不会切换当前文件或中断播放。待阅区保留最近 32 份、最多 64 MiB 的原文快照。") +
    (droppedIncoming_ ? QStringLiteral("\n已有 %1 份较早快照超出待阅容量；文件来源仍可从原目录打开。").arg(droppedIncoming_) : QString()));
}
void BattleReplayPage::showIncomingReplays() {
  QString html = QStringLiteral("<h3>收到的新战报</h3><p>点击打开原文快照。当前文件不会被后台新战报替换。</p>");
  for (auto it = incomingReplays_.crbegin(); it != incomingReplays_.crend(); ++it)
    html += QStringLiteral("<p><a href='incoming:%1'>打开 · %2</a></p>")
      .arg(esc(it->sha256), esc(publicReplayLabel(it->label)));
  if (incomingReplays_.isEmpty()) html += QStringLiteral("<p>当前没有待阅战报。</p>");
  if (droppedIncoming_) html += QStringLiteral("<p>较早的 %1 份已超出待阅容量；有文件来源的记录可从原目录打开。</p>").arg(droppedIncoming_);
  html += QStringLiteral("<p><a href='source-raw:all'>查看原始接收状态</a></p>");
  unit_->setHtml(html); openBoardDetail();
}
void BattleReplayPage::openIncomingReplay(const QString& sha256) {
  for (int i = 0; i < incomingReplays_.size(); ++i) {
    const auto item = incomingReplays_[i]; if (item.sha256 != sha256) continue;
    const auto key = QStringLiteral("memory:") + item.sha256;
    if (!loadReplayBytes(item.bytes, item.label, key)) return;
    memoryReplays_.insert(key, item.bytes); incomingReplays_.removeAt(i); updateIncomingButton(); return;
  }
}
void BattleReplayPage::showCurrentDiagnostics() {
  if (replay_.steps.isEmpty()) return;
  const auto& step = replay_.steps[current_];
  QString html = QStringLiteral("<h3>本步核对 · %1</h3>").arg(esc(step.title));
  if (!step.unlinkedReferences.isEmpty()) {
    html += QStringLiteral("<h4>未关联的对象引用</h4><p>战报出现了以下编号，但初始阵容没有对应实例。原记录完整保留，不把它猜成某只精灵或放进九宫格。</p>");
    for (const auto& item : step.unlinkedReferences) {
      QStringList flags;
      for (auto it = item.flags.begin(); it != item.flags.end(); ++it)
        flags.append(esc(it.key()) + QStringLiteral("=") + (it.value() ? QStringLiteral("true") : QStringLiteral("false")));
      html += QStringLiteral("<p><b>编号 %1 · 队伍 %2 · 玩家 %3</b><br>%4<br>路径：%5<br>判定：%6</p><pre>%7</pre>")
        .arg(esc(item.id)).arg(item.team).arg(esc(item.player), esc(item.reason), esc(item.path), flags.join(QStringLiteral("，")), esc(ReplayInspection::json(item.raw)));
    }
  }
  int unknowns = 0;
  for (const auto& fighter : displayedState()) {
    if (ReplayInspection::unknownValues(fighter, replay_.root).isEmpty()) continue;
    ++unknowns;
    html += QStringLiteral("<h4>%1</h4>").arg(link(fighter, catalog_)) + ReplayInspection::unknownHtml(fighter, replay_.root, Step{});
  }
  if (!step.playbackNotes.isEmpty()) {
    html += QStringLiteral("<h4>本次播放依据与边界</h4>");
    for (const auto& note : step.playbackNotes) html += QStringLiteral("<p>%1</p>").arg(esc(note));
  }
  if (unknowns == 0 && step.unlinkedReferences.isEmpty() && step.playbackNotes.isEmpty())
    html += QStringLiteral("<p>本步没有未关联对象或未确认的当前数值。</p>");
  if (!replay_.issues.isEmpty()) {
    html += QStringLiteral("<h4>全场原始数据提示</h4><p>以下包含其他行动的提示，不表示都发生在当前阶段。</p>");
    for (const auto& issue : replay_.issues) html += QStringLiteral("<p>%1</p>").arg(esc(issue));
  }
  html += QStringLiteral("<p>未记录的施加来源保持“来源未记”；缺少历史官方配置不会据当前效果反推。</p>");
  unit_->setHtml(html); openBoardDetail();
}
void BattleReplayPage::showTeamDetails(int side, bool raw) {
  if (replay_.steps.isEmpty() || side < 0 || side >= 2) return;
  const int teamId = queues_[side]->property("teamId").toInt();
  const auto ownerKey = fighterKey(teamId, players_[side]->currentData().toString(), QStringLiteral("@team"));
  const auto state = displayedState();
  QString html = QStringLiteral("<h3>%1 · 阵容资料</h3>").arg(side == 0 ? QStringLiteral("我方") : QStringLiteral("敌方"));
  if (!raw) {
    QStringList names;
    for (const auto& fighter : state)
      if (fighter.team == teamId && fighter.player == players_[side]->currentData().toString() && !fighter.representer)
        names << QStringLiteral("%1 · %2").arg(link(fighter, catalog_), esc(fighter.presence));
    html += QStringLiteral("<p>%1</p>").arg(names.join(QStringLiteral("<br>")));
    html += queues_[side]->property("queueDetailHtml").toString();
    html += QStringLiteral("<h4>通灵之魂</h4><p>%1</p>").arg(esc(spirits_[side]->accessibleName()));
    if (teamSources_[side]->isVisible()) html += QStringLiteral("<h4>当前触发来源</h4><p>%1</p>").arg(esc(teamSources_[side]->accessibleName()));
    if (!dragonBars_[side]->property("dragonPublicHtml").toString().isEmpty()) html += dragonBars_[side]->property("dragonPublicHtml").toString();
    html += QStringLiteral("<h4>队伍效果</h4>") + teamBuffs_[side]->property("completeHtml").toString();
    html += QStringLiteral("<p><a href='team-raw:%1'>查看原始详情</a></p>").arg(side);
    unit_->setHtml(html); openBoardDetail(); return;
  }
  html.prepend(QStringLiteral("<p><a href='team:%1'>返回阵容概览</a> · 原始详情</p>").arg(side));
  if (state.contains(ownerKey)) {
    const auto& owner = state[ownerKey];
    html += QStringLiteral("<p>%1</p>").arg(link(owner, catalog_)) + ReplayInspection::recordedMetadata(owner, catalog_);
  } else html += QStringLiteral("<p>当前选中的阵容没有可关联的队伍记录。</p>");
  html += queues_[side]->property("queueDetailHtml").toString();
  if (teamSources_[side]->isVisible()) html += QStringLiteral("<h4>当前触发来源</h4><p>%1</p>")
    .arg(esc(teamSources_[side]->property("rawEvidence").toString()).replace(QLatin1Char('\n'), QStringLiteral("<br>")));
  html += QStringLiteral("<p>%1</p><pre>%2</pre><p>通灵 Buff：%3</p><pre>%4</pre>")
    .arg(esc(queues_[side]->property("queuePath").toString()), esc(spirits_[side]->property("rawEvidence").toString()),
      esc(spirits_[side]->property("progressBuffId").toString()), esc(spirits_[side]->property("progressRaw").toString()));
  if (!dragonBars_[side]->property("dragonDetailHtml").toString().isEmpty()) html += dragonBars_[side]->property("dragonDetailHtml").toString();
  html += QStringLiteral("<h4>队伍效果</h4>") + teamBuffs_[side]->property("completeHtml").toString();
  unit_->setHtml(html); openBoardDetail();
}
int BattleReplayPage::nextVisibleStep(int direction) const {
  int result = -1;
  for (int row = 0; row < stepsProxy_->rowCount(); ++row) {
    const auto n = stepsProxy_->mapToSource(stepsProxy_->index(row, 0)).row();
    if (direction > 0 && n > current_) return n;
    if (direction < 0 && n < current_) result = n;
  }
  return result;
}
int BattleReplayPage::nextPlaybackStep() const {
  // Preparation snapshots remain manually accessible. Continuous playback
  // spends time on complete actions and actual formation/result changes.
  for (int row = 0; row < stepsProxy_->rowCount(); ++row) {
    const int index = stepsProxy_->mapToSource(stepsProxy_->index(row, 0)).row();
    if (index <= current_) continue;
    const auto& step = replay_.steps[index];
    if (!step.actor.isEmpty() || !step.keyChanges.isEmpty() || !step.events.isEmpty() || index == replay_.steps.size() - 1) return index;
  }
  return -1;
}
int BattleReplayPage::nextEffectIndex(int direction) const {
  if (replay_.steps.isEmpty()) return -1;
  const auto& currentEffects = replay_.steps[current_].effects;
  const bool numeric = playbackPhase_ >= 0 && playbackPhase_ < playbackPhases_.size() &&
    playbackPhases_[playbackPhase_].numericState.has_value();
  // Preserve ordinary command order. Within a refined command, the effect
  // array is grouped by recipient, whereas reading order can be A0,B0,A1,B1.
  // Use that order both inside the command and when entering it backwards.
  QMap<int, QList<int>> reading;
  for (const auto& phase : playbackPhases_) if (phase.numericState) {
    auto indices = phase.effects; std::sort(indices.begin(), indices.end());
    for (const int index : indices) if (currentEffects[index].moment == phase.lastMoment)
      reading[phase.lastMoment].append(index);
  }
  QList<int> ordered; QSet<int> expanded;
  for (int index = 0; index < currentEffects.size(); ++index) {
    const int moment = currentEffects[index].moment;
    if (!reading.contains(moment)) ordered.append(index);
    else if (!expanded.contains(moment)) { ordered.append(reading.value(moment)); expanded.insert(moment); }
  }
  const bool byEffect = effect_ >= 0 && (!numeric || currentEffects[effect_].moment == moment_);
  const int from = byEffect ? ordered.indexOf(effect_) + direction : direction > 0 ? 0 : int(ordered.size()) - 1;
  for (int at = from; at >= 0 && at < ordered.size(); at += direction) {
    const int index = ordered[at]; const auto& effect = currentEffects[index];
    if (!meaningfulEffect(effect)) continue;
    if (!byEffect && moment_ >= 0) {
      if (numeric && effect.moment == moment_) {
        const int phase = meterEventGroups_.value(index, -1);
        if (direction > 0 ? phase <= playbackPhase_ : phase >= playbackPhase_) continue;
      } else if (direction > 0 ? effect.moment <= moment_ : effect.moment >= moment_) continue;
    }
    return index;
  }
  return -1;
}
bool BattleReplayPage::advanceEffect(int direction) {
  if (replay_.steps.isEmpty()) return false;
  const int index = nextEffectIndex(direction);
  if (index >= 0) { selectEffect(index); return true; }
  const int next = nextVisibleStep(direction);
  if (next < 0) return false;
  seek(next);
  const auto& effects = replay_.steps[current_].effects;
  for (int i = direction > 0 ? 0 : int(effects.size()) - 1; i >= 0 && i < effects.size(); i += direction)
    if (meaningfulEffect(effects[i])) { selectEffect(i); break; }
  return true;
}
void BattleReplayPage::updateTimelineFilter() {
  pause();
  { QSignalBlocker blocker(timeline_->selectionModel());
    stepsProxy_->unit = unitFilter_->currentData().toString(); stepsProxy_->important = important_->isChecked(); stepsProxy_->refresh();
    timeline_->clearSelection();
    const auto index = stepsProxy_->mapFromSource(stepsModel_->index(current_, 0));
    if (index.isValid()) timeline_->selectRow(index.row());
  }
  filterCount_->setText(QStringLiteral("%1 / %2 条").arg(stepsProxy_->rowCount()).arg(replay_.steps.size()));
  render();
}
void BattleReplayPage::jumpEvent(int direction) {
  pause(); if (replay_.steps.isEmpty()) return;
  const auto kind = eventType_->currentIndex() == 0 ? QString() : eventType_->currentText();
  const auto unit = unitFilter_->currentData().toString();
  const auto& currentStep = replay_.steps[current_];
  const int position = moment_ < 0 ? int(currentStep.moments.size()) : moment_;
  const int numericPosition = playbackPhase_ >= 0 && playbackPhase_ < playbackPhases_.size() &&
    playbackPhases_[playbackPhase_].numericState ? playbackPhases_[playbackPhase_].numericSegment : -1;
  for (int i = current_; i >= 0 && i < replay_.steps.size(); i += direction) {
    const auto& step = replay_.steps[i];
    const auto events = ReplayTimeline::events(step);
    for (int j = direction > 0 ? 0 : int(events.size()) - 1; j >= 0 && j < events.size(); j += direction) {
      const auto& e = events[j];
      if ((!kind.isEmpty() && e.kind != kind) || (!unit.isEmpty() && e.key != unit)) continue;
      if (i == current_ && e.moment < 0) continue;
      if (i == current_) {
        if (e.moment == position) {
          if (e.segment < 0 || (numericPosition < 0 ? direction > 0 :
              direction > 0 ? e.segment <= numericPosition : e.segment >= numericPosition)) continue;
        } else if (direction > 0 ? e.moment < position : e.moment > position) continue;
      }
      seek(i);
      if (e.segment >= 0) {
        for (int phase = 0; phase < playbackPhases_.size(); ++phase) {
          const auto& candidate = playbackPhases_[phase];
          if (candidate.numericState && candidate.lastMoment == e.moment && candidate.numericSegment == e.segment) {
            seekPlaybackPhase(phase); return;
          }
        }
      }
      seekMoment(e.moment); return;
    }
  }
  filterCount_->setText(direction > 0 ? QStringLiteral("已无后续匹配事件") : QStringLiteral("已无更早匹配事件"));
}
void BattleReplayPage::loadDirectory(const QString& path) {
  if (!QFileInfo(path).isDir()) { status_->setText(QStringLiteral("战报文件夹不存在。")); status_->show(); return; }
  reportDirectory_ = QFileInfo(path).absoluteFilePath();
  const auto entries = QDir(path).entryInfoList({QStringLiteral("*.txt"), QStringLiteral("*.json")}, QDir::Files, QDir::Name);
  { QSignalBlocker blocker(files_); files_->clear(); for (const auto& f : entries) files_->addItem(f.fileName(), f.absoluteFilePath()); }
  if (!entries.isEmpty()) loadFile(entries.first().absoluteFilePath());
  else { status_->setText(QStringLiteral("该文件夹中没有 txt / json 日志。")); status_->show(); }
}
bool BattleReplayPage::loadFile(const QString& path) {
  if (memoryReplays_.contains(path)) {
    const int index = files_->findData(path);
    return loadReplayBytes(memoryReplays_.value(path), index >= 0 ? files_->itemText(index) : path, path);
  }
  pause(); QFile file(path);
  status_->show();
  if (!file.open(QIODevice::ReadOnly)) { status_->setText(QStringLiteral("读取失败：%1").arg(file.errorString())); return false; }
  reportDirectory_ = QFileInfo(path).absolutePath();
  if (file.size() > 32 * 1024 * 1024) { status_->setText(QStringLiteral("文件超过本版 32 MiB 的读取上限。")); return false; }
  return loadReplayBytes(file.readAll(), QFileInfo(path).absoluteFilePath());
}
bool BattleReplayPage::loadReplayBytes(const QByteArray& bytes, const QString& label, const QString& itemKey) {
  pause(); status_->show();
  const auto nextCatalog = latestCatalog_;
  if (!nextCatalog || nextCatalog->root.isEmpty()) {
    status_->setText(QStringLiteral("回放资料未就绪，暂不能打开战报。请等待资料读取完成，或点击“重新读取资料”后重试。"));
    return false;
  }
  auto result = BattleReplay::parse(bytes, nextCatalog->root);
  if (!result.error.isEmpty()) { status_->setText(QFileInfo(label).fileName() + QStringLiteral("：") + result.error); return false; }
  installReplay(std::move(result), nextCatalog, label, itemKey);
  return true;
}
void BattleReplayPage::installReplay(BattleReplay::Replay result, BattleReplayCatalogHandle nextCatalog,
                                    const QString& label, const QString& itemKey, bool preserveSettings) {
  const auto oldSide = side_->currentData();
  const auto oldFilter = unitFilter_->currentData();
  const bool oldImportant = important_->isChecked();
  stepsModel_->reload(nullptr); fieldsModel_->reload(nullptr);
  replayCatalog_ = nextCatalog; catalog_ = replayCatalog_->root;
  clearReferenceCatalogCache(); unit_->clear(); boardDetails_->clear();
  replay_ = std::move(result); file_ = label; current_ = 0; selectedUnit_.clear();
  if (!preserveSettings) for (auto* cell : cells_) cell->resetBuffExpansion();
  empowered_.clear(); empowermentNotes_.clear();
  empowermentEvidence_ = identifyEmpowerments(replay_.steps.first().state, catalog_);
  selfTransformEvidence_ = identifySelfTransforms(replay_.steps.first().state, catalog_);
  for (auto it = empowermentEvidence_.begin(); it != empowermentEvidence_.end(); ++it) {
    empowered_.insert(it.key()); empowermentNotes_.insert(it.key(), it.value().description);
  }
  stepsModel_->reload(&replay_); fieldsModel_->reload(&replay_);
  { QSignalBlocker a(unitFilter_), b(important_); unitFilter_->clear(); unitFilter_->addItem(QStringLiteral("全部单位"), QString()); important_->setChecked(false);
    for (const auto& f : replay_.steps.last().state) unitFilter_->addItem(QStringLiteral("%1 · %2").arg(f.player, replayFighterName(f, catalog_)), f.key);
  }
  stepsProxy_->unit.clear(); stepsProxy_->important = false; stepsProxy_->refresh();
  if (preserveSettings) {
    const QSignalBlocker a(unitFilter_), b(important_);
    unitFilter_->setCurrentIndex(std::max(0, unitFilter_->findData(oldFilter)));
    important_->setChecked(oldImportant);
    stepsProxy_->unit = unitFilter_->currentData().toString(); stepsProxy_->important = oldImportant; stepsProxy_->refresh();
  }
  filterCount_->setText(QStringLiteral("%1 条").arg(replay_.steps.size()));
  { QSignalBlocker blocker(files_); const auto key = itemKey.isEmpty() ? file_ : itemKey;
    int index = files_->findData(key); if (index < 0) { files_->addItem(publicReplayLabel(QFileInfo(label).fileName()), key); index = files_->count() - 1; } files_->setCurrentIndex(index); }
  { QSignalBlocker blocker(side_); side_->clear(); for (auto it = replay_.teams.begin(); it != replay_.teams.end(); ++it)
      side_->addItem(QStringLiteral("我方：%1").arg(it.value().isEmpty() ? (side_->count() == 0 ? QStringLiteral("左侧阵容") : QStringLiteral("右侧阵容")) : it.value()), it.key());
    QSet<int> matches;
    if (accountReliable_) for (const auto& fighter : replay_.steps.first().state)
      if (fighter.representer && fighter.player == accountName_) matches.insert(fighter.team);
    const bool selected = matches.size() == 1;
    if (selected) side_->setCurrentIndex(side_->findData(*matches.begin()));
    if (preserveSettings && side_->findData(oldSide) >= 0) side_->setCurrentIndex(side_->findData(oldSide));
    side_->setProperty("identityMatched", selected);
    side_->setToolTip(selected ? QStringLiteral("已按当前账号选择我方视角；可手动切换。") :
      QStringLiteral("当前使用手动选择的视角。"));
  }
  rounds_->clear(); int lastRound = -1;
  for (qsizetype i = 0; i < replay_.steps.size(); ++i) {
    const auto& step = replay_.steps[i];
    if (!ReplayTimeline::isAction(step)) continue;
    const int round = step.round < 0 ? -1 : step.round;
    if (round >= 0 && round != lastRound) rounds_->addItem(QStringLiteral("第 %1 回合").arg(round), i);
    // An unknown action separates repeated known round numbers; setup and
    // result records do not begin or split an action's recorded round.
    lastRound = round;
  }
  { QSignalBlocker a(slider_), b(jump_); slider_->setRange(0, static_cast<int>(replay_.steps.size()) - 1); jump_->setRange(0, slider_->maximum()); }
  slider_->setReplay(replay_, catalog_);
  rawAll_->setPlainText(QString::fromUtf8(replay_.original));
  status_->setText(QStringLiteral("%1  ·  %2 次行动").arg(publicReplayLabel(QFileInfo(label).fileName())).arg(replay_.actions));
  if (replay_.containerKind == QStringLiteral("team"))
    status_->setText(status_->text() + QStringLiteral(" · 组队战报按官方选择第 %1/%2 项").arg(replay_.selectedBattleIndex + 1).arg(replay_.containerBattleCount));
  files_->setToolTip(status_->text()); status_->hide();
  updateCatalogStatus(); updateNotes(); seek(0);
}
bool BattleReplayPage::useUpdatedCatalog() {
  if (catalogBusy_ || !latestCatalog_ || replay_.steps.isEmpty() || sameCatalogContent(replayCatalog_, latestCatalog_)) return false;
  // Prepare everything before altering the active scene. Failed interpretation
  // leaves its catalog, original bytes, navigation and playback intact.
  const auto nextCatalog = latestCatalog_;
  auto candidate = BattleReplay::parse(replay_.original, nextCatalog->root);
  if (!candidate.error.isEmpty() || candidate.steps.isEmpty()) {
    status_->setText(QStringLiteral("新资料未能解释本场战报，已保留原回放。")); status_->show(); return false;
  }
  const auto& previous = replay_.steps[current_];
  int restored = -1;
  // Paths are the original protocol action address, not a derived frame.
  // Actor/raw identity prevents a same-position but different action match.
  for (int index = 0; index < candidate.steps.size(); ++index) {
    const auto& step = candidate.steps[index];
    if (step.path == previous.path && step.actor == previous.actor && step.round == previous.round && step.raw == previous.raw) {
      if (restored >= 0) { restored = -1; break; }
      restored = index;
    }
  }
  // An unambiguous original action is required; internal moment/phase indices
  // are deliberately discarded even if the new parser happens to match them.
  if (restored < 0) {
    status_->setText(QStringLiteral("无法确定新版资料中的当前行动，已保留原回放。")); status_->show(); return false;
  }
  const auto itemKey = files_->currentData().toString();
  const auto selected = selectedUnit_;
  const std::array<QVariant,2> players{players_[0]->currentData(),players_[1]->currentData()};
  const bool followPlayers = followPlayers_;
  pause(); installReplay(std::move(candidate), nextCatalog, file_, itemKey, true);
  selectedUnit_ = selected;
  seekActionStart(restored);
  followPlayers_ = followPlayers;
  if (!followPlayers) for (int side = 0; side < 2; ++side) {
    const QSignalBlocker blocker(players_[side]);
    const int index = players_[side]->findData(players[side]);
    if (index >= 0) players_[side]->setCurrentIndex(index);
  }
  updateTimelineFilter(); render();
  return true;
}
void BattleReplayPage::seekActionStart(int step) {
  pause(); seek(step);
  if (!replay_.steps.isEmpty() && ReplayTimeline::isAction(replay_.steps[current_])) seekPlaybackPhase(0);
}
void BattleReplayPage::seek(int step) {
  if (replay_.steps.isEmpty()) return;
  clearPlaybackPhase();
  current_ = std::clamp(step, 0, static_cast<int>(replay_.steps.size()) - 1);
  boardInspector_->hide();
  followPlayers_ = true;
  moment_ = -1;
  effect_ = -1;
  rebuildPlaybackPhases();
  { QSignalBlocker blocker(moments_); moments_->clear(); moments_->addItem(QStringLiteral("行动总览（结束状态）"), -1);
    const auto& frames = replay_.steps[current_].moments;
    for (int i = 0; i < frames.size(); ++i) moments_->addItem(QStringLiteral("%1. %2").arg(i + 1).arg(frames[i].title), i);
  }
  { QSignalBlocker a(slider_), b(jump_), c(timeline_->selectionModel()); slider_->setValue(current_); jump_->setValue(current_);
    timeline_->clearSelection(); const auto index = stepsProxy_->mapFromSource(stepsModel_->index(current_, 0));
    if (index.isValid()) { timeline_->selectRow(index.row()); timeline_->scrollTo(index); }
  }
  render();
}
void BattleReplayPage::seekMoment(int moment) {
  if (replay_.steps.isEmpty()) return;
  clearPlaybackPhase();
  boardInspector_->hide();
  moment_ = std::clamp(moment, -1, static_cast<int>(replay_.steps[current_].moments.size()) - 1);
  effect_ = -1;
  const auto& effects = replay_.steps[current_].effects;
  for (int i = 0; i < effects.size(); ++i) {
    if (effects[i].moment == moment_ && effects[i].changed) { effect_ = i; break; }
  }
  { QSignalBlocker blocker(moments_); moments_->setCurrentIndex(moment_ + 1); }
  render();
}
void BattleReplayPage::selectEffect(int index) {
  if (replay_.steps.isEmpty()) return;
  const auto& effects = replay_.steps[current_].effects;
  if (index < 0 || index >= effects.size()) return;
  boardInspector_->hide();
  clearPlaybackPhase(); effect_ = index; moment_ = effects[index].moment;
  for (int n = 0; n < playbackPhases_.size(); ++n) {
    if (!playbackPhases_[n].numericState || !playbackPhases_[n].effects.contains(index)) continue;
    enterPlaybackPhase(n, false); effect_ = index; followPlayers_ = true; render(); return;
  }
  { QSignalBlocker blocker(moments_); moments_->setCurrentIndex(moment_ + 1); }
  followPlayers_ = true; render();
}
QString BattleReplayPage::unitCaption(const QString& key) const {
  if (key.isEmpty()) return QStringLiteral("施加者未记录");
  const auto& state = displayedState();
  if (!state.contains(key)) return QStringLiteral("精灵身份待确认");
  const auto& f = state[key];
  const QString side = f.team == side_->currentData().toInt() ? QStringLiteral("我方") : QStringLiteral("敌方");
  if (f.representer) return side + QStringLiteral("队伍效果");
  return side + QStringLiteral(" · ") + replayFighterName(f, catalog_);
}
QString BattleReplayPage::effectSummary(const Effect& e) const {
  if (e.kind == QStringLiteral("target") || e.kind == QStringLiteral("team-target")) {
    const auto raw = e.raw.toObject(); QStringList flags;
    if (raw.contains(QStringLiteral("ishit"))) flags.append(raw[QStringLiteral("ishit")].toBool() ? QStringLiteral("命中") : QStringLiteral("未命中"));
    if (raw.contains(QStringLiteral("isse")) && !raw[QStringLiteral("isse")].toBool()) flags.append(QStringLiteral("技能未生效"));
    for (const auto& flag : QList<QPair<QString, QString>>{{"isbl","格挡"},{"iscr","暴击"},{"issb","反击标记"},{"isIm","免疫"}})
      if (raw[flag.first].toBool()) flags.append(flag.second);
    return (e.kind == QStringLiteral("team-target") ? QStringLiteral("队伍目标记录：") : QStringLiteral("目标记录：")) +
      (flags.isEmpty() ? QStringLiteral("没有命中/生效标记") : flags.join(QStringLiteral(" · ")));
  }
  if (e.kind == QStringLiteral("buff")) {
    const bool adding = ReplayBuffHistory::isAddition(e);
    const QString verb = adding ? QStringLiteral("获得 ") : e.before == 0 ? QStringLiteral("移除记录（无匹配项） ") : QStringLiteral("移除一条 ");
    return verb + replayPublicReferenceName(catalog_, QStringLiteral("buffs"), e.causeId) + QStringLiteral("  [记录数 %1 → %2]").arg(number(e.before), number(e.after));
  }
  if (e.kind == QStringLiteral("progress")) {
    return QStringLiteral("通灵 %1 → %2 / %3").arg(number(e.before), number(e.after), valueText(e.raw.toObject()[QStringLiteral("mp")]));
  }
  if (e.kind == QStringLiteral("appearance")) return QStringLiteral("登场");
  if (e.kind == QStringLiteral("transform") || e.kind == QStringLiteral("attributes")) {
    const auto raw = e.raw.toObject(); QStringList names;
    for (const auto& id : raw[QStringLiteral("ca")].toString().split(QLatin1Char(','), Qt::SkipEmptyParts)) names.append(replayPublicReferenceName(catalog_, QStringLiteral("attributes"), id));
    return (e.kind == QStringLiteral("transform") ? QStringLiteral("变身为 ") + replayPublicReferenceName(catalog_, QStringLiteral("pets"), valueText(raw[QStringLiteral("cr")])) + QStringLiteral(" · ") : QString()) +
      QStringLiteral("属性 → ") + (names.isEmpty() ? QStringLiteral("未记录") : names.join(QStringLiteral("、")));
  }
  if (!e.metric.isEmpty() && (e.before || e.after)) {
    QString result = QStringLiteral("%1 %2 → %3").arg(e.metric, number(e.recordedBefore ? e.recordedBefore : e.before), number(e.recordedAfter ? e.recordedAfter : e.after));
    const auto delta = e.recordedDelta ? e.recordedDelta : e.delta;
    if (delta) result += QStringLiteral("（记录 %1%2）").arg(*delta > 0 ? QStringLiteral("+") : QString(), QString::number(*delta));
    if (e.recordedBefore && e.recordedBefore != e.before)
      result += QStringLiteral("；此前已知余额 %1，本条将余额确认至 %2").arg(number(e.before), number(e.after));
    return result;
  }
  return e.text;
}
void BattleReplayPage::renderEffects() {
  QSignalBlocker blocker(effectList_); effectList_->clear();
  if (replay_.steps.isEmpty()) return;
  const auto& effects = replay_.steps[current_].effects;
  for (int i = 0; i < effects.size(); ++i) {
    const auto& e = effects[i];
    if (!allEffects_->isChecked() && !meaningfulEffect(e)) continue;
    const QString origin = e.source.isEmpty() ? (e.cause == QStringLiteral("calibration") ? QStringLiteral("日志最终校准") : e.cause == QStringLiteral("record") ? QStringLiteral("日志状态记录") : QStringLiteral("施加者未记录")) : unitCaption(e.source);
    auto* item = new QListWidgetItem(QStringLiteral("%1 → %2\n%3%4").arg(origin, unitCaption(e.target), effectSummary(e),
      e.causeName.isEmpty() || e.kind == QStringLiteral("buff") ? QString() : QStringLiteral("\n") + e.causeName), effectList_);
    item->setData(Qt::UserRole, i); item->setToolTip(e.text + QLatin1Char('\n') + e.path);
    if (i == effect_) effectList_->setCurrentItem(item);
  }
  if (!effectList_->count()) {
    auto* item = new QListWidgetItem(QStringLiteral("本条没有已确认的变化。可勾选“包括无变化记录”，或查看过程与原文。"), effectList_);
    item->setFlags(Qt::NoItemFlags);
  }
}
void BattleReplayPage::renderOverview() {
  if (replay_.steps.isEmpty()) return;
  const auto& step = replay_.steps[current_];
  QStringList ordered;
  QMap<QString, QStringList> records;
  QSet<QString> directRecipients;
  // Summarize the whole action by recipient. Keep a link to each exact event,
  // so the summary never substitutes net change for damage or event order.
  for (int i = 0; i < step.effects.size(); ++i) {
    const auto& e = step.effects[i];
    if (e.kind == QStringLiteral("target")) {
      if (!inactiveTargetRecord(e) && !ordered.contains(e.target)) ordered.append(e.target);
      continue;
    }
    if (!meaningfulEffect(e)) continue;
    if (!ordered.contains(e.target)) ordered.append(e.target);
    const bool fromActor = !step.actor.isEmpty() && e.source == step.actor;
    if (fromActor) directRecipients.insert(e.target);
    const auto origin = e.source.isEmpty() ? (e.cause == QStringLiteral("record") || e.cause == QStringLiteral("calibration") ? QStringLiteral("日志状态记录") : QStringLiteral("施加者未记录")) : fromActor ? QStringLiteral("行动者") : unitCaption(e.source);
    const auto item = QStringLiteral("<a style='color:%1;text-decoration:none' href='effect:%2'>%3. %4：%5</a>")
      .arg(fromActor ? QStringLiteral("#245fa0") : QStringLiteral("#59677b")).arg(i).arg(i + 1)
      .arg(esc(origin), esc(effectSummary(e)));
    records[e.target].append(item);
  }
  QString html = QStringLiteral("<p style='color:#617086'>本次行动按单位归纳；点击记录定位发生的小步。序号保留原有事件顺序。</p>");
  // Put direct recipients ahead of changes whose applier is unrecorded.
  std::stable_sort(ordered.begin(), ordered.end(), [&](const auto& a, const auto& b) {
    const int pa = a == step.actor ? 1 : directRecipients.contains(a) ? 2 : 0;
    const int pb = b == step.actor ? 1 : directRecipients.contains(b) ? 2 : 0;
    return pa > pb;
  });
  for (const auto& key : ordered) {
    html += QStringLiteral("<h4 style='color:#234780;margin-bottom:4px'>%1</h4>").arg(esc(unitCaption(key)));
    QStringList flags;
    for (const auto& e : step.effects) if (e.kind == QStringLiteral("target") && e.target == key)
      flags.append(effectSummary(e));
    flags.removeDuplicates();
    if (!flags.isEmpty()) html += QStringLiteral("<p style='color:#985226'>%1</p>").arg(esc(flags.join(QStringLiteral("；"))));
    if (records.contains(key)) html += QStringLiteral("<p>%1</p>").arg(records[key].join(QStringLiteral("<br>")));
  }
  if (ordered.isEmpty()) html += QStringLiteral("<p>本条没有变化记录；完整目标标记保留在“作用”的无变化记录中。</p>");
  overview_->setHtml(html);
}
const QMap<QString, Fighter>& BattleReplayPage::displayedState() const {
  const auto& step = replay_.steps[current_];
  if (playbackPhase_ >= 0 && playbackPhase_ < playbackPhases_.size() && playbackPhases_[playbackPhase_].numericState)
    return *playbackPhases_[playbackPhase_].numericState;
  return moment_ < 0 ? step.state : step.moments[moment_].state;
}
void BattleReplayPage::render() {
  const bool loaded = !replay_.steps.isEmpty();
  configurationButton_->setEnabled(true); diagnosticsButton_->setEnabled(loaded);
  for (auto* widget : {static_cast<QWidget*>(play_), static_cast<QWidget*>(restart_), static_cast<QWidget*>(slider_), static_cast<QWidget*>(jump_), static_cast<QWidget*>(rounds_), static_cast<QWidget*>(timelineView_)}) widget->setEnabled(loaded);
  previous_->setEnabled(loaded && nextVisibleStep(-1) >= 0); next_->setEnabled(loaded && nextVisibleStep(1) >= 0);
  moments_->setEnabled(loaded && !replay_.steps[current_].moments.isEmpty());
  previousMoment_->setEnabled(loaded && !replay_.steps[current_].moments.isEmpty() && moment_ != 0);
  nextMoment_->setEnabled(loaded && moment_ + 1 < replay_.steps[current_].moments.size());
  if (!loaded) {
    smallTurnLabel_->setText(QStringLiteral("未载入战报"));
    smallTurnLabel_->setProperty("smallTurn", 0);
    smallTurnLabel_->setProperty("globalTurn", 0);
    smallTurnLabel_->setProperty("round", -1);
    meterOverlay_->setChanges({});
    for (auto* sourceCard : teamSources_) sourceCard->hide();
    castingAnchor_ = nullptr; castingPanel_->hide();
    skillSide_->setText(QStringLiteral("战斗回放")); skillSide_->setStyleSheet(QStringLiteral("background:#e1e8f4;color:#52657e;border-radius:5px;font-weight:600"));
    skillTitle_->setText(QStringLiteral("选择战报，开始连续播放")); skillTitle_->setProperty("skillId", QString());
    actionPhase_->setText(QStringLiteral("技能前 → 出手 → 技能后 · 自动衔接"));
    stepLabel_->setText(QStringLiteral("敌我双九宫格 · 文字回放"));
    relation_->setText(QStringLiteral("选择日志，查看每一步谁对谁产生了什么作用"));
    outcome_->setText(QStringLiteral("红色生命 · 蓝色护盾数值 · 橙色气势 · 紫色通灵格 · 绿框召唤 · 粉框元能目标"));
    return;
  }
  const auto& step = replay_.steps[current_];
  const PlaybackPhase* phase = playbackPhase_ >= 0 && playbackPhase_ < playbackPhases_.size() ? &playbackPhases_[playbackPhase_] : nullptr;
  slider_->setPerspective(side_->currentData().toInt());
  // seek() synchronizes the slider while blocking its signals. Explicitly
  // refresh the round strip and the selected local action after that sync.
  slider_->refreshPosition();
  const int globalTurn = slider_->turnAt(current_);
  const int smallTurn = slider_->roundTurnAt(current_);
  const int round = slider_->roundNumberAt(current_);
  smallTurnLabel_->setText(globalTurn == 0 ? QStringLiteral("开局") : round >= 0 ?
    QStringLiteral("第 %1 回合 · 第 %2 小回合").arg(round).arg(smallTurn) :
    QStringLiteral("回合未记录 · 第 %1 小回合").arg(smallTurn));
  smallTurnLabel_->setProperty("smallTurn", smallTurn);
  smallTurnLabel_->setProperty("globalTurn", globalTurn);
  smallTurnLabel_->setProperty("round", round);
  smallTurnLabel_->setToolTip((globalTurn == 0 ? QStringLiteral("尚未开始完整行动；全场共 %1 条完整行动。").arg(slider_->turnCount()) :
    QStringLiteral("全场完整行动：第 %1 / %2 条。").arg(globalTurn).arg(slider_->turnCount())) +
    QStringLiteral("\n小回合在当前大回合内从 1 编号；不把技能内部小步骤算成小回合。\n死亡 / 复活仅依据已记录的生命归零与恢复，不推断触发原因。"));
  if (step.round >= 0) {
    const QSignalBlocker blocker(rounds_);
    for (int i = rounds_->count() - 1; i >= 0; --i) {
      const int first = rounds_->itemData(i).toInt();
      if (first <= current_ && replay_.steps[first].round == step.round) { rounds_->setCurrentIndex(i); break; }
    }
  } else if (ReplayTimeline::isAction(step) || smallTurn == 0) {
    const QSignalBlocker blocker(rounds_);
    rounds_->setPlaceholderText(smallTurn == 0 ? QStringLiteral("开局") : QStringLiteral("回合未记录"));
    rounds_->setCurrentIndex(-1);
  }
  const auto& visible = displayedState();
  int uncertain = step.unlinkedReferences.size() + step.playbackNotes.size();
  for (const auto& fighter : visible) uncertain += ReplayInspection::unknownValues(fighter, replay_.root).size();
  diagnosticsButton_->setText(uncertain ? QStringLiteral("本步核对 · %1 项").arg(uncertain) : QStringLiteral("本步核对"));
  diagnosticsButton_->setProperty("unlinkedCount", step.unlinkedReferences.size());
  diagnosticsButton_->setProperty("reviewCount", uncertain);
  diagnosticsButton_->setStyleSheet(uncertain ? QStringLiteral("color:#9a6324;font-weight:600") : QString());
  const int previousMoment = phase ? phase->beforeMoment : moment_ - 1;
  const auto& before = phase && phase->numericBefore ? *phase->numericBefore : previousMoment >= 0 ? step.moments[previousMoment].state :
    step.parallelBatch ? step.batchStartState : replay_.steps[std::max(0, current_ - 1)].state;
  const Effect* focus = !phase && effect_ >= 0 && effect_ < step.effects.size() ? &step.effects[effect_] : nullptr;
  const QString source = focus ? focus->source : QString();
  const QString target = focus ? focus->target : QString();
  QList<Effect> shownEffects;
  if (phase) { for (const int i : phase->effects) shownEffects.append(step.effects[i]); }
  else shownEffects = moment_ < 0 ? step.effects : step.moments[moment_].effects;
  QString activeActor = phase ? phase->actor : step.actor;
  if (focus && focus->cause == QStringLiteral("counter") && !focus->source.isEmpty()) activeActor = focus->source;
  else if (!phase && moment_ >= 0 && step.moments[moment_].phase == QStringLiteral("counter")) activeActor = step.moments[moment_].actor;
  const auto rawAction = step.raw.toObject();
  const bool attackDisabled = step.playbackKind == QStringLiteral("disabled") || valueText(catalog_[QStringLiteral("execution")].toObject()[valueText(rawAction[QStringLiteral("ses")])].toObject()[QStringLiteral("type")]).toInt() == 2;
  const bool teamSimple = step.playbackKind == QStringLiteral("team-simple");
  const bool canAttack = !attackDisabled && !teamSimple && step.playbackKind != QStringLiteral("unresolved");
  const bool counterStage = activeActor != step.actor;
  const bool actorKnown = visible.contains(activeActor);
  TeamSourceIdentity activeTeamSource;
  if (actorKnown && visible.value(activeActor).representer) {
    const auto owner = visible.value(activeActor);
    const auto progress = phase && (phase->kind == QStringLiteral("prepare") || phase->kind == QStringLiteral("setup")) ? DragonCommandProgress::Before :
      phase && phase->kind != QStringLiteral("finish") ? DragonCommandProgress::Active : DragonCommandProgress::Complete;
    activeTeamSource = teamSourceIdentity(step, owner, dragonRosterAt(replay_, owner, current_, progress, catalog_), catalog_);
  }
  const bool alliedActor = actorKnown && visible.value(activeActor).team == side_->currentData().toInt();
  const auto skillColor = !actorKnown ? QStringLiteral("#52657e") : alliedActor ? QStringLiteral("#205caa") : QStringLiteral("#a73351");
  skillSide_->setText(actorKnown ? (alliedActor ? QStringLiteral("我方") : QStringLiteral("敌方")) + (counterStage ? QStringLiteral("反击") : teamSimple ? QStringLiteral("触发") : attackDisabled ? QStringLiteral("行动") : QStringLiteral("施放")) : QStringLiteral("战斗记录"));
  skillSide_->setStyleSheet(QStringLiteral("background:%1;color:white;border-radius:5px;font-size:12px;font-weight:700").arg(skillColor));
  castingPanel_->setStyleSheet(QStringLiteral("QFrame#replayCastingPanel{background:%1;border:1px solid %2;border-radius:4px}")
    .arg(alliedActor ? QStringLiteral("#edf5ff") : QStringLiteral("#fff0f4"), skillColor));
  QString skillId, buffId, title, skillCategory;
  skillId = counterStage ? step.counterSkillId : step.playbackSkillId;
  if (!skillId.isEmpty()) {
    title = catalog_[QStringLiteral("skills")].toObject()[skillId].toObject()[QStringLiteral("name")].toString();
    if (title.isEmpty()) title = replayPublicReferenceName(catalog_, QStringLiteral("skills"), skillId);
  } else if (counterStage) {
    skillCategory = QStringLiteral("反击"); title = QStringLiteral("普通技能未记录");
  } else if (rawAction.contains(QStringLiteral("epbi")) && visible.value(step.actor).representer) {
    buffId = valueText(rawAction[QStringLiteral("epbi")]);
    skillCategory = QStringLiteral("队伍效果"); title = replayPublicReferenceName(catalog_, QStringLiteral("buffs"), buffId);
  } else if (rawAction.contains(QStringLiteral("si"))) {
    skillId = step.loggedSkillId.isEmpty() ? valueText(rawAction[QStringLiteral("si")]) : step.loggedSkillId;
    title = replayPublicReferenceName(catalog_, QStringLiteral("skills"), skillId);
  } else title = step.title;
  if (!skillId.isEmpty()) {
    skillCategory = catalog_[QStringLiteral("skills")].toObject()[skillId].toObject()[QStringLiteral("skillBaseType")].toString();
    if (skillCategory.isEmpty()) skillCategory = QStringLiteral("技能类型未收录");
    if (actorKnown && visible.value(activeActor).representer) skillCategory.prepend(QStringLiteral("队伍技能 · "));
  }
  if (!skillCategory.isEmpty()) {
    title.prepend(skillCategory + QStringLiteral(" · "));
    if (actorKnown) {
      const auto& actor = visible.value(activeActor);
      const auto actorName = actor.representer && !activeTeamSource.name.isEmpty() ? activeTeamSource.name : replayFighterName(actor, catalog_);
      if (buffId.isEmpty() || actorName != replayPublicReferenceName(catalog_, QStringLiteral("buffs"), buffId))
        title.prepend(actorName + QStringLiteral(" · "));
    }
  }
  if (attackDisabled && !step.executionName.isEmpty()) title = step.executionName + QStringLiteral(" · ") + title;
  if (!counterStage && step.lastSkillStatus == QStringLiteral("applied")) title += QStringLiteral(" · 末击");
  if (!counterStage && step.lastSkillStatus == QStringLiteral("missing")) title += QStringLiteral(" · 末击配置待核实");
  skillTitle_->setProperty("skillId", skillId); skillTitle_->setProperty("buffId", buffId);
  skillTitle_->setProperty("loggedSkillId", step.loggedSkillId);
  skillTitle_->setProperty("playbackKind", step.playbackKind);
  skillTitle_->setProperty("lastSkillStatus", step.lastSkillStatus);
  skillTitle_->setAccessibleName(title);
  const auto href = !skillId.isEmpty() ? QStringLiteral("skill:") + skillId : !buffId.isEmpty() ? QStringLiteral("buff:") + buffId : QString();
  skillTitle_->setText(href.isEmpty() ? esc(title) : QStringLiteral("<a href='%1' style='color:%2;text-decoration:none'>%3</a>").arg(esc(href), skillColor, esc(title)));
  skillTitle_->setToolTip(title + (href.isEmpty() ? QString() : QStringLiteral("\n点击查看技能或效果说明")));
  actionPhase_->setText((replay_.containerKind == QStringLiteral("team") ? QStringLiteral("组队第 %1/%2 项 · ").arg(replay_.selectedBattleIndex + 1).arg(replay_.containerBattleCount) : QString()) +
    (step.parallelBatch ? (step.lineIndex >= 0 ? QStringLiteral("战线 %1 · ").arg(step.lineIndex + 1) : QString()) +
      QStringLiteral("同批 %1（分线查看） · ").arg(step.batchId + 1) : QString()) +
    (step.round >= 0 ? QStringLiteral("第 %1 回合 · ").arg(step.round) : QString()) +
    (phase ? phase->title : moment_ < 0 ? QStringLiteral("完整行动") : step.moments[moment_].title) +
    (step.overRoundLimit.value_or(false) ? QStringLiteral(" · 超出回合限制") : QString()));
  actionPhase_->setProperty("parallelBatch", step.parallelBatch);
  actionPhase_->setProperty("batchId", step.batchId);
  actionPhase_->setProperty("lineIndex", step.lineIndex);
  if (!step.parallelConflictKeys.isEmpty()) actionPhase_->setText(actionPhase_->text() + QStringLiteral(" · 跨线影响顺序待核实"));
  actionPhase_->setProperty("overRoundLimit", step.overRoundLimit.value_or(false));
  actionPhase_->setToolTip(actionPhase_->text());
  if (phase && phase->numericState) actionPhase_->setToolTip(actionPhase_->toolTip() +
    QStringLiteral("\n按已确认的数值列表顺序逐段阅读，条内为该段记录余额；同组对象并行展示，不推定跨对象的动画完成先后。"));
  if (phase) {
    QStringList trail;
    QStringList kinds, labels;
    int currentSegment = -1;
    for (int i = 0; i < playbackPhases_.size(); ++i) {
      const auto& p = playbackPhases_[i];
      QString kind = p.kind;
      if (kind == QStringLiteral("prepare")) continue;
      if (kind == QStringLiteral("cast")) kind = QStringLiteral("attack");
      if (kind == QStringLiteral("counter-cast")) kind = QStringLiteral("counter");
      if (kinds.isEmpty() || kinds.last() != kind) {
        kinds.append(kind);
        labels.append(kind == QStringLiteral("before") ? (teamSimple ? QStringLiteral("触发前") : QStringLiteral("技能前")) : kind == QStringLiteral("attack") ? QStringLiteral("出手") :
          kind == QStringLiteral("counter") ? QStringLiteral("反击") : kind == QStringLiteral("after") ? (teamSimple ? QStringLiteral("触发后") : QStringLiteral("技能后")) :
          kind == QStringLiteral("finish") ? QStringLiteral("完成") : kind == QStringLiteral("unresolved") ? QStringLiteral("顺序待核实") : QStringLiteral("状态变化"));
      }
      if (i == playbackPhase_) currentSegment = kinds.size() - 1;
    }
    for (int i = 0; i < labels.size(); ++i)
      trail.append(QStringLiteral("<span style='color:%1;font-weight:%2'>%3</span>").arg(i == currentSegment ? skillColor : i < currentSegment ? QStringLiteral("#5c7899") : QStringLiteral("#8996a8"), i == currentSegment ? QStringLiteral("700") : QStringLiteral("400"), labels[i]));
    phaseTrail_->setText(trail.join(QStringLiteral("&nbsp; → &nbsp;")));
  } else phaseTrail_->clear();
  QMap<QString, ReplayUnitOverview> overview;
  for (const auto& fighter : visible) overview[fighter.key].visualBadge = QString();
  QStringList actionTargets;
  QMap<QString, QStringList> unitRecords;
  QMap<QString, int> addedBuffs, removedBuffs, unmatchedBuffs;
  int totalAdded = 0, totalRemoved = 0;
  if (!focus) {
    if (phase && phase->targets && canAttack) {
      if (counterStage) actionTargets.append(step.actor);
      else for (const auto& e : step.effects)
        if (e.kind == QStringLiteral("target") && !inactiveTargetRecord(e) && !actionTargets.contains(e.target)) actionTargets.append(e.target);
      for (const auto& key : actionTargets) overview[key].target = true;
    }
    QSet<QString> directChanges;
    for (const auto& e : shownEffects) if (e.changed && !e.source.isEmpty() && e.source == step.actor) directChanges.insert(e.target);
    for (const auto& e : shownEffects) {
      // Only trl target relations establish action targets. A Buff change on
      // another unit does not establish that the actor applied it.
      if (canAttack && e.kind == QStringLiteral("target") && e.source == step.actor && !step.actor.isEmpty() && (!phase || phase->targets)) {
        const bool inactive = inactiveTargetRecord(e) && !directChanges.contains(e.target);
        if (!inactive) {
          overview[e.target].target = true;
          if (!actionTargets.contains(e.target)) actionTargets.append(e.target);
        }
      }
      if (activeActor != step.actor && e.cause == QStringLiteral("counter") && e.source == activeActor && e.changed && (!phase || e.target != activeActor)) {
        overview[e.target].target = true;
        if (!actionTargets.contains(e.target)) actionTargets.append(e.target);
      }
      if (phase && (e.changed || e.recordedDelta.value_or(0) != 0) && (e.target != activeActor || actionTargets.contains(e.target))) {
        // A receiving card can light up even when the Buff applier is absent.
        // Only a recorded source is allowed to create a connecting arrow.
        overview[e.target].target = true;
        if (canAttack && e.source == activeActor && e.source != e.target && !actionTargets.contains(e.target)) actionTargets.append(e.target);
      }
      if (e.kind == QStringLiteral("buff")) {
        if (ReplayBuffHistory::isAddition(e)) { ++addedBuffs[e.target]; ++totalAdded; }
        else {
          ++removedBuffs[e.target]; ++totalRemoved;
          if (e.before == 0) ++unmatchedBuffs[e.target];
        }
      }
      if (meaningfulEffect(e)) {
        const auto origin = e.source.isEmpty() ? (e.cause == QStringLiteral("record") || e.cause == QStringLiteral("calibration") ? QStringLiteral("日志状态记录") : QStringLiteral("施加者未记录")) : unitCaption(e.source);
        unitRecords[e.target].append(origin + QStringLiteral("：") + effectSummary(e));
      }
    }
    if (current_ > 0) for (const auto& f : visible) {
      if (f.representer) continue;
      const auto old = before.value(f.key);
      QStringList changes, values;
      const auto valueChange = [&](const QString& name, std::optional<qint64> a, std::optional<qint64> b) {
        if (a == b) return;
        values.append(name + QStringLiteral(" %1 → %2").arg(number(a), number(b)));
        changes.append(a && b ? name + QStringLiteral(" %1%2").arg(*b > *a ? QStringLiteral("+") : QString(), QString::number(*b - *a))
                             : name + QStringLiteral(" → ") + number(b));
      };
      valueChange(QStringLiteral("生命"), old.hp, f.hp);
      valueChange(QStringLiteral("护盾"), old.shield, f.shield);
      valueChange(QStringLiteral("气势"), old.power, f.power);
      if (!changes.isEmpty()) changes.prepend(QStringLiteral("本步净变化"));
      // Count operations, not final membership: an add followed by removal
      // must remain visible even when the end-state Buff list is unchanged.
      if (addedBuffs.value(f.key) || removedBuffs.value(f.key))
        changes.append(QStringLiteral("Buff +%1 / −%2").arg(addedBuffs.value(f.key)).arg(removedBuffs.value(f.key)));
      if (unmatchedBuffs.value(f.key)) changes.append(QStringLiteral("移除未匹配 %1 条").arg(unmatchedBuffs.value(f.key)));
      auto& item = overview[f.key]; item.summary = changes.join(QLatin1Char('\n'));
      item.detail = unitCaption(f.key) + QLatin1Char('\n') + values.join(QLatin1Char('\n')) +
        QStringLiteral("\n数值为本步前后净变化；Buff 的 +/− 为获得/移除记录条数。\n按日志顺序：\n") + unitRecords.value(f.key).join(QLatin1Char('\n'));
    }
  }
  // Every displayed event lives on its recipient's card. These are recorded
  // operations in order, not a simulated damage formula or a net replacement.
  for (int i = 0; i < step.effects.size(); ++i) {
    const auto& e = step.effects[i];
    if (e.kind == QStringLiteral("target") || e.kind == QStringLiteral("team-target")) {
      const bool relevant = phase ? phase->outcomes && phase->effects.contains(i) :
        focus ? (effect_ == i || (focus->source == step.actor && focus->target == e.target)) :
        (moment_ < 0 || (moment_ >= 0 && step.moments[moment_].phase == QStringLiteral("attack")));
      const bool teamRecordShown = e.kind == QStringLiteral("team-target") && (phase ? phase->effects.contains(i) : moment_ < 0 || e.moment == moment_);
      if ((relevant || teamRecordShown) && !inactiveTargetRecord(e)) {
        const auto flags = e.raw.toObject(); QStringList labels;
        if (flags[QStringLiteral("ishit")].isBool() && !flags[QStringLiteral("ishit")].toBool()) labels.append(QStringLiteral("未命中"));
        for (const auto& flag : QList<QPair<QString, QString>>{{"isbl", "格挡"}, {"iscr", "被暴击"}, {"isIm", "免疫"}, {"issb", "反击标记"}})
          if (flags[flag.first].toBool()) labels.append(flag.second);
        if (flags[QStringLiteral("isse")].isBool() && !flags[QStringLiteral("isse")].toBool()) labels.append(QStringLiteral("技能未生效"));
        auto badges = overview[e.target].badge.split(QStringLiteral(" · "), Qt::SkipEmptyParts);
        badges.append(labels); badges.removeDuplicates(); overview[e.target].badge = badges.join(QStringLiteral(" · "));
        // Preserve all raw flags above. The prominent animation follows the
        // official branch: miss, else immunity, else block and critical.
        // A disabled target result must not look like a successful impact.
        QStringList visuals;
        const bool playsHitResult = e.kind == QStringLiteral("target") &&
          (step.playbackKind == QStringLiteral("normal") || step.playbackKind == QStringLiteral("team-skill"));
        // Disabled, simple-team, summon and unresolved branches retain their
        // flags as records, but never execute the official hit renderer.
        if (playsHitResult && flags[QStringLiteral("isse")].isBool() && !flags[QStringLiteral("isse")].toBool()) {
          visuals.append(QStringLiteral("技能未生效"));
        } else if (playsHitResult && flags[QStringLiteral("isse")].toBool()) {
          if (flags[QStringLiteral("ishit")].isBool() && !flags[QStringLiteral("ishit")].toBool()) visuals.append(QStringLiteral("闪避"));
          else if (flags[QStringLiteral("isIm")].toBool()) visuals.append(QStringLiteral("免疫"));
          else {
            if (flags[QStringLiteral("isbl")].toBool()) visuals.append(QStringLiteral("格挡"));
            if (flags[QStringLiteral("iscr")].toBool()) visuals.append(QStringLiteral("被暴击"));
          }
          if (flags[QStringLiteral("issb")].toBool()) visuals.append(QStringLiteral("反击标记"));
        }
        addVisualBadges(overview[e.target], visuals);
      }
      // Target outcomes are already on the recipient as badges; do not add
      // another text block between its meters and Buffs.
      continue;
    }
    if (phase ? !phase->effects.contains(i) : moment_ >= 0 && e.moment != moment_) continue;
    if (!meaningfulEffect(e) && effect_ != i) continue;
    ReplayUnitEvent event; event.index = i; event.selected = effect_ == i;
    event.color = QStringLiteral("#765899");
    if (e.kind == QStringLiteral("buff")) {
      event.buffId = e.causeId; event.removing = !ReplayBuffHistory::isAddition(e);
      event.restoresDisplayLayers = e.restoresDisplayLayers;
      event.buffSource = e.buffSource; event.buffSourceName = e.buffSourceName; event.buffSourceTeam = e.buffSourceTeam; event.buffSourcePath = e.buffSourcePath;
      event.label = (event.removing ? QStringLiteral("− ") : QStringLiteral("＋ ")) + replayPublicReferenceName(catalog_, QStringLiteral("buffs"), e.causeId);
      if (event.removing && e.before == 0) event.label += QStringLiteral("（无匹配）");
      event.color = event.removing ? QStringLiteral("#b74761") : QStringLiteral("#167447");
      if (e.restoresDisplayLayers) event.label = QStringLiteral("恢复显示");
      if (e.visibilityCommand == QStringLiteral("show-up")) {
        event.label += QStringLiteral(" · 显现");
        auto& badge = overview[e.target].badge;
        badge = badge.isEmpty() ? QStringLiteral("显现") : badge + QStringLiteral(" · 显现");
        addVisualBadges(overview[e.target], {QStringLiteral("显现")});
      }
    } else if (!e.metric.isEmpty() && (e.recordedDelta || e.delta || e.before || e.after)) {
      event.metric = e.metric;
      if (e.metric != QStringLiteral("生命上限")) { event.observedBefore = e.before; event.observedAfter = e.after; }
      const auto delta = e.recordedDelta ? e.recordedDelta : e.delta;
      event.valueText = delta ? (*delta > 0 ? QStringLiteral("+") : QString()) + QString::number(*delta)
        : number(e.before) + QStringLiteral(" → ") + number(e.after);
      if (e.cause == QStringLiteral("calibration")) event.valueText.prepend(QStringLiteral("校准 "));
      if (e.metric == QStringLiteral("生命上限")) { event.metric = QStringLiteral("生命"); event.valueText.prepend(QStringLiteral("上限 ")); }
      event.label = e.metric + QLatin1Char(' ') + event.valueText;
      if (e.metric == QStringLiteral("生命") && e.cause == QStringLiteral("buff")) {
        const auto mark = catalog_[QStringLiteral("buffs")].toObject()[e.causeId].toObject()[QStringLiteral("bhs")].toString();
        const auto allowed = catalog_[QStringLiteral("clientPresentation")].toObject()[QStringLiteral("buffHarmSources")].toArray();
        if (!mark.isEmpty() && allowed.contains(mark)) { event.harmLabel = mark; event.label.prepend(mark + QLatin1Char(' ')); }
      }
      event.color = e.metric == QStringLiteral("护盾") ? QStringLiteral("#246fc2") : e.metric == QStringLiteral("气势") ? QStringLiteral("#a66817") :
        delta && *delta < 0 ? QStringLiteral("#bd3858") : QStringLiteral("#198253");
    } else event.label = effectSummary(e);
    if (e.kind != QStringLiteral("buff")) {
      const auto origin = e.source.isEmpty() ? (e.cause == QStringLiteral("calibration") ? QStringLiteral("校准") : e.cause == QStringLiteral("record") ? QStringLiteral("记录") : QStringLiteral("来源未记")) :
        e.source == e.target ? QStringLiteral("自身") : replayFighterName(visible.value(e.source), catalog_);
      event.label = origin + QStringLiteral(" · ") + event.label;
    }
    event.detail = (e.source.isEmpty() ? QStringLiteral("施加者未记录") : unitCaption(e.source)) + QStringLiteral(" → ") + unitCaption(e.target) + QLatin1Char('\n') + effectSummary(e);
    if (!event.harmLabel.isEmpty()) event.detail += QStringLiteral("\n官方数值标注：") + event.harmLabel;
    if (e.buffWaitType > 0)
      event.detail += e.buffWaitType == 2 ? QStringLiteral("\n此对象的等待 Buff 按顺序触发。") : QStringLiteral("\n此对象的该批 Buff 并行等待完成。") ;
    if (e.afterWaitCallback) event.detail += QStringLiteral("\n该条属于等待回调的后续记录；不同对象之间的精确动画先后未记录。");
    if (e.buffWaitCondition == QStringLiteral("visible-or-skip"))
      event.detail += QStringLiteral("\n官方在单位动画可见时等待；动画隐藏时可跳过等待。战报未记录透明度，这里保留配置队列，不据生命值猜测隐藏状态。");
    if (e.visibilityCommand == QStringLiteral("show-up")) event.detail += QStringLiteral("\n官方显示指令；没有据此改变生命值或判定复活。");
    if (e.restoresDisplayLayers) event.detail += QStringLiteral("\n官方 BattleUtil.handleOneBuff 在本条新增、等待类型>0且召唤/已呼出分身条件满足时调用 summonAppear，恢复显示层；不改登场标志和生命。\n记录路径：") + e.path;
    auto& item = overview[e.target]; item.events.append(event);
    if (meaningfulEffect(e) && (e.kind == QStringLiteral("value") || e.kind == QStringLiteral("progress")) && e.cause == QStringLiteral("buff") && e.causeId.toLongLong() > 0) {
      // A numeric record names the Buff responsible for this operation, but
      // does not identify who originally applied that Buff. Keep that unknown.
      // Nonpositive IDs also carry numerical presentation markers in the
      // official client; they are not evidence of a Buff triggering.
      ReplayUnitEvent trigger;
      trigger.index = i; trigger.selected = event.selected; trigger.buffId = e.causeId; trigger.buffTriggered = true;
      trigger.label = QStringLiteral("触发 · ") + replayPublicReferenceName(catalog_, QStringLiteral("buffs"), e.causeId); trigger.color = QStringLiteral("#79569b"); trigger.detail = event.detail;
      item.events.append(trigger);
    }
    if (focus) item.detail += event.detail + QLatin1Char('\n');
  }
  // The transient Buff notices use the report's battle round (csn), not the
  // per-player action counter (pr). Rebuild from the visible prefix so seeks,
  // perspective changes and pauses cannot leak future or previous-round cues.
  // Keep this history separate from phase events and the current multiset.
  const auto& actionBeginning = !step.moments.isEmpty() ? step.moments.first().state :
    step.parallelBatch ? step.batchStartState : replay_.steps[std::max(0, current_ - 1)].state;
  const bool actionMetersPending = current_ > 0 && (phase ? phase->kind != QStringLiteral("finish") :
    moment_ >= 0 && step.moments[moment_].phase != QStringLiteral("settle"));
  ReplayActionMeters::Window meterWindow;
  if (phase) {
    meterWindow.groups = meterEventGroups_; meterWindow.current = playbackPhase_;
    auto actor = step.actor;
    for (int n = 0; n <= playbackPhase_; ++n) {
      const auto& next = playbackPhases_[n].actor;
      if (!next.isEmpty() && next != actor) { meterWindow.first = n; actor = next; }
    }
  }
  ReplayActionMeters::populate(step, actionBeginning, visible, catalog_,
    moment_ >= 0 ? moment_ : int(step.moments.size()) - 1, actionMetersPending, overview, phase ? &meterWindow : nullptr);
  if (step.round >= 0) {
    for (const auto& fighter : visible) {
      overview[fighter.key].roundBuffEvents.emplace();
      overview[fighter.key].buffRound = step.round;
    }
    int first = current_;
    while (first > 0 && replay_.steps[first - 1].round == step.round) --first;
    for (int n = current_; n >= first; --n) {
      const auto& recorded = replay_.steps[n];
      // A different simultaneous lane has no observed completion order.
      if (n != current_ && step.parallelBatch && recorded.batchId == step.batchId) continue;
      for (int i = recorded.effects.size() - 1; i >= 0; --i) {
        const auto& e = recorded.effects[i];
        if (!visible.contains(e.target) || (n == current_ && moment_ >= 0 && (e.moment < 0 || e.moment > moment_))) continue;
        if (n == current_ && phase && meterEventGroups_.value(i, -1) > playbackPhase_) continue;
        const bool membership = e.kind == QStringLiteral("buff");
        const bool trigger = (e.kind == QStringLiteral("value") || e.kind == QStringLiteral("progress")) && meaningfulEffect(e) && e.cause == QStringLiteral("buff") && e.causeId.toLongLong() > 0;
        if (!membership && !trigger) continue;
        ReplayUnitEvent event; event.index = i; event.recordStep = n; event.selected = n == current_ && effect_ == i;
        event.buffId = e.causeId; event.buffTriggered = trigger;
        event.restoresDisplayLayers = e.restoresDisplayLayers;
        event.removing = membership && !ReplayBuffHistory::isAddition(e);
        if (membership) {
          event.buffSource = e.buffSource; event.buffSourceName = e.buffSourceName;
          event.buffSourceTeam = e.buffSourceTeam; event.buffSourcePath = e.buffSourcePath;
        }
        event.label = (trigger ? QStringLiteral("触发 · ") : event.removing ? QStringLiteral("− ") : QStringLiteral("＋ ")) + e.causeName;
        if (e.restoresDisplayLayers) event.label = QStringLiteral("恢复显示");
        if (event.removing && e.before == 0) event.label += QStringLiteral("（无匹配）");
        if (e.visibilityCommand == QStringLiteral("show-up")) event.label += QStringLiteral(" · 显现");
        event.color = trigger ? QStringLiteral("#79569b") : event.removing ? QStringLiteral("#b74761") : QStringLiteral("#167447");
        const auto& atEvent = e.moment >= 0 && e.moment < recorded.moments.size() ? recorded.moments[e.moment].state : recorded.state;
        event.detail = QStringLiteral("第 %1 回合 · 行动 %2\n%3\n%4\n记录路径：%5\n点击回看这条记录。")
          .arg(recorded.round).arg(n).arg(replayFighterName(atEvent.value(e.target), catalog_), effectSummary(e), e.path);
        if (e.restoresDisplayLayers) event.detail += QStringLiteral("\n官方 summonAppear 恢复显示层；不表示新增召唤、登场或复活。");
        overview[e.target].roundBuffEvents->append(event);
      }
    }
  }
  // Reconstruct only the recorded special units visible at this boundary.
  // This local presentation copy preserves intermediate changes in one group;
  // it never writes back to the replay's state or numeric ledger.
  QMap<QString, Fighter> visualStates;
  QMap<QString, int> visualMoments;
  QMap<QString, QSet<QString>> detailedTransitionKinds;
  for (int i = 0; i < step.effects.size(); ++i) {
    const auto& effect = step.effects[i];
    if (effect.kind != QStringLiteral("transform") && effect.kind != QStringLiteral("attributes")) continue;
    if (phase ? !phase->effects.contains(i) : moment_ >= 0 && effect.moment != moment_) continue;
    if (!visible.contains(effect.target) || visible.value(effect.target).representer || !effect.changed) continue;
    if (!visualMoments.contains(effect.target) || visualMoments.value(effect.target) != effect.moment) {
      const auto& stateBeforeRecord = effect.moment > 0 && effect.moment <= step.moments.size()
        ? step.moments[effect.moment - 1].state : before;
      visualStates[effect.target] = stateBeforeRecord.value(effect.target);
      visualMoments[effect.target] = effect.moment;
    }
    const auto prior = visualStates.value(effect.target); if (prior.key.isEmpty()) continue;
    auto afterRecord = prior;
    const auto raw = effect.raw.toObject();
    afterRecord.attributes = raw[QStringLiteral("ca")].toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
    afterRecord.attributesKnown = raw[QStringLiteral("ca")].isString();
    if (effect.kind == QStringLiteral("transform")) {
      afterRecord.raceId = recordedIntegerId(raw[QStringLiteral("cr")]);
      afterRecord.faceId = recordedIntegerId(raw[QStringLiteral("cf")]);
      afterRecord.normalSkill = recordedIntegerId(raw[QStringLiteral("cn")]);
      const auto unique = recordedIntegerId(raw[QStringLiteral("cu")].toString().section(QLatin1Char(','), 0, 0));
      afterRecord.uniqueSkill = unique == QStringLiteral("0") ? recordedIntegerId(raw[QStringLiteral("scu")]) : unique;
      if (afterRecord.originRaceId.toLongLong() <= 0 && prior.raceId.toLongLong() > 0) {
        afterRecord.originRaceId = prior.raceId; afterRecord.originFighterKey = prior.key;
        afterRecord.originRelation = QStringLiteral("transform"); afterRecord.originPath = effect.path;
      }
      updateFormReference(afterRecord, catalog_);
    }
    auto& cue = overview[effect.target];
    const auto add = [&](const QString& kind, const QString& label, const QString& explanation) {
      ReplayVisualCue visual; visual.kind = kind; visual.label = label;
      visual.detail = explanation + QStringLiteral("\n记录：") + effect.path + QLatin1Char('\n') + pretty(effect.raw);
      visual.eventIndex = i; visual.applier = effect.source;
      if (visible.contains(effect.source)) { visual.applierName = replayFighterName(visible.value(effect.source), catalog_); visual.applierTeam = visible.value(effect.source).team; }
      cue.visuals.append(visual); detailedTransitionKinds[effect.target].insert(kind);
    };
    if (prior.raceId != afterRecord.raceId || prior.faceId != afterRecord.faceId) {
      const auto oldName = replayFighterName(prior, catalog_), newName = replayFighterName(afterRecord, catalog_);
      const auto names = oldName == newName ? newName : oldName + QStringLiteral(" → ") + newName;
      add(QStringLiteral("transform"), QStringLiteral("形态变化\n") + names,
        QStringLiteral("原形态：%1 [%2]；当前形态：%3 [%4]；外观 %5 → %6。原名仅使用已核实映射。")
          .arg(oldName, prior.raceId, newName, afterRecord.raceId, prior.faceId, afterRecord.faceId));
    }
    if (prior.attributes != afterRecord.attributes || prior.attributesKnown != afterRecord.attributesKnown) {
      const auto oldAttributes = prior.attributesKnown ? cueAttributeNames(prior.attributes, catalog_) : QStringLiteral("未记录");
      const auto newAttributes = afterRecord.attributesKnown ? cueAttributeNames(afterRecord.attributes, catalog_) : QStringLiteral("待核实");
      add(QStringLiteral("attributes"), QStringLiteral("属性\n%1 → %2").arg(oldAttributes, newAttributes),
        QStringLiteral("属性 %1 → %2；不据变化推测元能、幻元或施加者。").arg(oldAttributes, newAttributes));
    }
    if (prior.normalSkill != afterRecord.normalSkill)
      add(QStringLiteral("skill-swap"), QStringLiteral("普通技已换\n%1 → %2").arg(cueSkillName(prior.normalSkill, catalog_), cueSkillName(afterRecord.normalSkill, catalog_)),
        QStringLiteral("普通技能 %1 → %2").arg(prior.normalSkill, afterRecord.normalSkill));
    if (prior.uniqueSkill != afterRecord.uniqueSkill)
      add(QStringLiteral("skill-swap"), QStringLiteral("超杀已换\n%1 → %2").arg(cueSkillName(prior.uniqueSkill, catalog_), cueSkillName(afterRecord.uniqueSkill, catalog_)),
        QStringLiteral("当前超杀 %1 → %2；按官方 cu 为 0 时使用 scu 的记录规则。").arg(prior.uniqueSkill, afterRecord.uniqueSkill));
    visualStates[effect.target] = afterRecord;
  }
  const bool showInitialOrigins = current_ == 0;
  if (showInitialOrigins) {
    const auto addInitialOrigin = [&](const EmpowermentEvidence& evidence, bool self) {
      if (!visible.contains(evidence.target) || !visible.contains(evidence.source)) return;
      ReplayVisualCue cue; cue.kind = self ? QStringLiteral("self-transform") : QStringLiteral("empowerment");
      const auto from = cueAttributeNames(evidence.beforeAttributes, catalog_), to = cueAttributeNames(evidence.afterAttributes, catalog_);
      const auto provider = replayFighterName(visible.value(evidence.source), catalog_);
      cue.label = (self ? QStringLiteral("开局幻元\n") : QStringLiteral("开局元能 · %1\n").arg(provider)) + QStringLiteral("原生%1 → 开局%2").arg(from, to);
      cue.detail = QStringLiteral("官方原生属性 → 回包开局属性，属于开局证据，不是战斗中的一帧施法。\n") + evidence.description + QStringLiteral("\n官方效果：") + evidence.officialEffect;
      cue.derived = true; cue.evidenceType = QStringLiteral("official-initial-match");
      cue.applier = evidence.source; cue.applierName = provider; cue.applierTeam = visible.value(evidence.source).team;
      overview[evidence.target].visuals.append(cue);
    };
    for (const auto& evidence : empowermentEvidence_) addInitialOrigin(evidence, false);
    for (const auto& evidence : selfTransformEvidence_) addInitialOrigin(evidence, true);
  }
  for (const auto& f : visible) {
    auto& cue = overview[f.key]; const auto old = before.value(f.key);
    if (current_ > 0) {
      auto badges = cue.badge.split(QStringLiteral(" · "), Qt::SkipEmptyParts);
      const int eventFirst = phase ? (phase->beforeMoment == phase->lastMoment ? -1 : phase->firstMoment) : moment_ >= 0 ? moment_ : 0;
      const int eventLast = phase ? phase->lastMoment : moment_ >= 0 ? moment_ : step.moments.size() - 1;
      QStringList transitions;
      if (eventFirst >= 0 && !(phase && phase->numericState)) for (const auto& event : step.events)
        if (event.key == f.key && event.moment >= eventFirst && event.moment <= eventLast &&
            (event.kind == QStringLiteral("生命归零") || event.kind == QStringLiteral("生命恢复") || event.kind == QStringLiteral("变身") ||
             event.kind == QStringLiteral("登场") || event.kind == QStringLiteral("属性变化") || event.kind == QStringLiteral("生命上限变化"))) {
          const auto label = event.kind == QStringLiteral("登场")
            ? (f.clone ? QStringLiteral("分身登场") : f.summoned ? QStringLiteral("召唤登场") : event.kind) : event.kind;
          if (transitions.isEmpty() || transitions.last() != label) transitions.append(label);
        }
      if (!transitions.isEmpty()) {
        badges.append(transitions.join(QStringLiteral(" → ")));
        cue.detail += QStringLiteral("\n本阶段已记录过程：") + transitions.join(QStringLiteral(" → "));
        auto visualTransitions = transitions;
        if (detailedTransitionKinds.value(f.key).contains(QStringLiteral("transform"))) visualTransitions.removeAll(QStringLiteral("变身"));
        if (detailedTransitionKinds.value(f.key).contains(QStringLiteral("attributes"))) visualTransitions.removeAll(QStringLiteral("属性变化"));
        addVisualBadges(cue, visualTransitions);
      } else if (phase || moment_ >= 0) {
        const auto changes = stateTransitionBadges(old, f);
        badges.append(changes);
        auto visualChanges = changes;
        if (detailedTransitionKinds.value(f.key).contains(QStringLiteral("transform"))) visualChanges.removeAll(QStringLiteral("变身"));
        if (detailedTransitionKinds.value(f.key).contains(QStringLiteral("attributes"))) visualChanges.removeAll(QStringLiteral("属性变化"));
        addVisualBadges(cue, visualChanges);
      }
      badges.removeDuplicates(); cue.badge = badges.join(QStringLiteral(" · "));
    }
    if (phase && visibleStateChanged(old, f)) {
      if (f.key != activeActor || actionTargets.contains(f.key)) cue.target = true;
      if ((old.hp && !f.hp) || (old.shield && !f.shield) || (old.power && !f.power) || (old.entryKnown && !f.entryKnown) || (old.buffsKnown && !f.buffsKnown))
        cue.badge = cue.badge.isEmpty() ? QStringLiteral("状态待核实") : cue.badge + QStringLiteral(" · 状态待核实");
    }
    cue.hpBefore = old.hp; cue.maxHpBefore = old.maxHp;
    cue.powerBefore = old.power; cue.maxPowerBefore = old.maxPower;
    cue.animate = phase && animations_->isChecked();
    cue.transitionMs = phase ? playbackDuration() : 350;
  }
  if (attackDisabled && !activeActor.isEmpty() && !step.executionName.isEmpty()) {
    auto& cue = overview[activeActor];
    cue.badge = cue.badge.isEmpty() ? step.executionName : step.executionName + QStringLiteral(" · ") + cue.badge;
    addVisualBadges(cue, {step.executionName});
  }
  if (counterStage && !activeActor.isEmpty()) addVisualBadges(overview[activeActor], {QStringLiteral("反击")});
  // Official-client cues use the state at the actual hit boundary. Never
  // derive them from a later transform, final HP, or a future Buff change.
  const bool clientCuePhase = !phase || ((phase->kind == QStringLiteral("attack") || phase->kind == QStringLiteral("counter")) &&
    (!phase->numericState || phase->numericSegment + 1 == phase->numericSegmentCount));
  const int cueFirst = !clientCuePhase ? -1 : phase ? phase->firstMoment : moment_ >= 0 ? moment_ : focus ? focus->moment : 0;
  const int cueLast = phase ? phase->lastMoment : moment_ >= 0 ? moment_ : focus ? focus->moment : step.moments.size() - 1;
  QSet<QString> cueKeys;
  QList<ClientPresentationCue> relationCues;
  if (cueFirst >= 0) for (int m = cueFirst; m <= cueLast && m < step.moments.size(); ++m) {
    const auto& prior = m > 0 ? step.moments[m - 1].state :
      step.parallelBatch ? step.batchStartState : replay_.steps[std::max(0, current_ - 1)].state;
    for (const auto& cue : clientPresentationCues(step, step.moments[m], prior, catalog_)) {
      if (!visible.contains(cue.recipient)) continue;
      const auto cueKey = cue.recipient + QLatin1Char('|') + cue.kind + QLatin1Char('|') + cue.source + QLatin1Char('|') + cue.target + QLatin1Char('|') + cue.label;
      if (cueKeys.contains(cueKey)) continue;
      cueKeys.insert(cueKey);
      if (cue.kind == QStringLiteral("strong") || cue.kind == QStringLiteral("weak")) {
        relationCues.append(cue);
        continue; // Attribute effectiveness is carried by the matching arrow.
      }
      auto& item = overview[cue.recipient];
      auto labels = item.clientBadge.split(QStringLiteral(" · "), Qt::SkipEmptyParts);
      labels.append(cue.label); labels.removeDuplicates(); item.clientBadge = labels.join(QStringLiteral(" · "));
      if (!item.clientDetail.isEmpty()) item.clientDetail += QStringLiteral("\n\n");
      item.clientDetail += replayFighterName(prior.value(cue.source), catalog_) + QStringLiteral(" → ") + replayFighterName(prior.value(cue.target), catalog_) + QLatin1Char('\n') + cue.explanation;
      if (!cue.evidence.isEmpty()) item.clientDetail += QStringLiteral("\n依据：") + cue.evidence.join(QStringLiteral("；"));
    }
  }
  stepLabel_->setText(QStringLiteral("%1 / %2  ·  %3  ·  %4").arg(current_).arg(replay_.steps.size() - 1).arg(step.title,
    moment_ < 0 ? QStringLiteral("行动结束状态") : step.moments[moment_].title));
  if (focus) {
    const QString origin = source.isEmpty() ? (focus->cause == QStringLiteral("record") || focus->cause == QStringLiteral("calibration") ? QStringLiteral("日志状态记录") : QStringLiteral("施加者未记录")) : unitCaption(source);
    relation_->setText(origin + QStringLiteral("  →  ") + unitCaption(target));
    outcome_->setText(focus->causeName + QStringLiteral("  ·  ") + effectSummary(*focus));
    relation_->setToolTip(relation_->text()); outcome_->setToolTip(focus->text);
  } else {
    QStringList captions, results;
    for (const auto& key : actionTargets) {
      const auto caption = unitCaption(key); captions.append(caption);
    }
    const auto actor = activeActor.isEmpty() || visible.value(activeActor).representer ? step.title : unitCaption(activeActor);
    const auto recipients = actionTargets.size() == 1 ? unitCaption(actionTargets.first()) : QStringLiteral("%1 个橙框目标").arg(actionTargets.size());
    relation_->setText(actor + (actionTargets.isEmpty() ? QString() : QStringLiteral("  →  ") + recipients));
    relation_->setToolTip(actor + QStringLiteral("\n目标：\n") + captions.join(QLatin1Char('\n')));
    if (activeActor != step.actor) results.append(QStringLiteral("反击结算"));
    if (!actionTargets.isEmpty()) results.append(QStringLiteral("%1 个目标").arg(actionTargets.size()));
    QSet<QString> changedUnits;
    for (const auto& e : shownEffects) if (e.changed) changedUnits.insert(e.target);
    if (!changedUnits.isEmpty()) results.append(QStringLiteral("%1 个单位发生变化").arg(changedUnits.size()));
    if (totalAdded || totalRemoved) results.append(QStringLiteral("Buff 获得 %1 条 / 移除 %2 条").arg(totalAdded).arg(totalRemoved));
    if (changedUnits.isEmpty()) results.append(phase ?
      (phase->kind == QStringLiteral("prepare") ? QStringLiteral("当前行动者已突出，数值保持出手前状态") :
       phase->kind == QStringLiteral("cast") || phase->kind == QStringLiteral("counter-cast") ? QStringLiteral("箭头指向本次目标，命中时显示结果") :
       phase->kind == QStringLiteral("finish") ? QStringLiteral("本次行动完成") : QStringLiteral("本阶段没有记录数值变化")) :
      moment_ < 0 ? QStringLiteral("点击单位查看当前 Buff；右侧保留全部作用记录。") : QStringLiteral("该小步没有已确认的数值变化；目标标记可在“包括无变化记录”中查看。"));
    outcome_->setText(results.join(QStringLiteral(" · ")));
    outcome_->setToolTip(QStringLiteral("条内数字为当前已记录余额；浮字和虚色标出本指标最近一段变化，点击对应条子可看此前记录。Buff 条数统计获得/移除记录，包含同一步先加后删；并不表示 Buff 层数或技能施加来源。"));
  }
  renderEffects();
  renderOverview();
  QWidget *sourceWidget = nullptr, *targetWidget = nullptr;
  castingAnchor_ = nullptr;
  QMap<QString, QWidget*> boardUnits;
  auto teamIds = replay_.teams.keys(); const int own = side_->currentData().toInt(); teamIds.removeAll(own); teamIds.prepend(own);
  for (int side = 0; side < 2; ++side) {
    const int teamId = teamIds.value(side, -1);
    const auto previousPlayer = players_[side]->currentData().toString();
    { QSignalBlocker blocker(players_[side]); players_[side]->clear();
      for (const auto& f : visible) if (f.team == teamId && f.id == QStringLiteral("@team"))
        players_[side]->addItem(f.player + (f.playerRemoved ? QStringLiteral(" · 已移除") : QString()), f.player);
      int preferred = players_[side]->findData(previousPlayer);
      if (followPlayers_) for (const auto& pn : step.participants) {
        const auto index = players_[side]->findData(pn); if (index >= 0) { preferred = index; break; }
      }
      if (focus && followPlayers_) for (const auto& key : {source, target}) {
        const auto participant = visible.value(key);
        if (participant.team == teamId) { const int index = players_[side]->findData(participant.player); if (index >= 0) preferred = index; }
      }
      players_[side]->setCurrentIndex(preferred >= 0 ? preferred : 0);
    }
    const auto player = players_[side]->currentData().toString();
    const auto owner = visible.value(fighterKey(teamId, player, QStringLiteral("@team")));
    auto dragonProgress = DragonCommandProgress::Complete;
    if (phase) dragonProgress = phase->kind == QStringLiteral("prepare") || phase->kind == QStringLiteral("setup") ? DragonCommandProgress::Before :
      phase->kind == QStringLiteral("finish") ? DragonCommandProgress::Complete : DragonCommandProgress::Active;
    else if (moment_ >= 0) dragonProgress = step.moments[moment_].phase == QStringLiteral("setup") ? DragonCommandProgress::Before :
      moment_ + 1 == step.moments.size() ? DragonCommandProgress::Complete : DragonCommandProgress::Active;
    const auto roster = dragonRosterAt(replay_, owner, current_, dragonProgress, catalog_);
    QStringList dragonHtml, dragonDetails, dragonStates, dragonPublicDetails;
    QString dragonPublicHtml = QStringLiteral("<h4>小神龙</h4>");
    QString dragonDetailHtml = QStringLiteral("<h3>小神龙 · %1</h3><p>状态按官方播放队列更新；只显示已记录的出手，不推算下一次行动。</p>").arg(esc(player));
    for (const auto& dragon : roster.dragons) {
      const auto state = dragonStateText(dragon.state);
      const auto dragonName = dragon.name == QStringLiteral("小神龙 [%1]").arg(dragon.id) ? QStringLiteral("小神龙名称待确认") : dragon.name;
      const bool active = dragon.state == DragonState::Fighting;
      const auto color = active ? QStringLiteral("#b56520") : dragon.state == DragonState::Finished ? QStringLiteral("#71817c") : QStringLiteral("#326c65");
      const auto label = dragonName +
        (dragon.glittery.value_or(false) ? QStringLiteral("★") : QString()) + QStringLiteral(" · ") + state;
      dragonHtml.append(QStringLiteral("<a href='dragon:%1' style='color:%2;text-decoration:none;%3'>%4</a>").arg(dragon.slot).arg(color, active ? QStringLiteral("font-weight:700") : QString(), esc(label)));
      QString detail = dragonName + QStringLiteral(" · ") + state + QStringLiteral("\n品质：") + (dragon.qualityName.isEmpty() ? QStringLiteral("未记录") : dragon.qualityName) +
        QStringLiteral("\n闪光：") + (!dragon.glittery ? QStringLiteral("未确认") : *dragon.glittery ? QStringLiteral("是") : QStringLiteral("否"));
      if (!dragon.description.isEmpty()) detail += QStringLiteral("\n技能：") + dragon.description;
      dragonPublicDetails.append(detail);
      dragonPublicHtml += QStringLiteral("<p>%1</p>").arg(esc(detail).replace(QLatin1Char('\n'), QStringLiteral("<br>")));
      detail += QStringLiteral("\n实例 ID：%1 · 定义 ID：%2\n能力值：%3 · 执行 Buff：%4\n基因：%5")
        .arg(dragon.id.isEmpty() ? QStringLiteral("未记录") : dragon.id, dragon.defineId.isEmpty() ? QStringLiteral("未记录") : dragon.defineId,
          number(dragon.skillValue), dragon.executeBufferId.isEmpty() ? QStringLiteral("未记录") : dragon.executeBufferId,
          dragon.geneRecorded ? (dragon.gene.isEmpty() ? QStringLiteral("空字符串") : dragon.gene) : QStringLiteral("未记录"));
      if (!dragon.lastSkillId.isEmpty()) detail += QStringLiteral("\n最近播放技能：") + replayPublicReferenceName(catalog_, QStringLiteral("skills"), dragon.lastSkillId);
      detail += QStringLiteral("\n记录：") + dragon.path;
      if (!dragon.lastActionPath.isEmpty()) detail += QStringLiteral("\n出手记录：") + dragon.lastActionPath;
      if (!dragon.lastCompletionPath.isEmpty()) detail += QStringLiteral("\n队列完成记录：") + dragon.lastCompletionPath;
      if (!dragon.issues.isEmpty()) detail += QLatin1Char('\n') + dragon.issues.join(QLatin1Char('\n'));
      dragonDetailHtml += QStringLiteral("<p>%1</p><h4>官方定义来源</h4><pre>%2</pre><h4>本场原始资料</h4><pre>%3</pre>")
        .arg(esc(detail).replace(QLatin1Char('\n'), QStringLiteral("<br>")), esc(pretty(dragon.definitionSource)), esc(pretty(dragon.raw)));
      dragonDetails.append(detail); dragonStates.append(dragon.id + QLatin1Char(':') + dragonStateKey(dragon.state));
    }
    if (!roster.issues.isEmpty()) dragonDetails.append(roster.issues.join(QLatin1Char('\n')));
    if (!roster.issues.isEmpty()) dragonDetailHtml += QStringLiteral("<p>%1</p>").arg(esc(roster.issues.join(QLatin1Char('\n'))).replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    dragonBars_[side]->setProperty("dragonDetailHtml", dragonDetailHtml);
    dragonBars_[side]->setProperty("dragonPublicHtml", roster.dragons.isEmpty() ? QString() : dragonPublicHtml);
    dragonBars_[side]->setVisible(!roster.dragons.isEmpty() || !roster.issues.isEmpty());
    dragonBars_[side]->setText(QStringLiteral("<b>小神龙</b>　") + (dragonHtml.isEmpty() ? QStringLiteral("资料待核实") : dragonHtml.join(QStringLiteral("　"))));
    dragonBars_[side]->setProperty("rawEvidence", dragonDetails.join(QStringLiteral("\n\n")));
    dragonBars_[side]->setToolTip(dragonPublicDetails.join(QStringLiteral("\n\n")));
    dragonBars_[side]->setAccessibleName(QStringLiteral("小神龙：\n") + dragonPublicDetails.join(QStringLiteral("\n\n")));
    dragonBars_[side]->setProperty("ownerKey", owner.key); dragonBars_[side]->setProperty("dragonStates", dragonStates);
    dragonBars_[side]->setProperty("activeDragonId", roster.activeId);
    const bool teamSourceActive = current_ > 0 && !owner.key.isEmpty() && activeActor == owner.key && rawAction.contains(QStringLiteral("epbi"));
    auto* sourceCard = static_cast<TeamSourceCard*>(teamSources_[side]);
    sourceCard->setVisible(teamSourceActive);
    sourceCard->setProperty("teamId", teamId);
    if (teamSourceActive) {
      const auto identity = teamSourceIdentity(step, owner, roster, catalog_);
      sourceCard->display(identity.kind, identity.name, owner.key, identity.instance, step.playbackSkillId, identity.detail, phase && animations_->isChecked());
      sourceCard->setProgress(phase ? playbackProgress_ : 1);
    } else {
      sourceCard->display({}, {}, {}, {}, {}, {}, false); sourceCard->setProgress(1);
    }
    const QString playerRound = step.playerRounds.contains(teamId) ? QStringLiteral(" · 第 %1 轮").arg(step.playerRounds.value(teamId)) : QString();
    teamLabels_[side]->setText(QStringLiteral("%1%2%3").arg(side == 0 ? QStringLiteral("我方") : QStringLiteral("敌方"), playerRound, owner.playerRemoved ? QStringLiteral(" · 已移除") : QString()));
    teamLabels_[side]->setToolTip(side == 0 ? QStringLiteral("我方阵容") : QStringLiteral("敌方阵容"));
    const QString queuePlayer = step.actionQueuePlayers.value(teamId);
    QStringList queueNames, queueHtml;
    for (const auto& id : step.actionQueues.value(teamId)) {
      const auto key = fighterKey(teamId, queuePlayer, id);
      const QString name = visible.contains(key) ? replayFighterName(visible.value(key), catalog_) : QStringLiteral("精灵身份待确认");
      queueNames.append(name);
      queueHtml.append(key == activeActor ? QStringLiteral("<b style='color:%1'>行动 · %2</b>").arg(skillColor, esc(name)) : esc(name));
    }
    const QString queueFallback = step.knownActionQueues.contains(teamId) ? QStringLiteral("空") : QStringLiteral("未记录");
    QString queueStatus;
    if (actorKnown && activeActor == step.actor && visible.value(activeActor).team == teamId && step.fromNormalQueue.has_value())
      queueStatus = *step.fromNormalQueue ? QStringLiteral(" · 本次从队列出手") : QStringLiteral(" · 本次为队列外行动");
    if (!queuePlayer.isEmpty() && queuePlayer != player) queueStatus.prepend(QStringLiteral(" · %1 的队列").arg(queuePlayer));
    // Keep the board compact without hiding the complete order or turning
    // inferred next turns into facts. Compare only this BattleUnit's records.
    const auto queueIds = step.actionQueues.value(teamId);
    const bool queueKnown = step.knownActionQueues.contains(teamId);
    const QString unitPath = step.path.section(QStringLiteral("/rl/"), 0, 0);
    QString queuePath, changeText;
    const QString queueField = teamId == 0 ? QStringLiteral("seqa") : QStringLiteral("seqb");
    for (int n = current_; n >= 0; --n) {
      const auto& candidate = replay_.steps[n];
      if (candidate.path.section(QStringLiteral("/rl/"), 0, 0) != unitPath) break;
      if (candidate.raw.toObject().contains(queueField)) { queuePath = candidate.path + QLatin1Char('/') + queueField; break; }
    }
    if (queueKnown && current_ > 0) {
      const auto& previous = replay_.steps[current_ - 1];
      if (previous.path.section(QStringLiteral("/rl/"), 0, 0) == unitPath && previous.knownActionQueues.contains(teamId) &&
          previous.actionQueuePlayers.value(teamId) == queuePlayer && previous.actionQueues.value(teamId) != queueIds) {
        QStringList removed = previous.actionQueues.value(teamId), added = queueIds;
        for (const auto& id : queueIds) if (removed.removeOne(id)) added.removeOne(id);
        const auto names = [&](const QStringList& ids) {
          QStringList result;
          for (const auto& id : ids) { const auto key = fighterKey(teamId, queuePlayer, id);
            result.append(visible.contains(key) ? replayFighterName(visible.value(key), catalog_) : QStringLiteral("精灵身份待确认")); }
          return result.join(QStringLiteral("、"));
        };
        if (!added.isEmpty()) changeText += QStringLiteral("加入：") + names(added);
        if (!removed.isEmpty()) changeText += (changeText.isEmpty() ? QString() : QStringLiteral("；")) + QStringLiteral("移出：") + names(removed);
        if (added.isEmpty() && removed.isEmpty()) changeText = QStringLiteral("记录顺序改变");
      }
    }
    QString queueDetail = QStringLiteral("<h4>已记录的行动队列%1</h4><p>%2</p>")
      .arg(esc(queueStatus), queueHtml.isEmpty() ? queueFallback : queueHtml.join(QStringLiteral(" → ")));
    if (!changeText.isEmpty()) queueDetail += QStringLiteral("<p><b>与前一条记录相比：</b>%1</p>").arg(esc(changeText));
    QStringList completed, restricted;
    const bool currentComplete = phase ? phase->kind == QStringLiteral("finish") : moment_ < 0 || moment_ + 1 == step.moments.size();
    if (step.playerRounds.contains(teamId)) for (int n = 0; n <= current_; ++n) {
      const auto& candidate = replay_.steps[n];
      if (!candidate.playerRounds.contains(teamId) || candidate.playerRounds.value(teamId) != step.playerRounds.value(teamId)) continue;
      if (n == current_ && !currentComplete) continue;
      const auto beforeAction = candidate.moments.isEmpty() ? candidate.state : candidate.moments.first().state;
      const auto actor = beforeAction.value(candidate.actor);
      if (!actor.key.isEmpty() && !actor.representer && actor.team == teamId && actor.player == player) {
        if (candidate.playbackKind == QStringLiteral("disabled") || candidate.playbackKind == QStringLiteral("unresolved"))
          restricted.append(esc(replayFighterName(actor, catalog_)) + (candidate.playbackKind == QStringLiteral("disabled") ? QStringLiteral("（行动受限）") : QStringLiteral("（执行待核实）")));
        else completed.append(esc(replayFighterName(actor, catalog_)) + (candidate.fromNormalQueue && !*candidate.fromNormalQueue ? QStringLiteral("（队列外）") : QString()));
      }
      for (const auto& moment : candidate.moments) if (moment.phase == QStringLiteral("counter")) {
        const auto counter = moment.state.value(moment.actor);
        if (!counter.key.isEmpty() && !counter.representer && counter.team == teamId && counter.player == player)
          completed.append(esc(replayFighterName(counter, catalog_)) + QStringLiteral("（反击）"));
      }
    }
    queueDetail += QStringLiteral("<h4>本方本轮已播放出手</h4><p>%1</p><p>队列只显示本战斗段最近记录；已播放出手仅取当前记录及之前，不预测下一次行动，也不把移出队列解释成死亡。</p>")
      .arg(completed.isEmpty() ? (step.playerRounds.contains(teamId) ? QStringLiteral("尚无已播放出手") : QStringLiteral("本方轮次未记录")) : completed.join(QStringLiteral(" → ")));
    if (!restricted.isEmpty()) queueDetail += QStringLiteral("<h4>本轮受限或未确认执行</h4><p>%1</p>").arg(restricted.join(QStringLiteral(" → ")));
    const auto preview = queueHtml.isEmpty() ? queueFallback : queueHtml.first() + (queueHtml.size() > 1 ? QStringLiteral(" → …") : QString());
    queues_[side]->setText(QStringLiteral("行动队列：%1　<a href='queue:all'>%2</a>%3").arg(preview,
      queueKnown ? QStringLiteral("全部 %1 项").arg(queueIds.size()) : QStringLiteral("查看依据"), changeText.isEmpty() ? QString() : QStringLiteral(" · 有变化")));
    queueDetail += QStringLiteral("<p><a href='team-raw:%1'>查看原始详情</a></p>").arg(side);
    queues_[side]->setProperty("queueDetailHtml", queueDetail); queues_[side]->setProperty("queuePath", queuePath);
    queues_[side]->setProperty("queueChangeText", changeText); queues_[side]->setProperty("completedActions", completed);
    queues_[side]->setAccessibleName(QStringLiteral("行动队列：") + (queueNames.isEmpty() ? queueFallback : queueNames.join(QStringLiteral(" → "))) + queueStatus);
    queues_[side]->setToolTip(queues_[side]->accessibleName() + QStringLiteral("\n按本战斗段最近一次记录的顺序；没有推算未记录的下次行动。"));
    queues_[side]->setProperty("teamId", teamId); queues_[side]->setProperty("queueKnown", step.knownActionQueues.contains(teamId));
    queues_[side]->setProperty("playerRound", step.playerRounds.contains(teamId) ? QVariant(step.playerRounds.value(teamId)) : QVariant());
    QString progressId = valueText(owner.raw[QStringLiteral("ppbf")].toObject()[QStringLiteral("pbid")]);
    if (!owner.teamProgress.contains(progressId)) progressId = owner.teamProgress.isEmpty() ? QString() : owner.teamProgress.firstKey();
    // Show the progress touched in this phase, preserving every independent
    // balance in teamProgress instead of replacing it with the last t=4 unit.
    for (const auto& e : shownEffects) if (e.kind == QStringLiteral("progress") && e.target == owner.key) progressId = e.causeId;
    const auto progress = owner.teamProgress.value(progressId);
    std::optional<qint64> progressBefore, progressDelta; QString progressStatus, progressKind, progressEvidence;
    QStringList progressRecords;
    QList<ReplaySpiritTransition> progressTransitions;
    QStringList progressKinds;
    const auto progressExecution = catalog_[QStringLiteral("bufferExecution")].toObject()[progressId].toObject();
    const auto configuredMax = recordedIntegerId(progressExecution[QStringLiteral("maxValue")]).toLongLong();
    for (const auto& e : shownEffects) if (e.kind == QStringLiteral("progress") && e.target == owner.key && e.causeId == progressId) {
      progressBefore = e.before; progressDelta = e.delta;
      const auto rawProgress = e.raw.toObject();
      const auto skillEffect = rawProgress[QStringLiteral("isse")];
      // SkillNumUnitBuffChangedState.as defaults an absent isse to false.
      // Explicit null or malformed values remain unconfirmed here.
      const bool noSkillEffect = !rawProgress.contains(QStringLiteral("isse")) || (skillEffect.isBool() && !skillEffect.toBool());
      progressKind = QStringLiteral("neutral"); progressStatus = QStringLiteral("通灵变化");
      if (e.delta && *e.delta > 0) { progressKind = QStringLiteral("charge"); progressStatus = QStringLiteral("通灵充能"); }
      else if (skillEffect.isBool() && skillEffect.toBool()) { progressKind = QStringLiteral("release"); progressStatus = QStringLiteral("通灵技能生效"); }
      else if (e.delta && *e.delta < 0 && configuredMax > 0 && e.before && e.after && *e.after < configuredMax &&
               !(*e.before >= configuredMax && *e.after == 0) && noSkillEffect) {
        progressKind = QStringLiteral("drain"); progressStatus = QStringLiteral("通灵扣除");
      } else if (e.delta && *e.delta < 0) {
        progressStatus = configuredMax > 0 && e.before && e.after && *e.before >= configuredMax && *e.after == 0
          ? QStringLiteral("满槽归零") : QStringLiteral("通灵减少");
      }
      progressKinds.append(progressKind);
      progressEvidence += QStringLiteral("%1：%2\n%3\n").arg(e.path, progressStatus, pretty(e.raw));
      progressTransitions.append({e.before, e.after, e.delta, progressKind, progressStatus, e.path + QLatin1Char('\n') + pretty(e.raw)});
      progressRecords.append(effectSummary(e));
    }
    if (step.playbackKind == QStringLiteral("team-skill") && valueText(rawAction[QStringLiteral("epbi")]) == progressId && activeActor == owner.key && phase &&
        (phase->kind == QStringLiteral("cast") || phase->kind == QStringLiteral("attack"))) {
      progressKind = QStringLiteral("release"); progressStatus = QStringLiteral("通灵技能施放");
      progressEvidence += QStringLiteral("当前队伍施法执行 Buff 与进度 Buff 一致：%1/epbi = %2；不根据余额猜测施法。\n").arg(step.path, progressId);
    }
    spirits_[side]->setProgress(progress.current, progress.maximum,
      progressId.isEmpty() ? QString() : replayPublicReferenceName(catalog_, QStringLiteral("buffs"), progressId),
      QStringLiteral("分格与上限按本场记录显示。\n") + progressRecords.join(QLatin1Char('\n')));
    spirits_[side]->setProperty("progressBuffId", progressId);
    spirits_[side]->setProperty("progressRaw", pretty(progress.raw));
    spirits_[side]->setTransition(progressBefore, progressDelta, progressStatus, phase && animations_->isChecked());
    spirits_[side]->setTransitionKind(progressKind, progressEvidence);
    spirits_[side]->setRecordedTransitions(progressTransitions);
    spirits_[side]->setProperty("transitionKinds", progressKinds);
    QStringList teamHtml;
    const QString hero = valueText(owner.raw[QStringLiteral("hs")]);
    if (hero.toLongLong() > 0) teamHtml.append(QStringLiteral("英雄技：") + referenceLink(catalog_, QStringLiteral("skills"), hero));
    QMap<QString, BuffMember> memberships; QMap<QString, int> counts;
    for (const auto& member : recordedBuffMembers(owner)) {
      const QString key = member.id + QLatin1Char('|') + member.source + QLatin1Char('|') + QString::number(member.sourceTeam);
      memberships.insert(key, member); ++counts[key];
    }
    QStringList buffHtml, unknownBuffHtml, playbackNotes;
    QSet<QString> playbackNoteIds;
    int unknownMembers = 0;
    for (auto it = memberships.begin(); it != memberships.end(); ++it) {
      const auto& member = it.value();
      const bool known = replayBuffSourceSide(member.source, member.sourceTeam, own) != ReplayBuffSourceSide::Unknown;
      const QString color = !known ? QStringLiteral("#65717e") : member.sourceTeam == own ? QStringLiteral("#246bb0") : QStringLiteral("#b4425c");
      const QString origin = !known ? QStringLiteral("来源未记") : member.sourceTeam == own ? QStringLiteral("我方施加") : QStringLiteral("敌方施加");
      const auto text = QStringLiteral("%1 · 记录 %2 条").arg(replayPublicReferenceName(catalog_, QStringLiteral("buffs"), member.id)).arg(counts.value(it.key()));
      const auto row = QStringLiteral("<a href='buff:%1' style='color:%2;text-decoration:none' title='%3'>%4</a>")
        .arg(esc(member.id), color, esc(origin + (member.sourceName.isEmpty() ? QString() : QStringLiteral(" · ") + member.sourceName)), esc(text));
      const auto definition = catalog_[QStringLiteral("buffs")].toObject()[member.id].toObject();
      const bool explained = replayBuffHasExplanation(definition);
      (explained ? buffHtml : unknownBuffHtml).append(row);
      if (!explained) unknownMembers += counts.value(it.key());
      const auto presentation = replayBuffPublicPresentation(definition);
      if (!presentation.labels.isEmpty() && !playbackNoteIds.contains(member.id)) {
        playbackNoteIds.insert(member.id);
        playbackNotes.append(QStringLiteral("%1：%2").arg(esc(replayPublicReferenceName(catalog_, QStringLiteral("buffs"), member.id)), esc(presentation.labels.join(QStringLiteral(" · ")))));
      }
    }
    teamHtml.append(QStringLiteral("队伍 Buff：") + (buffHtml.isEmpty() ? (unknownMembers ? QStringLiteral("有未解释登记") : owner.buffsKnown ? QStringLiteral("无") : QStringLiteral("未记录")) : buffHtml.join(QStringLiteral("　"))));
    if (unknownMembers) teamHtml.append(QStringLiteral("<a href='team-effects:all'>未解释记录 %1 条</a>").arg(unknownMembers));
    for (const auto& p : owner.teamProgress) if (p.buffId != progressId)
      teamHtml.append(esc(replayPublicReferenceName(catalog_, QStringLiteral("buffs"), p.buffId)) + QStringLiteral(" %1/%2").arg(number(p.current), number(p.maximum)));
    QStringList teamChanges, unknownTeamChanges;
    for (const auto& e : shownEffects) if (e.target == owner.key && meaningfulEffect(e)) {
      const bool unknown = e.kind == QStringLiteral("buff") && !e.restoresDisplayLayers &&
        !replayBuffHasExplanation(catalog_[QStringLiteral("buffs")].toObject()[e.causeId].toObject());
      (unknown ? unknownTeamChanges : teamChanges).append(esc(e.restoresDisplayLayers
        ? QStringLiteral("恢复显示") : effectSummary(e)));
    }
    if (!teamChanges.isEmpty()) teamHtml.prepend(QStringLiteral("<b style='color:#a56922'>%1</b>").arg(teamChanges.join(QStringLiteral(" · "))));
    if (!overview.value(owner.key).badge.isEmpty()) teamHtml.prepend(QStringLiteral("<b style='color:#a34745'>%1</b>").arg(esc(overview.value(owner.key).badge)));
    teamBuffs_[side]->setProperty("ownerKey", owner.key);
    const auto visibleTeamHtml = teamHtml.join(QStringLiteral("<br>"));
    QString unknownTeamHtml;
    if (!unknownBuffHtml.isEmpty() || !unknownTeamChanges.isEmpty())
      unknownTeamHtml = QStringLiteral("<h4>未解释记录</h4><p>保留登记与变动；具体效果未解释，不按名称判断为纯动画或推算层数。</p>") +
        unknownTeamChanges.join(QStringLiteral("<br>")) + QStringLiteral("<br>") + unknownBuffHtml.join(QStringLiteral("<br>"));
    const auto playbackTeamHtml = playbackNotes.isEmpty() ? QString() : QStringLiteral("<h4>播放详情</h4><p>以下只说明播放设置，不决定 Buff 的其他用途。</p>") + playbackNotes.join(QStringLiteral("<br>"));
    const auto completeTeamHtml = visibleTeamHtml + unknownTeamHtml + playbackTeamHtml;
    teamBuffs_[side]->setProperty("completeHtml", QStringLiteral("<h3>队伍效果 · %1</h3>%2").arg(esc(player), completeTeamHtml));
    teamBuffs_[side]->setHtml(QStringLiteral("<a href='team-effects:all'>展开全部队伍效果%1</a><br>")
      .arg(unknownTeamHtml.isEmpty() && playbackTeamHtml.isEmpty() ? QString() : QStringLiteral("（含折叠记录）")) + visibleTeamHtml);
    teamBuffs_[side]->setAccessibleName(teamBuffs_[side]->toPlainText());
    if (source == owner.key) sourceWidget = teamSourceActive ? teamSources_[side] : spirits_[side];
    if (target == owner.key) targetWidget = spirits_[side];
    boardUnits.insert(owner.key, teamSourceActive ? teamSources_[side] : spirits_[side]);
    if (activeActor == owner.key) castingAnchor_ = cells_[side * 9];
    for (int cell = 0; cell < 9; ++cell) {
      // Official f-index is x + y*3. x runs top-to-bottom, y front-to-back.
      // The left locator's front is on the right; the right locator faces it.
      // Use the displayed side, not the original team ID, when switching views.
      const int x = cell / 3;
      const int y = side == 0 ? 2 - cell % 3 : cell % 3;
      const int slot = x + y * 3 + 1;
      cells_[side * 9 + cell]->setToolTip(side == 0 ? QStringLiteral("我方阵容") : QStringLiteral("敌方阵容"));
      QList<Fighter> occupants; QSet<QString> changed;
      for (auto it = visible.begin(); it != visible.end(); ++it) {
        const auto& f = it.value(); if (f.representer || f.team != teamId || f.player != player || f.x != x || f.y != y) continue;
        const auto old = before.value(f.key);
        if (f.hp != old.hp || f.power != old.power || f.shield != old.shield || f.buffs != old.buffs || f.raceId != old.raceId || f.attributes != old.attributes || f.presence != old.presence) changed.insert(f.key);
        occupants.append(f);
      }
      auto* view = cells_[side * 9 + cell];
      view->display(slot, occupants, catalog_, activeActor, changed, source, target, focus ? effectSummary(*focus) : QString(), empowered_, overview, own);
      for (const auto& f : occupants) {
        if (f.key == activeActor) castingAnchor_ = view;
        auto* anchor = view->relationAnchor(f.key);
        boardUnits.insert(f.key, anchor);
        if (f.key == source) sourceWidget = anchor;
        if (f.key == target) { targetWidget = anchor; view->focusUnit(target); }
        if (empowermentNotes_.contains(f.key)) view->setProperty("empowermentEvidence", empowermentNotes_[f.key]);
      }
    }
    QStringList extras;
    for (const auto& f : visible) {
      if (f.team != teamId || f.player != player) continue;
      if (!f.representer && (f.x < 0 || f.x > 2 || f.y < 0 || f.y > 2)) extras.append(link(f, catalog_) + QStringLiteral(" · 阵位待核实"));
    }
    teamEffects_[side]->setText(extras.join(QStringLiteral("<br>")));
    teamEffects_[side]->setVisible(!extras.isEmpty());
  }
  QList<ReplayRelation> relations;
  const auto addRelation = [&](const QString& sourceKey, const QString& targetKey, QWidget* from, QWidget* to) {
    if (!to) return;
    if (std::any_of(relations.cbegin(), relations.cend(), [from, to](const ReplayRelation& r) { return r.source == from && r.target == to; })) return;
    ReplayRelation relation; relation.source = from; relation.target = to;
    relation.sourceKey = from ? sourceKey : QString(); relation.targetKey = targetKey;
    relation.sourceTeam = from && visible.contains(sourceKey) ? visible.value(sourceKey).team : -1;
    relation.targetTeam = visible.contains(targetKey) ? visible.value(targetKey).team : -1;
    if (from) for (const auto& cue : relationCues) {
      if (cue.source != sourceKey || cue.target != targetKey) continue;
      const auto attribute = catalog_[QStringLiteral("attributes")].toObject()[cue.attributeId].toObject()[QStringLiteral("nm")].toString();
      const auto label = cue.kind == QStringLiteral("strong") && !attribute.isEmpty() ? attribute + QStringLiteral("强效") : cue.label;
      if (!relation.labels.contains(label)) relation.labels.append(label);
      if (!relation.detail.isEmpty()) relation.detail += QStringLiteral("\n\n");
      relation.detail += label + QLatin1Char('\n') + cue.explanation + QStringLiteral("\n依据：") + cue.evidence.join(QStringLiteral("；"));
    }
    relations.append(relation);
  };
  if (focus) addRelation(source, target, !canAttack && focus->kind == QStringLiteral("target") ? nullptr : sourceWidget, targetWidget);
  else for (const auto& key : actionTargets) addRelation(activeActor, key, boardUnits.value(activeActor), boardUnits.value(key));
  if (phase) for (const auto& e : shownEffects) if (e.changed && !actionTargets.contains(e.target) && e.source != e.target) {
    addRelation(e.source, e.target, e.source.isEmpty() ? nullptr : boardUnits.value(e.source), boardUnits.value(e.target));
  }
  if (phase) for (auto it = overview.begin(); it != overview.end(); ++it) if (it.value().target) {
    auto* receiver = boardUnits.value(it.key());
    if (receiver && std::none_of(relations.begin(), relations.end(), [receiver](const ReplayRelation& relation) { return relation.target == receiver; }))
      addRelation({}, it.key(), nullptr, receiver);
  }
  int initialRelations = 0;
  if (showInitialOrigins) {
    const auto initialRelation = [&](const EmpowermentEvidence& evidence, bool self) {
      auto* from = boardUnits.value(evidence.source); auto* to = boardUnits.value(evidence.target);
      if (!from || !to) return;
      addRelation(evidence.source, evidence.target, from, to);
      for (auto& relation : relations) if (relation.source == from && relation.target == to) {
        relation.labels.append(self ? QStringLiteral("开局幻元") : QStringLiteral("开局元能"));
        relation.detail += QStringLiteral("\n原生属性 → 开局属性的唯一核对，非战斗中施法。\n") + evidence.description;
        ++initialRelations; break;
      }
    };
    for (const auto& evidence : empowermentEvidence_) initialRelation(evidence, false);
    for (const auto& evidence : selfTransformEvidence_) initialRelation(evidence, true);
  }
  links_->setProperty("initialRelationCount", initialRelations);
  links_->setRelations(relations, replay_.sha256 + QStringLiteral("/%1/%2/%3/%4/%5").arg(current_).arg(moment_).arg(effect_).arg(own).arg(playbackPhase_), animations_->isChecked() && !showInitialOrigins);
  const auto playbackConfig = catalog_[step.lastSkillStatus == QStringLiteral("applied") && !counterStage
    ? QStringLiteral("skillLastEffects") : QStringLiteral("skills")].toObject()[skillId].toObject();
  const auto attackType = playbackConfig[QStringLiteral("attackType")].toString();
  links_->setMotion(step.playbackKind == QStringLiteral("summon") ? QStringLiteral("summon") :
    attackType == QStringLiteral("近身") ? QStringLiteral("close") : attackType == QStringLiteral("远程") ? QStringLiteral("remote") : QString());
  QList<ReplayMeterChange> meterChanges;
  for (const auto* cell : cells_) meterChanges.append(cell->floatingMeterChanges());
  meterOverlay_->setChanges(meterChanges);
  if (castingPanel_->isVisible()) castingPanel_->raise();
  if (phase) updatePlaybackProgress(playbackProgress_);
  followPlayers_ = false;
  QString html = QStringLiteral("<h3>%1</h3>").arg(esc(step.title));
  if (evidence_->isChecked() || step.schedule.contains(QStringLiteral("待核实"))) html += QStringLiteral("<p style='color:#68788a'>%1</p>").arg(esc(step.schedule));
  if (!step.actor.isEmpty()) {
    const auto action = step.raw.toObject(); QStringList targets;
    for (const auto& list : {QStringLiteral("trl"), QStringLiteral("prl")}) for (const auto& value : action[list].toArray()) {
      const auto target = value.toObject();
      const auto key = fighterKey(target[QStringLiteral("tti")].toInt(-1), valueText(target[QStringLiteral("tpn")]),
        targetFighterId(target[QStringLiteral("tci")]));
      if (visible.contains(key)) targets.append(link(visible[key], catalog_));
    }
    targets.removeDuplicates();
    html += QStringLiteral("<p><b>行动者：</b>%1<br><b>记录涉及：</b>%2</p>").arg(link(visible.value(step.actor), catalog_), targets.isEmpty() ? QStringLiteral("没有目标记录") : targets.join(QStringLiteral("、")));
    if (!step.keyChanges.isEmpty()) html += QStringLiteral("<p><b>关键变化：</b>%1</p>").arg(esc(step.keyChanges.join(QStringLiteral(" · "))));
  }
  if (moment_ >= 0) html += QStringLiteral("<h4>当前：%1</h4><p>格子显示已播到的记录余额；可确认顺序的数值列表逐段展示，作用列表保留各条记录的前后值。</p>").arg(esc(step.moments[moment_].title));
  QString changes;
  for (const auto& key : step.affected) {
    const auto f = visible.value(key), old = before.value(key);
    QStringList values;
    if (f.hp != old.hp) values.append(QStringLiteral("生命 %1 → %2").arg(number(old.hp), number(f.hp)));
    if (f.power != old.power) values.append(QStringLiteral("气势 %1 → %2").arg(number(old.power), number(f.power)));
    if (f.shield != old.shield) values.append(QStringLiteral("护盾 %1 → %2").arg(number(old.shield), number(f.shield)));
    if (f.buffs != old.buffs) values.append(QStringLiteral("Buff 记录 %1 → %2 条（详情见下方过程及单位详情）").arg(old.buffs.size()).arg(f.buffs.size()));
    if (f.raceId != old.raceId) values.append(QStringLiteral("形态 %1 → %2").arg(esc(replayPublicReferenceName(catalog_, QStringLiteral("pets"), old.raceId)), esc(replayPublicReferenceName(catalog_, QStringLiteral("pets"), f.raceId))));
    if (f.attributes != old.attributes) values.append(QStringLiteral("属性 %1 → %2").arg(esc(attributesText(old, catalog_)), esc(attributesText(f, catalog_))));
    if (f.maxPower != old.maxPower) values.append(QStringLiteral("满气势 %1 → %2").arg(number(old.maxPower), number(f.maxPower)));
    if (f.presence != old.presence) values.append(esc(f.presence));
    if (!values.isEmpty()) changes += QStringLiteral("<tr><td style='padding:4px 12px 4px 0'><b>%1</b></td><td>%2</td></tr>").arg(link(f, catalog_), values.join(QStringLiteral("；")));
  }
  if (!changes.isEmpty()) html += QStringLiteral("<h4>本条记录变化</h4><table>%1</table>").arg(changes);
  const auto action = step.raw.toObject();
  if (moment_ < 0 && action.contains(QStringLiteral("si"))) {
    const auto config = catalog_[QStringLiteral("skills")].toObject()[valueText(action[QStringLiteral("si")])].toObject();
    html += QStringLiteral("<p><b>官方技能说明：</b>%1</p>").arg(officialDescription(config[QStringLiteral("description")].toString(QStringLiteral("未收录，不推测说明"))));
  }
  for (const auto& phase : {QStringLiteral("行动"), QStringLiteral("目标"), QStringLiteral("战斗段")}) {
    if (moment_ >= 0) continue;
    bool heading = false;
    for (const auto& d : step.details) {
      if (d.phase != phase) continue;
      if (!heading) { html += QStringLiteral("<h4 style='color:#195e96'>%1</h4>").arg(phase); heading = true; }
      QString text = d.text;
      if (phase == QStringLiteral("目标")) {
        const auto flags = d.raw.toObject(); QStringList meaningful;
        if (flags.contains(QStringLiteral("ishit"))) meaningful.append(flags[QStringLiteral("ishit")].toBool() ? QStringLiteral("命中") : QStringLiteral("未命中"));
        for (const auto& flag : QList<QPair<QString, QString>>{{"isbl", "格挡"}, {"iscr", "暴击"}, {"issb", "反击标记"}, {"isIm", "免疫"}})
          if (flags[flag.first].toBool()) meaningful.append(flag.second);
        if (flags.contains(QStringLiteral("isse")) && !flags[QStringLiteral("isse")].toBool()) meaningful.append(QStringLiteral("技能生效标记为否；其他变化仍保留"));
        text = meaningful.isEmpty() ? QStringLiteral("目标记录；完整标记见原文") : meaningful.join(QStringLiteral(" · "));
      }
      html += QStringLiteral("<p><b>%1</b>：%2%3</p>").arg(esc(d.object), esc(text), evidence_->isChecked() ? QStringLiteral("<br><span style='font-size:10px;color:#7a8798'>%1</span>").arg(esc(d.path)) : QString());
    }
  }
  for (int i = 0; i < step.moments.size(); ++i) {
    if (moment_ >= 0 && moment_ != i) continue;
    const auto& frame = step.moments[i];
    html += QStringLiteral("<h4 style='color:#195e96'>%1. %2</h4>").arg(i + 1).arg(esc(frame.title));
    if (!frame.note.isEmpty() && (evidence_->isChecked() || frame.note.contains(QStringLiteral("待核实")))) html += QStringLiteral("<p style='color:#68788a'>%1</p>").arg(esc(frame.note));
    for (const auto& d : frame.details)
      html += QStringLiteral("<p><b>%1</b>：%2%3</p>").arg(esc(d.object), esc(d.text), evidence_->isChecked() ? QStringLiteral("<br><span style='font-size:10px;color:#7a8798'>%1</span>").arg(esc(d.path)) : QString());
  }
  if (step.details.isEmpty()) html += QStringLiteral("<p>完整内容见“本条原文”；点击九宫格中的名称查看单位。</p>");
  details_->setHtml(html); rawStep_->setPlainText(pretty(step.raw));
  fitBoardHeight();
  if (!selectedUnit_.isEmpty()) showUnit(selectedUnit_);
  else unit_->setHtml(QStringLiteral("<p>点击九宫格中的单位名称，查看当前 Buff、完整身份及数值来源。</p>"));
}
void BattleReplayPage::showUnit(const QString& key, bool raw) {
  if (replay_.steps.isEmpty() || !displayedState().contains(key)) return;
  selectedUnit_ = key; const auto& f = displayedState()[key];
  const auto encodedKey = QString::fromLatin1(QUrl::toPercentEncoding(key));
  if (!raw) {
    QString html = QStringLiteral("<h3>%1</h3><p>%2 · %3</p>")
      .arg(esc(replayFighterName(f, catalog_)), f.team == side_->currentData().toInt() ? QStringLiteral("我方") : QStringLiteral("敌方"), esc(f.presence));
    if (!f.representer) {
      const auto definitionRace=replayFighterPublicDefinitionRace(f,catalog_);
      html+=QStringLiteral("<p>%1").arg(PublicDataDetailLinks::anchor(
        {PublicDataViewKind::BattleReplay,QStringLiteral("pets"),definitionRace},
        definitionRace==f.raceId ? QStringLiteral("查看精灵官方资料") : QStringLiteral("查看已记录本体的资料")));
      if (definitionRace!=f.raceId) html+=QStringLiteral(" · ")+PublicDataDetailLinks::anchor(
        {PublicDataViewKind::BattleReplay,QStringLiteral("pets"),f.raceId},QStringLiteral("查看当前形态资料"));
      html+=QStringLiteral("</p>");
    }
    if (!f.representer) {
      html += QStringLiteral("<table width='100%' cellpadding='8'><tr><td bgcolor='#fff0f2'><b>生命</b><br>%1 / %2</td><td bgcolor='#edf5ff'><b>护盾</b><br>%3</td><td bgcolor='#fff5e7'><b>气势</b><br>%4 / %5</td></tr></table><p>属性：%6</p>")
        .arg(number(f.hp), number(f.maxHp), number(f.shield), number(f.power), number(f.maxPower), esc(attributesText(f, catalog_)));
    }
    for (const auto& entry : QList<QPair<QString, QString>>{{QStringLiteral("普通技能"), f.normalSkill}, {QStringLiteral("超杀技能"), f.uniqueSkill},
        {QStringLiteral("英雄技能"), f.representer ? valueText(f.raw[QStringLiteral("hs")]) : QString()}}) {
      if (entry.second.isEmpty() || entry.second == QStringLiteral("0")) continue;
      const auto description = catalog_[QStringLiteral("skills")].toObject()[entry.second].toObject()[QStringLiteral("description")].toString();
      html += QStringLiteral("<h4>%1 · %2</h4><p>%3</p>").arg(entry.first, referenceLink(catalog_, QStringLiteral("skills"), entry.second),
        description.isEmpty() ? QStringLiteral("暂无官方说明。") : officialDescription(description));
    }
    html += QStringLiteral("<h4>当前效果</h4>");
    if (!f.buffsKnown) html += QStringLiteral("<p>当前效果记录可能不完整。</p>");
    QMap<QString, int> counts;
    for (const auto& id : f.buffs) ++counts[id];
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
      const auto config = catalog_[QStringLiteral("buffs")].toObject()[it.key()].toObject();
      const auto description = replayBuffMessage(config[QStringLiteral("mess")].toString(), replayFighterName(f, catalog_));
      html += QStringLiteral("<p><b>%1</b>%2<br>%3</p>").arg(referenceLink(catalog_, QStringLiteral("buffs"), it.key()),
        it.value() > 1 ? QStringLiteral(" · %1 条记录").arg(it.value()) : QString(),
        description.isEmpty() ? (replayBuffStateCues(config).isEmpty() ? QStringLiteral("具体效果暂无说明。") : esc(replayBuffStateCues(config).join(QStringLiteral(" · ")))) : officialDescription(description));
    }
    if (counts.isEmpty()) html += QStringLiteral("<p>暂无已记录效果。</p>");
    QStringList history;
    for (const auto& id : ReplayBuffHistory::observedIds(replay_, key, current_, moment_))
      if (!counts.contains(id)) history << referenceLink(catalog_, QStringLiteral("buffs"), id);
    if (!history.isEmpty()) html += QStringLiteral("<h4>此前出现的效果</h4><p>%1</p>").arg(history.join(QStringLiteral("<br>")));
    html += QStringLiteral("<p style='color:#68788e'>条数是已记录次数，不表示效果层数。点击效果可查看来源和发生过程。</p><p><a href='unit-raw:%1'>查看原始详情</a></p>").arg(encodedKey);
    unit_->setHtml(html);
    if (sender() && (qobject_cast<QTextBrowser*>(sender()) || qobject_cast<QLabel*>(sender()) || qobject_cast<ReplayCell*>(sender()))) tabs_->setCurrentWidget(unit_);
    return;
  }
  QString html = QStringLiteral("<h3>%1</h3><p>队伍 %2 · 玩家 %3 · 单位 ID %4<br>%5</p>")
    .arg(esc(replayFighterName(f, catalog_))).arg(f.team).arg(esc(f.player), esc(f.id), esc(f.presence));
  if (!f.representer) html += QStringLiteral("<p>最近记录生命 %1 / %2，气势 %3，护盾 %4</p><p>生命来源：%5<br>气势来源：%6<br>护盾来源：%7</p>")
    .arg(number(f.hp), number(f.maxHp), number(f.power), number(f.shield), esc(f.hpPath), esc(f.powerPath), esc(f.shieldPath));
  html += ReplayInspection::unknownHtml(f, replay_.root, replay_.steps[current_]);
  html += ReplayInspection::recordedMetadata(f, catalog_);
  html += ReplayReference::recordedTitles(catalog_, f.raw[QStringLiteral("title")]);
  const auto displayEvidence = replayFighterNameEvidence(f, catalog_);
  if (!displayEvidence.isEmpty()) html += QStringLiteral("<p>%1</p>").arg(esc(displayEvidence).replace(QLatin1Char('\n'), QStringLiteral("<br>")));
  if (f.representer && valueText(f.raw[QStringLiteral("hs")]).toLongLong() > 0) {
    const auto hero = valueText(f.raw[QStringLiteral("hs")]);
    const auto config = catalog_[QStringLiteral("skills")].toObject()[hero].toObject();
    html += QStringLiteral("<h4>英雄技能</h4><p>%1<br>%2</p>").arg(referenceLink(catalog_, QStringLiteral("skills"), hero),
      officialDescription(config[QStringLiteral("description")].toString()));
  }
  if (!f.representer) {
    const auto pet = catalog_[QStringLiteral("pets")].toObject()[f.raceId].toObject();
    const auto identity = pet[QStringLiteral("identity")].toObject();
    html += QStringLiteral("<h4>精灵原名与外观</h4><p>原精灵：%1<br>当前形态原名：%2<br>日志名称：%3<br>自定义名称：%4</p>")
      .arg(esc(f.name), esc(fighterName(f)), esc(f.raw[QStringLiteral("name")].toString()), esc(f.raw[QStringLiteral("customName")].toString()));
    if (!f.originRaceId.isEmpty()) {
      const auto originLabel = f.originRelation == QStringLiteral("chaser") ? QStringLiteral("追击关联本体") : QStringLiteral("变身来源");
      html += QStringLiteral("<p><b>%1：%2</b><br>种族 ID：%3<br>关联依据：%4</p>")
        .arg(esc(originLabel), esc(petName(catalog_, f.originRaceId)), esc(f.originRaceId), esc(f.originPath));
    }
    if (identity[QStringLiteral("status")].toString() == QStringLiteral("skin"))
      html += QStringLiteral("<p>皮肤：%1<br>官方换肤对应：种族 %2 → 原精灵 %3<br>皮肤只用于名称归一；技能、属性仍读取战报中的种族与技能编号。</p>")
        .arg(esc(pet[QStringLiteral("name")].toString()), esc(f.raceId), esc(identity[QStringLiteral("baseRaceId")].toString()));
    else if (identity[QStringLiteral("status")].toString() == QStringLiteral("unresolved"))
      html += QStringLiteral("<p>该形态缺少明确的原名对应。官方记录名：%1；字典归属：%2。归属编号可能包含追击等派生形态，不能直接作为该单位原名。</p>")
        .arg(esc(pet[QStringLiteral("name")].toString()), esc(identity[QStringLiteral("groupName")].toString()));
    if (catalog_[QStringLiteral("petMechanics")].toObject().contains(f.raceId))
      html += QStringLiteral("<p><a href='mechanics:%1'>查看当前形态的官方机制资料：固有技能与气运／溯灵</a></p>").arg(esc(f.raceId));
    const auto powerFull = ReplayReference::powerFull(catalog_, f.faceId);
    if (!powerFull.isEmpty()) {
      const auto index = catalog_[QStringLiteral("replaySupplements")].toObject()[QStringLiteral("powerFull")].toObject()[QStringLiteral("byRaceId")].toObject()[f.faceId];
      html += QStringLiteral("<p>满气势专属表现：%1（按本场外观 [%2] 查询；不添加战报状态）</p>")
        .arg(index.isUndefined() ? referenceLink(catalog_, QStringLiteral("buffs"), ReplayReference::text(powerFull[QStringLiteral("buffId")])) :
          ReplayReference::link(QStringLiteral("powerFull"), ReplayReference::text(index), QStringLiteral("官方表现 Buff [%1]").arg(ReplayReference::text(powerFull[QStringLiteral("buffId")]))), esc(f.faceId));
    }
    html += QStringLiteral("<h4>当前形态与技能</h4><p>形态：%1；外观 ID：%2<br>属性：%3<br>满气势：%4（官方精灵字典）<br>普通技能：%5<br>当前超杀：%6<br>额外超杀字段：%7</p>")
      .arg(esc(replayPublicReferenceName(catalog_, QStringLiteral("pets"), f.raceId)), esc(f.faceId.isEmpty() ? QStringLiteral("待核实") : f.faceId), esc(attributesText(f, catalog_)), number(f.maxPower),
        esc(replayPublicReferenceName(catalog_, QStringLiteral("skills"), f.normalSkill)), esc(replayPublicReferenceName(catalog_, QStringLiteral("skills"), f.uniqueSkill)),
        f.superUniqueSkill.isEmpty() ? QStringLiteral("未记录") : esc(f.superUniqueSkill));
    html += QStringLiteral("<p>形态来源：%1<br>属性来源：%2<br>技能来源：%3<br>登场依据：%4<br>显示恢复依据：%5</p>").arg(esc(f.formPath), esc(f.attributesPath), esc(f.skillsPath), esc(f.presencePath), esc(f.displayLayersPath));
    html += QStringLiteral("<p>护盾显示当前余额；满气势是条形显示参考，不是数值封顶。</p>");
    if (empowermentNotes_.contains(key)) html += QStringLiteral("<p style='color:#a93679'><b>元能目标 · 交叉核对</b><br>%1</p>").arg(esc(empowermentNotes_[key]));
    if (selfTransformEvidence_.contains(key)) html += QStringLiteral("<p style='color:#7652a6'><b>开局自身幻元 · 唯一核对</b><br>%1</p>")
      .arg(esc(selfTransformEvidence_.value(key).description));
    for (const auto& id : {f.normalSkill, f.uniqueSkill}) {
      if (id.isEmpty()) continue;
      const auto config = catalog_[QStringLiteral("skills")].toObject()[id].toObject();
      html += QStringLiteral("<p><b>%1</b><br>%2</p>").arg(referenceLink(catalog_, QStringLiteral("skills"), id), officialDescription(config[QStringLiteral("description")].toString(QStringLiteral("未收录说明"))));
    }
  }
  html += QStringLiteral("<h4>当前 Buff%1</h4><p>重复次数是客户端成员记录数，不自动解释为技能层数；隐藏 Buff 同样保留。</p>").arg(f.buffsKnown ? QString() : QStringLiteral("（集合不完整，待核实）"));
  QMap<QString, int> counts; for (const auto& id : f.buffs) ++counts[id];
  if (counts.isEmpty()) html += f.buffsKnown ? QStringLiteral("<p>当前无 Buff 记录。</p>") : QStringLiteral("<p>当前集合未确认。</p>");
  for (auto it = counts.begin(); it != counts.end(); ++it) {
    const auto id = it.key();
    const auto config = catalog_[QStringLiteral("buffs")].toObject()[id].toObject();
    html += QStringLiteral("<p><b>%1 · 记录 %2 条%3</b><br>%4</p>").arg(referenceLink(catalog_, QStringLiteral("buffs"), id)).arg(it.value())
      .arg(valueText(config[QStringLiteral("hd")]) == QStringLiteral("1") ? QStringLiteral(" · 隐藏附着动画") : QString(), officialDescription(replayBuffMessage(config[QStringLiteral("mess")].toString(), replayFighterName(f, catalog_))));
  }
  QStringList pastBuffs;
  for (const auto& id : ReplayBuffHistory::observedIds(replay_, key, current_, moment_))
    if (!counts.contains(id)) pastBuffs << referenceLink(catalog_, QStringLiteral("buffs"), id);
  if (!pastBuffs.isEmpty()) html += QStringLiteral("<h4>本局其他 Buff 记录</h4><p>以下编号在已播放部分出现过增删、数值或进度记录，目前不在此单位的成员集合中。点击查看过程。</p><p>%1</p>").arg(pastBuffs.join(QStringLiteral("<br>")));
  html += QStringLiteral("<p>最近 Buff 变更来源：%1</p><h4>初始 Buff 原始列表</h4><p>%2</p>").arg(esc(f.buffPath), esc(f.initialBuffs.join(QStringLiteral(","))));
  html += QStringLiteral("<h4>最近数值记录</h4><pre>%1</pre><h4>最近特殊记录</h4><pre>%2</pre><h4>完整初始信息</h4><pre>%3</pre>").arg(esc(pretty(f.lastNumbers)), esc(pretty(f.lastUnit)), esc(pretty(f.raw)));
  html.prepend(QStringLiteral("<p><a href='unit:%1'>返回精灵概览</a> · 原始详情</p>").arg(encodedKey));
  unit_->setHtml(html);
  // The detail tab is selected on a deliberate board click, not on each tick.
  if (sender() && (qobject_cast<QTextBrowser*>(sender()) || qobject_cast<QLabel*>(sender()) || qobject_cast<ReplayCell*>(sender()))) {
    tabs_->setCurrentWidget(unit_);
  }
}
void BattleReplayPage::showBuff(const QString& key, const QString& id, bool raw) {
  if (replay_.steps.isEmpty() || !displayedState().contains(key)) return;
  selectedUnit_ = key;
  const auto& f = displayedState()[key];
  const auto config = catalog_[QStringLiteral("buffs")].toObject()[id].toObject();
  const auto title = raw ? referenceName(catalog_, QStringLiteral("buffs"), id) : replayPublicReferenceName(catalog_, QStringLiteral("buffs"), id);
  auto description = config[QStringLiteral("mess")].toString();
  if (description.isEmpty()) description = config.isEmpty()
    ? QStringLiteral("当前资料尚未收录此效果的说明。")
    : QStringLiteral("暂无独立说明，可在下方查看已收录的关联资料。");
  QString information = officialDescription(replayBuffMessage(description, replayFighterName(f, catalog_)));
  information.prepend(QStringLiteral("<p>%1</p>").arg(PublicDataDetailLinks::anchor(
      {PublicDataViewKind::BattleReplay,QStringLiteral("buffs"),id},QStringLiteral("查看官方效果资料与关联"))));
  if (!replayBuffHasExplanation(config))
    information.prepend(QStringLiteral("<p><b>具体效果未解释</b>：名称和动画资源不能证明状态含义。已记录的数值、执行和显示指令分别保留。</p>"));
  const auto presentation = raw ? replayBuffPresentation(config) : replayBuffPublicPresentation(config);
  if (!presentation.explanations.isEmpty())
    information += QStringLiteral("<br><br><b>官方表现规则：</b><br>") + esc(presentation.explanations.join(QLatin1Char('\n'))).replace(QLatin1Char('\n'), QStringLiteral("<br>"));
  for (const auto& field : {QStringLiteral("beffect"), QStringLiteral("bhs")}) {
    const auto value = config[field].toString(); if (value.isEmpty()) continue;
    information += QStringLiteral("<br><br><b>%1：</b>%2").arg(field == QStringLiteral("beffect")
      ? QStringLiteral("官方效果备注") : QStringLiteral("官方伤害来源标注"), officialDescription(value));
  }
  QMap<QPair<int, QString>, QPair<QString, int>> origins;
  for (const auto& member : recordedBuffMembers(f)) if (member.id == id) {
    auto& origin = origins[qMakePair(member.sourceTeam, member.source)]; ++origin.second;
    const bool known = replayBuffSourceSide(member.source, member.sourceTeam, side_->currentData().toInt()) != ReplayBuffSourceSide::Unknown;
    if (known) {
      const bool own = member.sourceTeam == side_->currentData().toInt();
      const auto name = displayedState().contains(member.source) ? replayFighterName(displayedState().value(member.source), catalog_) : member.sourceName.isEmpty() ? QStringLiteral("施加者名称未记录") : replayPublicFighterNameText(member.sourceName);
      origin.first = QStringLiteral("<span style='color:%1'><b>%2</b> · %3</span>")
        .arg(own ? QStringLiteral("#245da7") : QStringLiteral("#af3550"), own ? QStringLiteral("我方施加") : QStringLiteral("敌方施加"), esc(name));
    } else origin.first = QStringLiteral("<span style='color:#65717e'>来源未记</span>");
  }
  if (!origins.isEmpty()) {
    QStringList lines; for (const auto& origin : origins) lines.append(origin.first + QStringLiteral(" · 记录 %1 条").arg(origin.second));
    information.prepend(QStringLiteral("<b>当前成员的施加来源</b><br>") + lines.join(QStringLiteral("<br>")) + QStringLiteral("<br><br>"));
  }
  const auto history = ReplayBuffHistory::recorded(replay_, key, id, current_, moment_);
  QString historyHtml = QStringLiteral("<h4>本局已记录过程 · %1 条</h4><p>截至当前播放位置，最新记录在前。记录数不是层数；数值关联不认定施加者。</p>").arg(history.size());
  if (history.isEmpty()) historyHtml += QStringLiteral("<p>当前播放位置之前，该精灵没有此效果的明确记录。</p>");
  else {
    historyHtml += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='5'><tr bgcolor='#e8eff8'><th align='left'>时点</th><th align='left'>已记录变化</th><th align='left'>施加来源</th></tr>");
    for (int i = history.size() - 1; i >= 0; --i) {
      const auto& entry = history[i]; const auto& event = entry.effect;
      const auto& step = replay_.steps[entry.step];
      const auto when = entry.initial ? QStringLiteral("初始阵容") : (step.round >= 0 ? QStringLiteral("第 %1 回合<br>").arg(step.round) : QString()) + QStringLiteral("行动 %1").arg(entry.step);
      QString change = entry.initial ? (event.kind == QStringLiteral("progress") ? QStringLiteral("初始进度 %1 / %2").arg(number(event.after), valueText(event.raw.toObject()[QStringLiteral("mp")])) :
        QStringLiteral("初始已有 %1 条成员记录").arg(number(event.after))) : effectSummary(event);
      if (!entry.initial && event.kind != QStringLiteral("buff") && event.kind != QStringLiteral("progress")) change = QStringLiteral("关联记录 · ") + change;
      QString origin = QStringLiteral("<span style='color:#697583'>来源未记</span>");
      if (!entry.initial && replayBuffSourceSide(event.buffSource, event.buffSourceTeam, side_->currentData().toInt()) != ReplayBuffSourceSide::Unknown) {
        const auto& atEvent = event.moment >= 0 && event.moment < step.moments.size() ? step.moments[event.moment].state : step.state;
        const auto sourceName = atEvent.contains(event.buffSource) ? replayFighterName(atEvent.value(event.buffSource), catalog_) :
          event.buffSourceName.isEmpty() ? QStringLiteral("施加者名称未记录") : replayPublicFighterNameText(event.buffSourceName);
        const bool own = event.buffSourceTeam == side_->currentData().toInt();
        origin = QStringLiteral("<span style='color:%1'>%2<br>%3</span>").arg(own ? QStringLiteral("#245da7") : QStringLiteral("#af3550"),
          own ? QStringLiteral("我方施加") : QStringLiteral("敌方施加"), esc(sourceName));
      }
      historyHtml += QStringLiteral("<tr bgcolor='%1'><td valign='top'>%2<br><a href='buff-record:%3:%4'>回到此时</a></td><td valign='top'>%5%6</td><td valign='top'>%7</td></tr>")
        .arg(i % 2 ? QStringLiteral("#ffffff") : QStringLiteral("#f2f6fc"), when).arg(entry.step).arg(entry.effectIndex)
        .arg(esc(change), raw ? QStringLiteral("<br><small>%1</small><pre>%2</pre>").arg(esc(event.path), esc(pretty(event.raw))) : QString(), origin);
    }
    historyHtml += QStringLiteral("</table>");
  }
  const auto encodedId = QString::fromLatin1(QUrl::toPercentEncoding(id));
  QString rawHtml = QStringLiteral("<p><a href='%1:%2'>%3</a></p>").arg(raw ? QStringLiteral("buff") : QStringLiteral("buff-raw"), encodedId,
    raw ? QStringLiteral("返回效果概览") : QStringLiteral("查看原始详情"));
  if (raw) rawHtml += QStringLiteral("<h4>官方配置与连接来源</h4><pre>%1</pre>").arg(esc(pretty(config)));
  unit_->setHtml(QStringLiteral("<h3>%1</h3><p>%2</p><p>当前成员记录数：%3%4</p><p>%5</p>%6<p>%7</p><p><a href='unit:%8'>查看该单位全部 Buff 和属性</a></p>%9%10")
    .arg(esc(title), esc(unitCaption(key))).arg(f.buffs.count(id))
    .arg(valueText(config[QStringLiteral("hd")]) == QStringLiteral("1") ? QStringLiteral(" · 隐藏 Buff") : QString(),
      (raw ? esc(replayBuffAvailability(config)) : QString()), historyHtml, information,
      QString::fromLatin1(QUrl::toPercentEncoding(key)), officialEntriesHtml(id, false, raw) + tableReferencesHtml(id, false, raw), rawHtml));
  unit_->setProperty("buffHistoryCount", history.size()); unit_->setProperty("buffHistoryId", id);
  tabs_->setCurrentWidget(unit_);
}
QString BattleReplayPage::tableReferencesHtml(const QString& id, bool skill, bool raw) const {
  const auto values = catalog_[skill ? QStringLiteral("relationsBySkill") : QStringLiteral("relationsByBuff")].toObject()[id].toArray();
  QSet<QString> wanted; for (const auto& value : values) wanted.insert(value.toString());
  QString html = QStringLiteral("<h4>官方表中的关联</h4>");
  if (wanted.isEmpty()) return html + QStringLiteral("<p>当前引用表没有提供该条目的技能—Buff关联。</p>");
  html += QStringLiteral("<p>以下为客户端配置引用；某次行动的施加来源仍以战斗记录为准。</p>");
  for (const auto& value : catalog_[QStringLiteral("tableRelations")].toArray()) {
    const auto row = value.toObject(); if (!wanted.contains(row[QStringLiteral("id")].toString())) continue;
    const auto skillName = referenceLink(catalog_, QStringLiteral("skills"), valueText(row[QStringLiteral("skillId")]));
    const auto buffName = referenceLink(catalog_, QStringLiteral("buffs"), valueText(row[QStringLiteral("buffId")]));
    const auto kind = row[QStringLiteral("kind")].toString();
    if (kind == QStringLiteral("combo_buff_playback")) {
      html += QStringLiteral("<p>%1：持有 %2 时，改用播放技能 %3。</p>").arg(skillName, buffName,
        referenceLink(catalog_, QStringLiteral("skills"), valueText(row[QStringLiteral("targetSkillId")])));
    } else {
      html += QStringLiteral("<p>%1 → %2%3</p>").arg(buffName, skillName,
        kind == QStringLiteral("buffer_execute_lighted_skill") ? QStringLiteral("（闪光状态的队伍执行配置）") : QStringLiteral("（队伍执行／通灵显示配置）"));
      if (!skill) {
        const auto definition = catalog_[QStringLiteral("skills")].toObject()[valueText(row[QStringLiteral("skillId")])].toObject();
        const auto description = definition[QStringLiteral("description")].toString();
        if (!description.isEmpty()) html += QStringLiteral("<p><b>关联技能说明</b><br>%1</p>").arg(officialDescription(description));
        html += officialEntriesHtml(valueText(row[QStringLiteral("skillId")]), true, raw);
      }
    }
    if (raw) html += QStringLiteral("<p style='color:#738094;font-size:10px'>%1 · %2</p>").arg(esc(row[QStringLiteral("sourceTable")].toString()), esc(row[QStringLiteral("sourcePath")].toString()));
  }
  return html;
}
QString BattleReplayPage::officialEntriesHtml(const QString& id, bool skill, bool raw) const {
  const auto labels = catalog_[skill ? QStringLiteral("entryMentionsBySkill") : QStringLiteral("entryMentionsByBuff")].toObject()[id].toArray();
  if (labels.isEmpty()) return {};
  const auto mentions = catalog_[QStringLiteral("officialEntryMentions")].toObject();
  QString html = QStringLiteral("<h4>关联官方词条说明</h4><p>按官方文字中的方括号词条查询；词条解释不代表这个 Buff 的全部机制。</p>");
  for (const auto& label : labels) {
    const auto entry = mentions[label.toString()].toObject();
    const auto text = entry[QStringLiteral("status")].toString() == QStringLiteral("resolved")
      ? officialDescription(entry[QStringLiteral("description")].toString())
      : QStringLiteral("本次官方词条表未查到此名称，保留原文。");
    html += QStringLiteral("<p><b>[%1]</b><br>%2</p>").arg(esc(label.toString()), text);
    if (raw && !entry[QStringLiteral("sourcePath")].toString().isEmpty())
      html += QStringLiteral("<p style='color:#738094;font-size:10px'>EntryConfigXML · %1</p>").arg(esc(entry[QStringLiteral("sourcePath")].toString()));
  }
  return html;
}
void BattleReplayPage::showSkill(const QString& id, bool raw) {
  const auto config = catalog_[QStringLiteral("skills")].toObject()[id].toObject();
  const auto description = config[QStringLiteral("description")].toString();
  QString html = QStringLiteral("<h3>%1</h3><p>%2</p>%3")
    .arg(esc(raw ? referenceName(catalog_, QStringLiteral("skills"), id) : replayPublicReferenceName(catalog_, QStringLiteral("skills"), id)),
      description.isEmpty() ? QStringLiteral("暂无官方技能说明。") : officialDescription(description),
      officialEntriesHtml(id, true, raw) + tableReferencesHtml(id, true, raw));
  html.insert(html.indexOf(QStringLiteral("</h3>"))+5,QStringLiteral("<p>%1</p>").arg(PublicDataDetailLinks::anchor(
      {PublicDataViewKind::BattleReplay,QStringLiteral("skills"),id},QStringLiteral("查看官方技能资料与关联"))));
  html += QStringLiteral("<p><a href='%1:%2'>%3</a></p>").arg(raw ? QStringLiteral("skill") : QStringLiteral("skill-raw"),
    QString::fromLatin1(QUrl::toPercentEncoding(id)), raw ? QStringLiteral("返回技能概览") : QStringLiteral("查看原始详情"));
  if (raw) html += ReplayReference::availability(QStringLiteral("skills"), config) + ReplayReference::context(catalog_, {}, id) +
    QStringLiteral("<h4>官方配置与连接来源</h4><pre>%1</pre>").arg(esc(pretty(config)));
  unit_->setHtml(html);
  if (displayedState().contains(selectedUnit_)) {
    const auto& fighter = displayedState()[selectedUnit_];
    unit_->append(QStringLiteral("<p><a href='unit:%1'>返回精灵概览</a> · <a href='mechanics:%2'>原始机制资料</a></p>")
      .arg(QString::fromLatin1(QUrl::toPercentEncoding(selectedUnit_)), esc(fighter.raceId)));
  }
  tabs_->setCurrentWidget(unit_);
}
void BattleReplayPage::showPetMechanics(const QString& raceId) {
  const auto pet = catalog_[QStringLiteral("petMechanics")].toObject()[raceId].toObject();
  if (pet.isEmpty()) return;
  QString html = QStringLiteral("<h3>%1 · 官方机制资料</h3><p>按当前官方精灵字典关联。是否解锁或生效，以本场战报记录为准。</p>")
    .arg(esc(petName(catalog_, raceId)));
  if (!selectedUnit_.isEmpty()) html += QStringLiteral("<p><a href='unit:%1'>返回单位状态与当前 Buff</a></p>")
    .arg(QString::fromLatin1(QUrl::toPercentEncoding(selectedUnit_)));
  html += ReplayReference::context(catalog_, raceId);
  html += ReplayReference::consumption(pet);
  html += QStringLiteral("<p>独立养成资料：%1 · %2 · %3。战报没有装备快照时，不能确认本场配置。</p>")
    .arg(ReplayReference::indexLink(QStringLiteral("stargods")), ReplayReference::indexLink(QStringLiteral("badges")), ReplayReference::indexLink(QStringLiteral("astrolabe")));
  const auto family = pet[QStringLiteral("mechanismFamily")].toString();
  const auto rules = catalog_[QStringLiteral("mechanismRules")].toObject();
  const auto plan = rules[family].toObject()[QStringLiteral("plans")].toObject()[pet[QStringLiteral("qiYunType")].toString()].toObject();
  if (!family.isEmpty()) {
    const auto label = family == QStringLiteral("suLing") ? QStringLiteral("溯灵") : QStringLiteral("气运");
    html += QStringLiteral("<h4>%1条件与效果</h4><p>本场触发情况：未判定。战报没有提供判定所需的完整开战能力值。</p>").arg(label);
    if (plan.isEmpty()) html += QStringLiteral("<p>当前资料未收录这项方案，保留未解析状态。</p>");
    for (const auto& value : plan[QStringLiteral("channels")].toArray()) {
      const auto channel = value.toObject();
      html += QStringLiteral("<p><b>%1</b></p>").arg(esc(channel[QStringLiteral("label")].toString()));
      const auto effects = channel[QStringLiteral("effects")].toArray();
      const auto thresholds = channel[QStringLiteral("thresholds")].toArray();
      for (int index = 0; index < effects.size(); ++index) {
        auto description = effects[index].toString();
        description.replace(QRegularExpression(QStringLiteral("#V[0-9]+#")), QStringLiteral("【数值未记录】"));
        if (family == QStringLiteral("qiYun") && index < thresholds.size())
          html += QStringLiteral("<p>属性档位 ≥ %1%：%2</p>").arg(QString::number(thresholds[index].toDouble() * 100, 'g', 12), officialDescription(description));
        else html += QStringLiteral("<p>%1</p>").arg(officialDescription(description));
      }
    }
    if (family == QStringLiteral("qiYun") && !plan.isEmpty()) html += QStringLiteral("<p>档位按官方顺序取已达到的最高档；本页未代入本场属性。</p>");
  }
  const auto huanId = pet[QStringLiteral("otherReferences")].toObject()[QStringLiteral("huanSkillNo")].toString();
  const auto huan = catalog_[QStringLiteral("huanMechanics")].toObject()[huanId].toObject();
  if (!huanId.isEmpty()) {
    html += QStringLiteral("<h4>寰力量／异族审判资料 [%1]</h4><p>本场是否生效：未判定。以下保留官方各项条件与效果，未代入本场战斗力。</p>").arg(esc(huanId));
    if (huan.isEmpty()) html += QStringLiteral("<p>当前资料未收录这项配置。</p>");
    for (const auto& item : huan[QStringLiteral("variants")].toArray()) {
      const auto variant = item.toObject();
      const auto condition = variant[QStringLiteral("typeToTakeEffect")].toInt() == 1
        ? QStringLiteral("战斗力 ≥ %1").arg(variant[QStringLiteral("valueToTakeEffect")].toInt())
        : QStringLiteral("条件类型尚未解析，保留官方说明。");
      html += QStringLiteral("<p><b>%1</b><br>%2<br>%3</p>").arg(esc(variant[QStringLiteral("name")].toString()), condition, officialDescription(variant[QStringLiteral("tip")].toString()));
    }
  }
  const auto legendId = pet[QStringLiteral("otherReferences")].toObject()[QStringLiteral("legendSkillNo")].toString();
  const auto legend = catalog_[QStringLiteral("legendMechanics")].toObject()[QStringLiteral("definitions")].toObject()[legendId].toObject();
  if (!legendId.isEmpty() && pet[QStringLiteral("legendReferenceNamespace")].toString() == QStringLiteral("legacyLegend")) {
    html += QStringLiteral("<h4>传说技能资料 [%1]</h4><p>本场档位：未记录。以下保留官方各档配置，不按最高档补算。</p>").arg(esc(legendId));
    if (legend.isEmpty()) html += QStringLiteral("<p>当前资料未收录这项传说技能。</p>");
    for (const auto& item : legend[QStringLiteral("variants")].toArray()) {
      const auto variant = item.toObject();
      const auto selector = variant[QStringLiteral("triggerParam")].toString();
      QString condition = QStringLiteral("官方选档参数：%1").arg(esc(selector));
      const int trigger = variant[QStringLiteral("trigger")].toInt();
      if ((trigger == 1 || trigger == 2) && selector.contains(QLatin1Char(':')))
        condition = QStringLiteral("计入等级的%1：%2；等级总和范围：%3")
          .arg(trigger == 1 ? QStringLiteral("传说石组") : QStringLiteral("传说石编号"),
               esc(selector.section(QLatin1Char(':'), 0, 0)), esc(selector.section(QLatin1Char(':'), 1)));
      const auto description = variant[QStringLiteral("descX")].toString().isEmpty() ? variant[QStringLiteral("desc")].toString() : variant[QStringLiteral("descX")].toString();
      html += QStringLiteral("<p><b>%1</b><br>%2<br>%3</p>").arg(esc(variant[QStringLiteral("name")].toString()), condition, officialDescription(description));
    }
  }
  if (pet[QStringLiteral("legendReferenceNamespace")].toString() == QStringLiteral("unclassified")) {
    html += QStringLiteral("<h4>尚未确认读取分支的精灵字段</h4><p>官方字典记录 legendSkillNo=%1，但该精灵没有满足旧传说资料的读取条件。下列同号说明仅供核对，尚不能认定与此字段对应。</p>")
      .arg(esc(pet[QStringLiteral("rawLegendSkillNo")].toString()));
    for (const auto& value : pet[QStringLiteral("legendReferenceCandidates")].toArray()) {
      const auto candidate = value.toObject();
      html += QStringLiteral("<p>同号候选说明（对应关系未证实）：%1</p>")
        .arg(referenceLink(catalog_, QStringLiteral("skills"), candidate[QStringLiteral("id")].toString()));
    }
  }
  html += QStringLiteral("<h4>精灵字典中的已核实技能引用</h4><p>点击技能可查看完整官方说明与已核实的 Buff 关联。</p>");
  for (const auto& value : pet[QStringLiteral("skillLinks")].toArray()) {
    const auto link = value.toObject();
    html += QStringLiteral("<p>%1：%2</p>").arg(esc(link[QStringLiteral("label")].toString()), referenceLink(catalog_, QStringLiteral("skills"), link[QStringLiteral("skillId")].toString()));
  }
  html += QStringLiteral("<h4>关联依据</h4><pre>%1</pre>").arg(esc(pretty(pet)));
  if (!plan.isEmpty()) html += QStringLiteral("<h4>官方机制配置原文</h4><pre>%1</pre>").arg(esc(pretty(plan)));
  if (!legend.isEmpty()) html += QStringLiteral("<h4>传说技能配置原文</h4><pre>%1</pre>").arg(esc(pretty(legend)));
  if (!huan.isEmpty()) html += QStringLiteral("<h4>寰力量／异族审判配置原文</h4><pre>%1</pre>").arg(esc(pretty(huan)));
  unit_->setHtml(html);
  tabs_->setCurrentWidget(unit_);
}
void BattleReplayPage::updateNotes() {
  const auto provenance = catalog_[QStringLiteral("provenance")].toObject();
  QString html = QStringLiteral("<h3>数据核对</h3><p>动作子步骤按官方客户端命令边界重建。护盾按已执行增减累计；当前 Buff 按初始列表和 bd_bfs/ad_bfs 变更维护，新增保留重复、删除一条匹配记录。旧增删字段不重复应用。</p><p>红色生命条为当前生命/生命上限；蓝色文字为当前护盾余额；橙色气势条为当前气势/官方满气势值。通灵格数取日志 mp，当前进度取 cp/proa，保留超出满值的原文数值。未知值用 ? 标记。</p><p>绿框表示官方召唤标记，粉框表示官方元能配置与唯一开局属性变化交叉匹配的目标；多来源、纯赋能或证据不足时不强行归属，ibat 也不单独用作元能判定。两个身份可同时存在。</p><p>形态、属性、技能和分身按特殊记录所在阶段更新。t=3 本身不触发登场；召唤技能的登场阶段独立重建。等待型 Buff 的 summonAppear 只恢复显示层，不改变登场标志或生命。生命归零与恢复是数值事实，不能仅据此归因为某个被动。玩家换位更新整套阵容位置，不改变精灵自身格位；已移除阵容标明末次记录。</p><p>作用箭头只连接有直接依据的来源和目标。Buff 变更缺少施加者时明确标注。各目标的动画可能并行；技能结算按同阶段汇总，未生成日志中不存在的命中帧或因果。未确认值不会补成 0。</p><p>解析依据和配置来自下载时的官方版本，可能晚于历史日志；不是历史版本一致性的证明。详见工具目录 state-evidence.json、battlefield-evidence.json 与 border-evidence.json。</p>");
  html += QStringLiteral("<p>当前文件：%1<br>SHA-256：%2</p>").arg(esc(file_), esc(replay_.sha256));
  html += QStringLiteral("<p>格内紫色标签为按当前已核对配置生成的官方客户端提示，包含属性强弱效、神职和符合条件的天启提示；只使用命中阶段当时的状态，不改变数值账本。历史版本仍需单独核对。</p><p>追击本体和变身来源按官方规则独立关联，保留当前精灵原名。小神龙按玩家名单及明确出手记录展示准备、出手中、已完成，回退时重新还原。</p><p>Buff 隐藏附着动画、动画仅播放一次、原版抑制飘字都属于表现规则；不会据此删除 Buff 或隐藏本回放已记录的数值。初始护盾字段完全缺省时按官方默认 0，显式 null 或非法值仍为未知。</p>");
  html += QStringLiteral("<h4>官方表连接</h4><p>%1 条 Buff 条目，%2 条技能条目，%3 条明确引用。名称与说明按官方 ID 连接，Flash/H5 原文差异单独保留，显示资源编号不当作说明编号。点击单位详情中的技能或 Buff，可沿双向关联查看资料。</p>")
    .arg(catalog_[QStringLiteral("buffs")].toObject().size()).arg(catalog_[QStringLiteral("skills")].toObject().size()).arg(catalog_[QStringLiteral("tableRelations")].toArray().size());
  if (catalog_.isEmpty()) html += QStringLiteral("<p style='color:#a05228'>官方字典没有加载，名称仅显示原始 ID。</p>");
  html += QStringLiteral("<h4>本文件待核实项（%1）</h4>").arg(replay_.issues.size());
  for (const auto& issue : replay_.issues) html += QStringLiteral("<p>%1</p>").arg(esc(issue));
  html += QStringLiteral("<h4>官方包来源与版本</h4><pre>%1</pre>").arg(esc(pretty(provenance)));
  notes_->setHtml(html);
}
