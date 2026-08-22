#include "routine_overview_window.h"

#include "routine_overview_catalog.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QHeaderView>
#include <QFont>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace {

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

QString rewardState(int active, int threshold, bool claimed) {
  if (claimed) return QStringLiteral("%1 已领取").arg(threshold);
  if (active >= threshold) return QStringLiteral("%1 可领取").arg(threshold);
  return QStringLiteral("%1 未达到").arg(threshold);
}

}  // namespace

RoutineOverviewWindow::RoutineOverviewWindow(QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("KQRoutineOverviewWindow"));
  setWindowTitle(QStringLiteral("原版氪奇 · 日常 / 周常 / 活动概要"));
  resize(1260, 820);
  setMinimumSize(980, 640);
  setAttribute(Qt::WA_DeleteOnClose, false);

  auto* root = new QVBoxLayout(this);
  auto* toolbar = new QHBoxLayout();
  refresh_ = new QPushButton(QStringLiteral("手动刷新任务/活动"), this);
  refresh_->setToolTip(QStringLiteral("只在点击时查询日常、周常、活动红点和已接入玩法次数；不会自动刷新"));
  status_ = new QLabel(QStringLiteral("本界面没有自动刷新"), this);
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  toolbar->addWidget(refresh_);
  toolbar->addWidget(status_, 1);
  root->addLayout(toolbar);

  auto* tabs = new QTabWidget(this);
  auto* dailyPage = new QWidget(tabs);
  auto* dailyLayout = new QVBoxLayout(dailyPage);
  dailySummary_ = new QLabel(dailyPage);
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
  activityNote_->setWordWrap(true);
  activityTable_ = makeTable(activityPage,
                             {QStringLiteral("当前活动"), QStringLiteral("状态"),
                              QStringLiteral("剩余待处理项（红点）"),
                              QStringLiteral("上线日期"), QStringLiteral("动态识别键"),
                              QStringLiteral("官方红点节点")});
  activityTable_->setObjectName(QStringLiteral("KQRoutineActivityTable"));
  activityLayout->addWidget(activityNote_);
  activityLayout->addWidget(activityTable_, 1);
  tabs->addTab(activityPage, QStringLiteral("活动"));

  auto* opportunityPage = new QWidget(tabs);
  auto* opportunityLayout = new QVBoxLayout(opportunityPage);
  opportunityNote_ = new QLabel(opportunityPage);
  opportunityNote_->setWordWrap(true);
  opportunityTable_ = makeTable(
      opportunityPage,
      {QStringLiteral("玩法"), QStringLiteral("周期"),
       QStringLiteral("剩余次数"), QStringLiteral("周期上限"),
       QStringLiteral("已使用 / 已进行"), QStringLiteral("数据状态"),
       QStringLiteral("精确数据来源")});
  opportunityTable_->setObjectName(QStringLiteral("KQRoutineOpportunityTable"));
  opportunityLayout->addWidget(opportunityNote_);
  opportunityLayout->addWidget(opportunityTable_, 1);
  tabs->addTab(opportunityPage, QStringLiteral("玩法剩余次数"));
  root->addWidget(tabs, 1);

  connect(refresh_, &QPushButton::clicked, this,
          &RoutineOverviewWindow::refreshRequested);
  rebuild();
}

void RoutineOverviewWindow::setData(const QJsonObject& dailyPacket, bool hasDaily,
                                    const QSet<int>& activeRedPoints,
                                    bool hasRedPoints,
                                    const QJsonObject& opportunityPackets) {
  dailyPacket_ = dailyPacket;
  hasDaily_ = hasDaily;
  activeRedPoints_ = activeRedPoints;
  hasRedPoints_ = hasRedPoints;
  opportunityPackets_ = opportunityPackets;
  rebuild();
}

void RoutineOverviewWindow::setStatus(const QString& status) { status_->setText(status); }

void RoutineOverviewWindow::setRunning(bool running) {
  refresh_->setEnabled(!running);
  refresh_->setText(running ? QStringLiteral("刷新中……")
                            : QStringLiteral("手动刷新任务/活动"));
}

QString RoutineOverviewWindow::prizeSummary(bool daily) const {
  const int active = daily ? dailyPacket_.value(QStringLiteral("av")).toInt()
                           : dailyPacket_.value(QStringLiteral("wav")).toInt();
  const QJsonArray states = daily ? dailyPacket_.value(QStringLiteral("bi")).toArray()
                                  : dailyPacket_.value(QStringLiteral("wbi")).toArray();
  const QVector<int>& thresholds = daily
      ? RoutineOverviewCatalog::instance().dayPrizeThresholds()
      : RoutineOverviewCatalog::instance().weekPrizeThresholds();
  QStringList parts;
  for (int index = 0; index < thresholds.size(); ++index)
    parts.append(rewardState(active, thresholds.at(index),
                             index < states.size() && states.at(index).toBool()));
  return parts.join(QStringLiteral("　|　"));
}

