#include "routine_overview_window.h"

#include "diagnostics/build_info.h"
#include "ui/common/ui_preferences.h"

#include "domain/checked_json_numbers.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QHeaderView>
#include <QFont>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollBar>
#include <limits>
#include <optional>

namespace {

// Per-period allowances the server does not send with the counters. Kept in
// one place so a rule change in the game is a one-line edit.
constexpr int kStarAdventureDailyLimit = 3;
constexpr int kStarAdventureWeeklyLimit = 6;
constexpr int kCompetitionDailyBase = 40;
constexpr int kCompetitionWeeklyLimit = 40;
constexpr int kFarmDailyRefreshLimit = 16;
constexpr int kArenaDailyBase = 8;

QString localTime(const QDateTime& utc) {
  return utc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QTableWidget* makeTable(QWidget* parent, const QStringList& headers) {
  auto* table = new QTableWidget(parent);
  table->setColumnCount(headers.size());
  table->setHorizontalHeaderLabels(headers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setAlternatingRowColors(true);
  table->verticalHeader()->setVisible(false);
  table->setWordWrap(false);
  table->setTextElideMode(Qt::ElideNone);
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  return table;
}

QTableWidgetItem* item(const QString& text, const QColor& color = {}) {
  auto* result = new QTableWidgetItem(text);
  result->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  if (color.isValid()) {
    result->setForeground(QBrush(color));
    QFont font = result->font();
    font.setBold(true);
    result->setFont(font);
  }
  return result;
}

std::optional<qint64> count(const QJsonValue& value) {
  qint64 number = 0;
  if (!DomainNumeric::checkedInteger(value, &number, 0, std::numeric_limits<int>::max())) return {};
  return number;
}
QString number(const std::optional<qint64>& value) { return value ? QString::number(*value) : QStringLiteral("—"); }
QString observationState(const ObservationValidity& state, const QString& current) {
  if (state.current()) return current;
  return state.state == ObservationValidityState::Invalidated ? QStringLiteral("已失效，请刷新")
                                                            : QStringLiteral("上次读取（周期未确认）");
}
QString sourceTip(const ObservationValidity& state) {
  QStringList values;
  values << observationState(state, QStringLiteral("数据有效"));
  if (state.observedAtUtc.isValid()) values << QStringLiteral("读取时间：%1").arg(localTime(state.observedAtUtc));
  if (state.validUntilUtc.isValid()) values << QStringLiteral("有效截止：%1").arg(localTime(state.validUntilUtc));
  if (!state.periodId.isEmpty()) values << QStringLiteral("周期标识：%1").arg(state.periodId);
  if (!state.reason.isEmpty()) values << state.reason;
  if (!state.evidenceReference.isEmpty()) values << state.evidenceReference;
  return values.join(QLatin1Char('\n'));
}
std::optional<qint64> remaining(qint64 base, const std::optional<qint64>& bought,
                               const std::optional<qint64>& used) {
  qint64 maximum = 0;
  if (!bought || !used || !DomainNumeric::checkedAdd(base, *bought, &maximum) ||
      maximum > std::numeric_limits<int>::max() || *used > maximum) return {};
  return maximum - *used;
}
bool sameValidity(const QHash<QString, ObservationValidity>& left, const QHash<QString, ObservationValidity>& right) {
  if (left.size() != right.size()) return false;
  for (auto entry = left.begin(); entry != left.end(); ++entry) {
    const auto other = right.constFind(entry.key());
    if (other == right.cend()) return false;
    const auto& a = entry.value(); const auto& b = other.value();
    if (a.state != b.state || a.reason != b.reason || a.evidenceReference != b.evidenceReference ||
        a.periodId != b.periodId || a.observedAtUtc != b.observedAtUtc || a.validUntilUtc != b.validUntilUtc ||
        a.observationSequence != b.observationSequence || a.expiresMonotonicMs != b.expiresMonotonicMs) return false;
  }
  return true;
}

}  // namespace

RoutineOverviewWindow::RoutineOverviewWindow(QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("KQRoutineOverviewWindow"));
  setWindowTitle(QStringLiteral("精灵工作台 · 日常 / 周常 / 活动 · %1")
                     .arg(BuildInfo::displayVersion()));
  resize(1260, 820);
  setMinimumSize(980, 640);
  setAttribute(Qt::WA_DeleteOnClose, false);

  auto* root = new QVBoxLayout(this);
  auto* toolbar = new QHBoxLayout();
  refresh_ = new QPushButton(QStringLiteral("手动刷新任务/活动"), this);
  refresh_->setToolTip(QStringLiteral("只在点击时查询日常、周常、活动红点和已接入玩法次数；不会自动刷新"));
  status_ = new QLabel(QStringLiteral("本界面没有自动刷新"), this);
  status_->setTextFormat(Qt::PlainText);
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  toolbar->addWidget(refresh_);
  toolbar->addWidget(status_, 1);
  root->addLayout(toolbar);

  auto* tabs = new QTabWidget(this);
  auto* dailyPage = new QWidget(tabs);
  auto* dailyLayout = new QVBoxLayout(dailyPage);
  dailySummary_ = new QLabel(dailyPage);
  dailySummary_->setObjectName(QStringLiteral("KQRoutineDailySummary")); dailySummary_->setTextFormat(Qt::PlainText);
  dailySummary_->setWordWrap(true);
  dailyTable_ = makeTable(dailyPage,
                          {QStringLiteral("日常任务"), QStringLiteral("状态"),
                           QStringLiteral("当前进度"), QStringLiteral("完成目标"),
                           QStringLiteral("距任务完成"),
                           QStringLiteral("完成活跃值")});
  dailyTable_->setObjectName(QStringLiteral("KQRoutineDailyTable"));
  dailyLayout->addWidget(dailySummary_);
  dailyLayout->addWidget(dailyTable_, 1);
  tabs->addTab(dailyPage, QStringLiteral("日常"));

  auto* weeklyPage = new QWidget(tabs);
  auto* weeklyLayout = new QVBoxLayout(weeklyPage);
  weeklySummary_ = new QLabel(weeklyPage);
  weeklySummary_->setObjectName(QStringLiteral("KQRoutineWeeklySummary")); weeklySummary_->setTextFormat(Qt::PlainText);
  weeklySummary_->setWordWrap(true);
  weeklyTable_ = makeTable(weeklyPage,
                           {QStringLiteral("周常任务"), QStringLiteral("状态"),
                            QStringLiteral("当前进度"), QStringLiteral("完成目标"),
                            QStringLiteral("距任务完成"),
                            QStringLiteral("完成活跃值")});
  weeklyTable_->setObjectName(QStringLiteral("KQRoutineWeeklyTable"));
  weeklyLayout->addWidget(weeklySummary_);
  weeklyLayout->addWidget(weeklyTable_, 1);
  tabs->addTab(weeklyPage, QStringLiteral("周常"));

  auto* activityPage = new QWidget(tabs);
  auto* activityLayout = new QVBoxLayout(activityPage);
  activityNote_ = new QLabel(activityPage);
  activityNote_->setTextFormat(Qt::PlainText);
  activityNote_->setWordWrap(true);
  activityTable_ = makeTable(activityPage,
                             {QStringLiteral("当前活动"), QStringLiteral("状态"),
                              QStringLiteral("待处理红点"), QStringLiteral("上线日期")});
  activityTable_->setObjectName(QStringLiteral("KQRoutineActivityTable"));
  activityLayout->addWidget(activityNote_);
  activityLayout->addWidget(activityTable_, 1);
  tabs->addTab(activityPage, QStringLiteral("活动"));

  auto* opportunityPage = new QWidget(tabs);
  auto* opportunityLayout = new QVBoxLayout(opportunityPage);
  opportunityNote_ = new QLabel(opportunityPage);
  opportunityNote_->setTextFormat(Qt::PlainText);
  opportunityNote_->setWordWrap(true);
  opportunityTable_ = makeTable(
      opportunityPage,
      {QStringLiteral("玩法"), QStringLiteral("周期"),
       QStringLiteral("剩余次数"), QStringLiteral("周期上限"),
       QStringLiteral("已使用 / 已进行"), QStringLiteral("数据状态")});
  opportunityTable_->setObjectName(QStringLiteral("KQRoutineOpportunityTable"));
  opportunityLayout->addWidget(opportunityNote_);
  opportunityLayout->addWidget(opportunityTable_, 1);
  tabs->addTab(opportunityPage, QStringLiteral("玩法剩余次数"));
  UiPreferences::bindTabWidget(tabs, QStringLiteral("routine/tab"));
  root->addWidget(tabs, 1);

  connect(refresh_, &QPushButton::clicked, this,
          &RoutineOverviewWindow::refreshRequested);
  rebuild();
}

void RoutineOverviewWindow::setData(const QJsonObject& dailyPacket, bool hasDaily,
                                    const QSet<int>& activeRedPoints,
                                    bool hasRedPoints,
                                    const QJsonObject& opportunityPackets) {
  sourceDailyPacket_ = dailyPacket;
  hasSourceDaily_ = hasDaily;
  sourceRedPoints_ = activeRedPoints;
  hasSourceRedPoints_ = hasRedPoints;
  sourceOpportunityPackets_ = opportunityPackets;
  updateObservedData();
}

void RoutineOverviewWindow::setReadOnlyObservations(const QJsonObject& packets) {
  if (readOnlyPackets_ == packets) return;
  readOnlyPackets_ = packets;
  updateObservedData();
}

void RoutineOverviewWindow::updateObservedData() {
  dailyPacket_ = sourceDailyPacket_;
  hasDaily_ = hasSourceDaily_;
  activeRedPoints_ = sourceRedPoints_;
  hasRedPoints_ = hasSourceRedPoints_;
  opportunityPackets_ = sourceOpportunityPackets_;
  for (auto packet = readOnlyPackets_.begin(); packet != readOnlyPackets_.end(); ++packet) {
    const auto observation = packet.value().toObject();
    if (packet.key() == QStringLiteral("1008_20170623_dt_0")) {
      for (auto field = observation.begin(); field != observation.end(); ++field)
        dailyPacket_.insert(field.key(), field.value());
      hasDaily_ = true;
    } else if (packet.key() == QStringLiteral("1037_0")) {
      if (!observation.value(QStringLiteral("rs")).isString()) continue;
      activeRedPoints_.clear();
      for (const auto& token : observation.value(QStringLiteral("rs")).toString().split(QLatin1Char('#'), Qt::SkipEmptyParts)) {
        bool valid = false;
        const int point = token.toInt(&valid);
        if (valid && point > 0) activeRedPoints_.insert(point);
      }
      hasRedPoints_ = true;
    } else opportunityPackets_.insert(packet.key(), observation);
  }
  rebuild();
}

void RoutineOverviewWindow::setStatus(const QString& status) { status_->setText(status); }

void RoutineOverviewWindow::setCatalogSnapshot(std::shared_ptr<const RoutineCatalogSnapshot> catalog) {
  if (!catalog || catalog == catalog_) return;
  if (catalog_ && catalog->revision < catalog_->revision) return;
  catalog_ = std::move(catalog); rebuild();
}
void RoutineOverviewWindow::setPeriodValidity(const QHash<QString, ObservationValidity>& value) {
  if (sameValidity(periodValidity_, value)) return;
  periodValidity_ = value; rebuild();
}
ObservationValidity RoutineOverviewWindow::validity(const QString& key) const {
  const QString group = key.section(QLatin1Char(':'), 0, 0);
  if (readOnlyPackets_.contains(group) ||
      (group == QStringLiteral("rs") && readOnlyPackets_.contains(QStringLiteral("1037_0"))) ||
      readOnlyPackets_.value(QStringLiteral("1008_20170623_dt_0")).toObject().contains(group))
    return {};
  return periodValidity_.value(key);
}

void RoutineOverviewWindow::resetSessionContext() {
  periodValidity_.clear();
  readOnlyPackets_ = {};
  setData({}, false, {}, false, {});
  setRunning(false);
  status_->setText(QStringLiteral("会话已更新，等待当前账号的任务与活动数据"));
  for (QTableWidget* table : {dailyTable_, weeklyTable_, activityTable_, opportunityTable_})
    { table->clearSelection(); table->setCurrentCell(-1, -1); }
}

void RoutineOverviewWindow::setRunning(bool running) {
  refresh_->setEnabled(!running);
  refresh_->setText(running ? QStringLiteral("刷新中……")
                            : QStringLiteral("手动刷新任务/活动"));
}

QString RoutineOverviewWindow::prizeSummary(bool daily) const {
  if (!catalog_) return QStringLiteral("目录尚未就绪");
  const QString activeKey = daily ? QStringLiteral("av") : QStringLiteral("wav");
  const QString claimedKey = daily ? QStringLiteral("bi") : QStringLiteral("wbi");
  const QString period = daily ? QStringLiteral(":daily") : QStringLiteral(":weekly");
  const auto active = hasDaily_ ? count(dailyPacket_.value(activeKey)) : std::optional<qint64>{};
  const auto states = hasDaily_ ? dailyPacket_.value(claimedKey).toArray() : QJsonArray{};
  const auto& thresholds = daily ? catalog_->dayPrizeThresholds : catalog_->weekPrizeThresholds;
  const bool claimedCurrent = validity(claimedKey + period).current();
  const bool current = validity(activeKey + period).current() && claimedCurrent;
  QStringList parts;
  for (int index = 0; index < thresholds.size(); ++index) {
    const int threshold = thresholds[index];
    const auto claimed = index < states.size() ? states[index] : QJsonValue{};
    QString state;
    if (threshold <= 0) state = QStringLiteral("目录目标无效");
    else if (!claimed.isBool()) state = QStringLiteral("领取状态未知");
    else if (claimed.toBool()) state = claimedCurrent ? QStringLiteral("已领取") : QStringLiteral("已领取（周期未确认）");
    else if (!active) state = QStringLiteral("活跃值未知");
    else if (!current) state = *active >= threshold ? QStringLiteral("已达到（周期未确认）") : QStringLiteral("未达到（周期未确认）");
    else state = *active >= threshold ? QStringLiteral("可领取") : QStringLiteral("未达到");
    parts.append(QStringLiteral("%1 %2").arg(threshold).arg(state));
  }
  return parts.isEmpty() ? QStringLiteral("暂无已知奖励目录") : parts.join(QStringLiteral("　|　"));
}

void RoutineOverviewWindow::rebuild() {
  struct ViewState { QTableWidget* table; QString identity; int vertical; int horizontal; };
  QList<ViewState> views;
  for (QTableWidget* table : {dailyTable_, weeklyTable_, activityTable_, opportunityTable_}) {
    const auto* selected = table->currentRow() >= 0 ? table->item(table->currentRow(), 0) : nullptr;
    views.append({table, selected ? selected->data(Qt::UserRole).toString() : QString{},
                  table->verticalScrollBar()->value(), table->horizontalScrollBar()->value()});
  }
  rebuildTasks(); rebuildActivities(); rebuildOpportunities();
  for (const auto& view : views) {
    if (!view.identity.isEmpty()) for (int row = 0; row < view.table->rowCount(); ++row) {
      if (view.table->item(row, 0) && view.table->item(row, 0)->data(Qt::UserRole).toString() == view.identity) {
        view.table->setCurrentCell(row, 0); break;
      }
    }
    view.table->verticalScrollBar()->setValue(view.vertical);
    view.table->horizontalScrollBar()->setValue(view.horizontal);
  }
}

void RoutineOverviewWindow::rebuildOpportunities() {
  opportunityTable_->setRowCount(0);
  const auto add = [this](const QString& name, const QString& period,
      const std::optional<qint64>& available, const QString& limit, const QString& used,
      const QString& group, const QString& source) {
    const auto state = validity(group + QStringLiteral(":activity"));
    const int row = opportunityTable_->rowCount(); opportunityTable_->insertRow(row);
    const QStringList values{name, period, number(available), limit, used,
        !available ? QStringLiteral("未读取到或数据无效") : observationState(state, QStringLiteral("数据有效"))};
    // Protocol origins stay available for troubleshooting without crowding the table.
    const QString tip = sourceTip(state) + QStringLiteral("\n数据来源：%1").arg(source);
    for (int column = 0; column < values.size(); ++column) {
      const QColor color = available && state.current() && column == 2
          ? (*available > 0 ? QColor(QStringLiteral("#087a43")) : QColor(QStringLiteral("#b54708"))) : QColor{};
      auto* cell = item(values[column], color); cell->setToolTip(tip);
      opportunityTable_->setItem(row, column, cell);
    }
    opportunityTable_->item(row, 0)->setData(Qt::UserRole, name + QLatin1Char('/') + period);
  };
  const QString starKey = QStringLiteral("1008_20220603_swa_0_0");
  const auto star = opportunityPackets_.value(starKey).toObject();
  const auto starToday = count(star.value(QStringLiteral("ti")));
  const auto starWeek = count(star.value(QStringLiteral("wgt")));
  add(QStringLiteral("星轮探险"), QStringLiteral("今日"), starToday, QString::number(kStarAdventureDailyLimit),
      starToday && *starToday <= kStarAdventureDailyLimit ? QString::number(kStarAdventureDailyLimit - *starToday)
                                                          : QStringLiteral("—"), starKey,
      QStringLiteral("1008_20220603_swa_0_0：ti"));
  add(QStringLiteral("星轮探险"), QStringLiteral("本周"),
      starWeek && *starWeek <= kStarAdventureWeeklyLimit ? std::optional<qint64>(kStarAdventureWeeklyLimit - *starWeek)
                                                         : std::nullopt,
      QString::number(kStarAdventureWeeklyLimit), number(starWeek), starKey, QStringLiteral("1008_20220603_swa_0_0：wgt"));
  const QString treeKey = QStringLiteral("1008_20190531_gbt_1");
  add(QStringLiteral("缤纷树"), QStringLiteral("今日"), count(opportunityPackets_.value(treeKey).toObject().value(QStringLiteral("ti"))),
      QStringLiteral("—"), QStringLiteral("—"), treeKey, QStringLiteral("1008_20190531_gbt_1：ti（服务器剩余次数）"));
  const QString beastKey = QStringLiteral("2_36_1");
  add(QStringLiteral("源兽之门"), QStringLiteral("今日"), count(opportunityPackets_.value(beastKey).toObject().value(QStringLiteral("t"))),
      QStringLiteral("—"), QStringLiteral("—"), beastKey, QStringLiteral("PJXExtension / 2_36_1：t"));
  const QString competitionKey = QStringLiteral("110_123_0");
  const auto competition = opportunityPackets_.value(competitionKey).toObject();
  const auto bought = count(competition.value(QStringLiteral("rdb"))), used = count(competition.value(QStringLiteral("rdt")));
  const auto daily = remaining(kCompetitionDailyBase, bought, used);
  add(QStringLiteral("全民斗技"), QStringLiteral("今日"), daily,
      bought && *bought <= std::numeric_limits<int>::max() - kCompetitionDailyBase
          ? QStringLiteral("%1 + 已购 %2").arg(kCompetitionDailyBase).arg(*bought) : QStringLiteral("—"),
      number(used), competitionKey, QStringLiteral("110_123_0：%1 + rdb - rdt").arg(kCompetitionDailyBase));
  add(QStringLiteral("全民斗技"), QStringLiteral("本周"), count(competition.value(QStringLiteral("rwwt"))),
      QString::number(kCompetitionWeeklyLimit),
      number(count(competition.value(QStringLiteral("wwt")))), competitionKey, QStringLiteral("110_123_0：rwwt"));
  const QString farmKey = QStringLiteral("1008_20260522_nf_0");
  const auto farm = opportunityPackets_.value(farmKey).toObject();
  const auto refreshes = count(farm.value(QStringLiteral("rft")));
  add(QStringLiteral("最新版农场·可用次数"), QStringLiteral("今日"), count(farm.value(QStringLiteral("pt"))),
      QStringLiteral("—"), QStringLiteral("—"), farmKey, QStringLiteral("1008_20260522_nf_0：pt"));
  add(QStringLiteral("最新版农场·刷新次数"), QStringLiteral("今日"), refreshes, QString::number(kFarmDailyRefreshLimit),
      refreshes && *refreshes <= kFarmDailyRefreshLimit ? QString::number(kFarmDailyRefreshLimit - *refreshes)
                                                        : QStringLiteral("—"), farmKey,
      QStringLiteral("1008_20260522_nf_0：rft"));
  add(QStringLiteral("灵骑破封"), QStringLiteral("今日"), {}, QStringLiteral("—"), QStringLiteral("—"), {},
      QStringLiteral("现有129_0_1为执行破封，没有已核验的只读次数来源"));
  const auto arena = opportunityPackets_.value(QStringLiteral("16_24_A")).toObject();
  const auto addArena = [&](const QString& name, const QString& field) {
    const auto info = arena.value(field).toObject();
    const auto used = count(info.value(QStringLiteral("ct"))), bought = count(info.value(QStringLiteral("bct")));
    const auto available = remaining(kArenaDailyBase, bought, used);
    add(name, QStringLiteral("今日"), available,
        bought && *bought <= std::numeric_limits<int>::max() - kArenaDailyBase
            ? QStringLiteral("%1 + 已购 %2").arg(kArenaDailyBase).arg(*bought) : QStringLiteral("—"),
        number(used), QStringLiteral("16_24_A:") + field,
        QStringLiteral("游戏内竞技场被动观察16_24_A：%1.ct/bct；扩展不主动查询").arg(field));
  };
  addArena(QStringLiteral("经典竞技场·挑战"), QStringLiteral("zao1"));
  addArena(QStringLiteral("传奇竞技场·挑战"), QStringLiteral("zao2"));
  opportunityNote_->setText(QStringLiteral(
      "数字来自最近一次读取；显示“周期未确认”时可能已经跨天或跨周，请以游戏内为准，这时也不会用绿色标记。"
      "竞技场次数在游戏内打开竞技场后才会更新；灵骑破封暂时没有可读取的次数。鼠标悬停可查看数据来源。"));
}

void RoutineOverviewWindow::rebuildTasks() {
  const auto tasks = catalog_ ? catalog_->tasks : QList<RoutineTaskDefinition>{};
  const auto build = [this, &tasks](bool daily) {
    auto* table = daily ? dailyTable_ : weeklyTable_;
    auto* summary = daily ? dailySummary_ : weeklySummary_;
    const QString field = daily ? QStringLiteral("ti") : QStringLiteral("wti");
    const QString activeField = daily ? QStringLiteral("av") : QStringLiteral("wav");
    const QString period = daily ? QStringLiteral(":daily") : QStringLiteral(":weekly");
    const auto state = validity(field + period);
    const auto progressValues = hasDaily_ ? dailyPacket_.value(field).toArray() : QJsonArray{};
    int rows = 0; for (const auto& task : tasks) if ((daily ? task.dayFinish : task.weekFinish) > 0) ++rows;
    table->setRowCount(rows);
    int row = 0, unknown = 0, unfinished = 0; qint64 totalGap = 0; bool sumKnown = true;
    for (const auto& task : tasks) {
      const int target = daily ? task.dayFinish : task.weekFinish;
      if (target <= 0) continue;
      const auto progress = row < progressValues.size() ? count(progressValues[row]) : std::optional<qint64>{};
      const auto gap = progress ? std::optional<qint64>(qMax<qint64>(0, qint64(target) - *progress)) : std::nullopt;
      const bool completed = progress && *progress >= target;
      QString status = !progress ? QStringLiteral("进度未知") : observationState(state, completed ? QStringLiteral("已完成") : QStringLiteral("未完成"));
      const QColor color = progress && state.current() ? (completed ? QColor(QStringLiteral("#087a43")) : QColor(QStringLiteral("#b54708"))) : QColor{};
      const QStringList values{task.name, status, number(progress), QString::number(target), number(gap),
                              QString::number(daily ? task.dayActive : task.weekActive)};
      for (int column = 0; column < values.size(); ++column) {
        auto* cell = item(values[column], column == 1 ? color : QColor{}); cell->setToolTip(sourceTip(state));
        table->setItem(row, column, cell);
      }
      table->item(row, 0)->setData(Qt::UserRole, (daily ? QStringLiteral("daily:") : QStringLiteral("weekly:")) + QString::number(task.id));
      if (!progress) ++unknown;
      else {
        if (*gap > 0) ++unfinished;
        qint64 next = 0; if (!DomainNumeric::checkedAdd(totalGap, *gap, &next)) sumKnown = false; else totalGap = next;
      }
      ++row;
    }
    const auto active = hasDaily_ ? count(dailyPacket_.value(activeField)) : std::optional<qint64>{};
    const auto activeState = validity(activeField + period);
    summary->setText(!catalog_ ? QStringLiteral("任务目录尚未就绪") :
        QStringLiteral("%1活跃度：%2（%3）　未完成 %4 项，进度未知 %5 项，合计还差 %6 次。奖励：%7")
            .arg(daily ? QStringLiteral("日") : QStringLiteral("周"), number(active),
                 observationState(activeState, QStringLiteral("数据有效"))).arg(unfinished).arg(unknown)
            .arg(sumKnown ? QString::number(totalGap) : QStringLiteral("—")).arg(prizeSummary(daily)));
  };
  build(true); build(false);
}

void RoutineOverviewWindow::rebuildActivities() {
  const auto activities = catalog_ ? catalog_->activities : QList<ActivityOverviewDefinition>{};
  const auto observation = validity(QStringLiteral("rs:activity"));
  activityTable_->setRowCount(activities.size());
  qint64 total = 0;
  for (int row = 0; row < activities.size(); ++row) {
    const auto& activity = activities[row];
    QSet<int> nodes;
    for (int id : activity.redPointIds) if (id > 0) nodes.insert(id);
    int active = 0; for (int id : nodes) if (activeRedPoints_.contains(id)) ++active;
    total += active;
    const bool known = hasRedPoints_ && !nodes.isEmpty();
    const QString status = !hasRedPoints_ ? QStringLiteral("红点未读取") : nodes.isEmpty() ? QStringLiteral("无法判断")
        : observationState(observation, active ? QStringLiteral("有待处理") : QStringLiteral("无红点"));
    QStringList ids; for (int id : activity.redPointIds) ids.append(QString::number(id));
    const QStringList values{activity.name, status, known ? QStringLiteral("%1 项").arg(active) : QStringLiteral("—"),
        activity.startDate.isValid() ? activity.startDate.toString(QStringLiteral("yyyy-MM-dd")) : QStringLiteral("—")};
    const QString tip = sourceTip(observation) +
        QStringLiteral("\n红点数量是亮起的提示数，不是剩余战斗次数，也不代表活动已完成。") +
        QStringLiteral("\n活动标识：%1\n红点节点：%2").arg(activity.key, ids.isEmpty() ? QStringLiteral("—") : ids.join(QLatin1Char(',')));
    for (int column = 0; column < values.size(); ++column) {
      // Absence of a red point is never an achievement/completion indicator.
      const QColor color = known && observation.current() && active > 0 && column == 1 ? QColor(QStringLiteral("#b54708")) : QColor{};
      auto* cell = item(values[column], color);
      cell->setToolTip(tip);
      activityTable_->setItem(row, column, cell);
    }
    activityTable_->item(row, 0)->setData(Qt::UserRole, activity.key);
  }
  activityNote_->setText(QStringLiteral("目录：%1；共 %2 个活动，%3 个红点亮起。红点只表示有待处理的内容，没有亮起不代表活动已完成。")
      .arg(catalog_ ? catalog_->sourceLabel : QStringLiteral("尚未就绪")).arg(activities.size()).arg(total));
}