void RoutineOverviewWindow::rebuild() {
  rebuildTasks();
  rebuildActivities();
  rebuildOpportunities();
}

void RoutineOverviewWindow::rebuildOpportunities() {
  opportunityTable_->setRowCount(0);
  const auto addRow = [this](const QString& name, const QString& period,
                             bool available, int remaining, const QString& limit,
                             const QString& used, const QString& source) {
    const int row = opportunityTable_->rowCount();
    opportunityTable_->insertRow(row);
    const QStringList values{
        name, period, available ? QString::number(qMax(0, remaining)) : QStringLiteral("—"),
        limit, available ? used : QStringLiteral("—"),
        available ? QStringLiteral("已读取") : QStringLiteral("未查询 / 无有效缓存"), source};
    for (int column = 0; column < values.size(); ++column) {
      const QColor color = available && column == 2
                               ? (remaining > 0 ? QColor(QStringLiteral("#087a43"))
                                                : QColor(QStringLiteral("#b54708")))
                               : QColor();
      opportunityTable_->setItem(row, column, item(values.at(column), color));
    }
  };

  const QJsonObject star = opportunityPackets_
      .value(QStringLiteral("1008_20220603_swa_0_0")).toObject();
  const bool hasStar = star.value(QStringLiteral("ti")).isDouble() &&
                       star.value(QStringLiteral("wgt")).isDouble();
  const int starTodayRemaining = star.value(QStringLiteral("ti")).toInt();
  const int starWeekPlayed = star.value(QStringLiteral("wgt")).toInt();
  addRow(QStringLiteral("星轮探险"), QStringLiteral("今日"), hasStar,
         starTodayRemaining, QStringLiteral("3"),
         QString::number(qMax(0, 3 - starTodayRemaining)),
         QStringLiteral("1008_20220603_swa_0_0：ti"));
  addRow(QStringLiteral("星轮探险"), QStringLiteral("本周"), hasStar,
         6 - starWeekPlayed, QStringLiteral("6"), QString::number(starWeekPlayed),
         QStringLiteral("1008_20220603_swa_0_0：wgt"));

  const QJsonObject arena = opportunityPackets_.value(QStringLiteral("16_24_A")).toObject();
  const bool hasArena = arena.value(QStringLiteral("sweep")).isDouble();
  int arenaChallenges = 0;
  for (const QString& key : {QStringLiteral("zao1"), QStringLiteral("zao2")}) {
    const QJsonObject field = arena.value(key).toObject();
    if (field.value(QStringLiteral("curz")).toInt() > 0)
      arenaChallenges += qMax(0, field.value(QStringLiteral("ct")).toInt());
  }
  const int sweepUsed = arena.value(QStringLiteral("sweep")).toInt();
  addRow(QStringLiteral("竞技场·金币奖励挑战"), QStringLiteral("今日"), hasArena,
         8 - arenaChallenges, QStringLiteral("8"), QString::number(arenaChallenges),
         QStringLiteral("16_24_A：zao1/zao2.ct"));
  addRow(QStringLiteral("竞技场·扫荡"), QStringLiteral("今日"), hasArena,
         8 - sweepUsed, QStringLiteral("8"), QString::number(sweepUsed),
         QStringLiteral("16_24_A：sweep"));
  if (hasArena) {
    const QStringList fieldKeys{QStringLiteral("zao1"), QStringLiteral("zao2")};
    const QStringList fieldNames{QStringLiteral("竞技场·经典场挑战"),
                                 QStringLiteral("竞技场·精英场挑战")};
    for (int index = 0; index < fieldKeys.size(); ++index) {
      const QJsonObject field = arena.value(fieldKeys.at(index)).toObject();
      if (field.value(QStringLiteral("curz")).toInt() <= 0) continue;
      const int used = qMax(0, field.value(QStringLiteral("ct")).toInt());
      const int bought = qMax(0, field.value(QStringLiteral("bct")).toInt());
      addRow(fieldNames.at(index), QStringLiteral("今日"), true, 8 - used + bought,
             bought > 0 ? QStringLiteral("8＋已购%1").arg(bought) : QStringLiteral("8"),
             QString::number(used),
             QStringLiteral("16_24_A：%1.ct / bct").arg(fieldKeys.at(index)));
    }
  }

  const QJsonObject fusion = opportunityPackets_.value(QStringLiteral("100_13_0")).toObject();
  const bool hasFusion = fusion.value(QStringLiteral("pt")).isDouble();
  const int fusionUsed = fusion.value(QStringLiteral("pt")).toInt();
  addRow(QStringLiteral("精灵公园·6只圈养精灵兑换"), QStringLiteral("本周"), hasFusion,
         3 - fusionUsed, QStringLiteral("3"), QString::number(fusionUsed),
         QStringLiteral("PetParkExtension / 100_13_0：pt"));

  const QJsonObject feed = opportunityPackets_.value(QStringLiteral("100_2_0")).toObject();
  const bool hasFeed = feed.value(QStringLiteral("rfc")).isDouble();
  addRow(QStringLiteral("精灵公园·新手带回"), QStringLiteral("今日"), hasFeed,
         feed.value(QStringLiteral("rfc")).toInt(), QStringLiteral("—"),
         QStringLiteral("—"), QStringLiteral("PetParkExtension / 100_2_0：rfc"));

  opportunityNote_->setText(QStringLiteral(
      "这里显示玩法服务器直接返回的可用次数，不再把“任务目标－任务进度”冒充剩余机会。"
      "当前已接入星轮探险、竞技场和精灵公园；竞技场分场行只在账号已进入对应场次时显示。"
      "农场等未能从当前官方资源确认查询协议与字段的玩法暂不猜测，后续可按独立协议适配器继续增加。"));
}

void RoutineOverviewWindow::rebuildTasks() {
  const QList<RoutineTaskDefinition>& tasks = RoutineOverviewCatalog::instance().tasks();
  int dayCount = 0;
  int weekCount = 0;
  for (const RoutineTaskDefinition& task : tasks) {
    if (task.dayFinish > 0) ++dayCount;
    if (task.weekFinish > 0) ++weekCount;
  }
  dailyTable_->setRowCount(dayCount);
  weeklyTable_->setRowCount(weekCount);
  const QJsonArray dayValues = dailyPacket_.value(QStringLiteral("ti")).toArray();
  const QJsonArray weekValues = dailyPacket_.value(QStringLiteral("wti")).toArray();
  int dayIndex = 0;
  int weekIndex = 0;
  int dayRow = 0;
  int weekRow = 0;
  for (const RoutineTaskDefinition& task : tasks) {
    if (task.dayFinish > 0) {
      const int progress = dayIndex < dayValues.size() ? dayValues.at(dayIndex).toInt() : 0;
      const bool complete = hasDaily_ && progress >= task.dayFinish;
      const QColor stateColor = !hasDaily_ ? QColor() : complete ? QColor(QStringLiteral("#087a43"))
                                                                : QColor(QStringLiteral("#b54708"));
      dailyTable_->setItem(dayRow, 0, item(task.name));
      dailyTable_->setItem(dayRow, 1, item(!hasDaily_ ? QStringLiteral("未查询")
                                                        : complete ? QStringLiteral("已完成")
                                                                   : QStringLiteral("未完成"), stateColor));
      dailyTable_->setItem(dayRow, 2, item(hasDaily_ ? QString::number(progress)
                                                     : QStringLiteral("—")));
      dailyTable_->setItem(dayRow, 3, item(QString::number(task.dayFinish)));
      dailyTable_->setItem(dayRow, 4,
                           item(hasDaily_ ? QString::number(qMax(0, task.dayFinish - progress))
                                          : QStringLiteral("—")));
      dailyTable_->setItem(dayRow, 5, item(QString::number(task.dayActive)));
      ++dayIndex;
      ++dayRow;
    }
    if (task.weekFinish > 0) {
      const int progress = weekIndex < weekValues.size() ? weekValues.at(weekIndex).toInt() : 0;
      const bool complete = hasDaily_ && progress >= task.weekFinish;
      const QColor stateColor = !hasDaily_ ? QColor() : complete ? QColor(QStringLiteral("#087a43"))
                                                                : QColor(QStringLiteral("#b54708"));
      weeklyTable_->setItem(weekRow, 0, item(task.name));
      weeklyTable_->setItem(weekRow, 1, item(!hasDaily_ ? QStringLiteral("未查询")
                                                          : complete ? QStringLiteral("已完成")
                                                                     : QStringLiteral("未完成"), stateColor));
      weeklyTable_->setItem(weekRow, 2, item(hasDaily_ ? QString::number(progress)
                                                       : QStringLiteral("—")));
      weeklyTable_->setItem(weekRow, 3, item(QString::number(task.weekFinish)));
      weeklyTable_->setItem(weekRow, 4,
                            item(hasDaily_ ? QString::number(qMax(0, task.weekFinish - progress))
                                           : QStringLiteral("—")));
      weeklyTable_->setItem(weekRow, 5, item(QString::number(task.weekActive)));
      ++weekIndex;
      ++weekRow;
    }
  }
  int unfinishedDay = 0;
  int remainingDay = 0;
  int unfinishedWeek = 0;
  int remainingWeek = 0;
  for (int row = 0; row < dailyTable_->rowCount(); ++row) {
    const int remaining = dailyTable_->item(row, 4)->text().toInt();
    if (remaining > 0) ++unfinishedDay;
    remainingDay += remaining;
  }
  for (int row = 0; row < weeklyTable_->rowCount(); ++row) {
    const int remaining = weeklyTable_->item(row, 4)->text().toInt();
    if (remaining > 0) ++unfinishedWeek;
    remainingWeek += remaining;
  }
  dailySummary_->setText(hasDaily_
                             ? QStringLiteral("日活跃值：%1　未完成 %2 项（合计还差 %3 次）　奖励：%4")
                                   .arg(dailyPacket_.value(QStringLiteral("av")).toInt())
                                   .arg(unfinishedDay).arg(remainingDay)
                                   .arg(prizeSummary(true))
                             : QStringLiteral("尚未手动查询日常状态"));
  weeklySummary_->setText(hasDaily_
                              ? QStringLiteral("周活跃值：%1　未完成 %2 项（合计还差 %3 次）　奖励：%4")
                                    .arg(dailyPacket_.value(QStringLiteral("wav")).toInt())
                                    .arg(unfinishedWeek).arg(remainingWeek)
                                    .arg(prizeSummary(false))
                              : QStringLiteral("尚未手动查询周常状态"));
}

void RoutineOverviewWindow::rebuildActivities() {
  const QList<ActivityOverviewDefinition>& activities =
      RoutineOverviewCatalog::instance().activities();
  activityTable_->setRowCount(activities.size());
  int row = 0;
  int totalActiveNodes = 0;
  for (const ActivityOverviewDefinition& activity : activities) {
    bool active = false;
    int activeCount = 0;
    for (int id : activity.redPointIds) {
      if (activeRedPoints_.contains(id)) {
        active = true;
        ++activeCount;
      }
    }
    totalActiveNodes += activeCount;
    QString state;
    QColor color;
    QString tooltip;
    if (!hasRedPoints_) {
      state = QStringLiteral("未查询");
    } else if (activity.redPointIds.isEmpty()) {
      state = QStringLiteral("无法统一判断");
      tooltip = QStringLiteral("该活动没有接入官方统一红点节点，需要进入活动面板确认完成度。");
    } else if (active) {
      state = QStringLiteral("有待处理 / 可领取");
      color = QColor(QStringLiteral("#c62828"));
      tooltip = QStringLiteral("官方红点已亮，表示存在待处理内容或可领取奖励；不等同于活动尚未开始。");
    } else {
      state = QStringLiteral("当前无待处理");
      color = QColor(QStringLiteral("#087a43"));
      tooltip = QStringLiteral("官方红点未亮。它不能证明活动全部完成，因此不强行标记为“已完成”。");
    }
    const QString redIds = [&activity]() {
      QStringList values;
      for (int id : activity.redPointIds) values.append(QString::number(id));
      return values.join(QLatin1Char(','));
    }();
    const QString remaining = !hasRedPoints_ || activity.redPointIds.isEmpty()
                                  ? QStringLiteral("—")
                                  : QStringLiteral("%1 项").arg(activeCount);
    const QStringList values = {activity.name, state, remaining,
                                activity.startDate.isValid()
                                    ? activity.startDate.toString(QStringLiteral("yyyy-MM-dd"))
                                    : QStringLiteral("—"),
                                activity.key, redIds.isEmpty() ? QStringLiteral("—") : redIds};
    for (int column = 0; column < values.size(); ++column) {
      QTableWidgetItem* cell = item(values.at(column), column == 1 ? color : QColor());
      cell->setToolTip(tooltip);
      activityTable_->setItem(row, column, cell);
    }
    ++row;
  }
  activityNote_->setText(QStringLiteral(
      "活动目录来自最新官方 HUD 配置，每次手动刷新会整表替换：新增活动自动加入，已从官方配置删除的旧活动自动移除。"
      "活动没有统一“剩余挑战次数”协议，因此“剩余待处理项”精确统计的是当前点亮的官方红点节点数，"
      "不是剩余战斗次数；无红点时不会误报为活动全部完成。当前目录：%1（%2 个活动，当前 %3 个待处理红点）")
                             .arg(RoutineOverviewCatalog::instance().sourceLabel())
                             .arg(activities.size()).arg(totalActiveNodes));
}
