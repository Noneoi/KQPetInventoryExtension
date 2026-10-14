#include "battle_replay_life_timeline.h"
#include <QAction>
#include <QHelpEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <tuple>

namespace {
constexpr int unitRowHeight = 27;
constexpr int maxVisibleUnitRows = 5;

class RoundSegmentButton final : public QToolButton {
public:
  explicit RoundSegmentButton(QWidget* parent) : QToolButton(parent), progress_(new QProgressBar(this)) {
    setCheckable(true);
    setFixedHeight(36);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    progress_->setObjectName(QStringLiteral("replayRoundProgress"));
    progress_->setRange(0, 1000);
    progress_->setTextVisible(false);
    progress_->setAttribute(Qt::WA_TransparentForMouseEvents);
    progress_->setStyleSheet(QStringLiteral(
      "QProgressBar{border:0;background:#dce3ed;border-radius:1px;}"
      "QProgressBar::chunk{background:#5085cc;border-radius:1px;}"));
    setCurrent(false, 0);
  }
  void setCurrent(bool current, int progress) {
    setChecked(current);
    if (!styled_ || current_ != current) {
      styled_ = true; current_ = current;
      setStyleSheet(QStringLiteral(
        "QToolButton{border:1px solid %1;border-radius:5px;background:%2;color:%3;"
        "padding:3px 8px 6px;font-size:12px;font-weight:%4;}"
        "QToolButton:hover{border-color:#7095cc;}"
        "QToolButton:focus{border:2px solid #3978ce;}")
        .arg(current ? "#86a9dc" : "#dce3ed", current ? "#e4edfc" : "#f3f6fa",
          current ? "#245ca6" : "#52647b", current ? "bold" : "normal"));
    }
    progress_->setValue(progress);
  }
protected:
  void resizeEvent(QResizeEvent* event) override {
    QToolButton::resizeEvent(event);
    progress_->setGeometry(5, height() - 5, std::max(1, width() - 10), 3);
  }
private:
  QProgressBar* progress_;
  bool styled_ = false, current_ = false;
};
}

ReplayLifeTimeline::ReplayLifeTimeline(QWidget* parent) : QSlider(Qt::Horizontal, parent),
  roundStrip_(new QScrollArea(this)), roundContent_(new QWidget),
  roundLayout_(new QHBoxLayout(roundContent_)), unitScroll_(new QScrollBar(Qt::Vertical, this)) {
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setAccessibleName(QStringLiteral("分回合战斗进度与精灵死亡复活"));
  setToolTip(QStringLiteral("上方切换大回合；点击 × 死亡 / + 复活，跳到对应小回合开始并暂停。\n小回合在每个大回合内从 1 编号；悬停标记查看精灵与发生时间。"));
  roundStrip_->setObjectName(QStringLiteral("replayRoundStrip"));
  roundStrip_->setFrameShape(QFrame::NoFrame);
  roundStrip_->setWidgetResizable(true);
  roundStrip_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  roundStrip_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  roundStrip_->setFocusPolicy(Qt::NoFocus);
  roundLayout_->setContentsMargins(0, 0, 0, 0);
  roundLayout_->setSpacing(6);
  roundLayout_->setAlignment(Qt::AlignTop);
  roundStrip_->setWidget(roundContent_);
  unitScroll_->setObjectName(QStringLiteral("replayUnitTimelineScroll"));
  unitScroll_->setSingleStep(1);
  unitScroll_->setFocusPolicy(Qt::NoFocus);
  connect(unitScroll_, &QScrollBar::valueChanged, this, [this] { closeMenus(); update(); });
  connect(this, &QSlider::valueChanged, this, [this] { refreshPosition(); });
  updateGeometryAndScroll();
}
QSize ReplayLifeTimeline::sizeHint() const { return {720, totalHeight()}; }
QSize ReplayLifeTimeline::minimumSizeHint() const { return {260, totalHeight()}; }
int ReplayLifeTimeline::headerHeight() const {
  return 42 + (roundContent_->minimumWidth() > width() ? style()->pixelMetric(QStyle::PM_ScrollBarExtent) : 0);
}
int ReplayLifeTimeline::visibleUnitRows() const { return std::min(maxVisibleUnitRows, std::max(1, int(unitOrder_.size()))); }
int ReplayLifeTimeline::totalHeight() const { return headerHeight() + (unitView_ ? visibleUnitRows() * unitRowHeight + 27 : 70); }
int ReplayLifeTimeline::roundNumberAt(int step) const {
  const int index = model_.roundIndexes.value(step, -1);
  return index >= 0 && index < model_.rounds.size() ? model_.rounds[index].number : -1;
}
int ReplayLifeTimeline::activeRoundIndex() const {
  if (model_.rounds.isEmpty()) return -1;
  return std::clamp(model_.roundIndexes.value(value(), -1), 0, int(model_.rounds.size()) - 1);
}
QToolButton* ReplayLifeTimeline::roundButton(int index) const {
  return index >= 0 && index < roundButtons_.size() ? roundButtons_[index] : nullptr;
}
QString ReplayLifeTimeline::roundCaption(int index) const {
  const int number = model_.rounds[index].number;
  return number >= 0 ? QStringLiteral("第 %1 回合").arg(number) : QStringLiteral("回合未记录");
}
void ReplayLifeTimeline::closeMenus() {
  for (auto* menu : findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly)) menu->close();
}
void ReplayLifeTimeline::setReplay(const BattleReplay::Replay& replay, const QJsonObject& catalog) {
  closeMenus();
  dragging_ = false; setSliderDown(false);
  model_ = ReplayTimeline::buildLifeModel(replay, catalog);
  for (auto* button : roundButtons_) delete button;
  roundButtons_.clear(); lastRound_ = -1;
  int minimumWidth = 0;
  for (int index = 0; index < model_.rounds.size(); ++index) {
    auto* button = new RoundSegmentButton(roundContent_);
    button->setObjectName(QStringLiteral("replayRoundSegment%1").arg(index));
    button->setText(roundCaption(index));
    button->setProperty("roundIndex", index);
    button->setProperty("roundNumber", model_.rounds[index].number);
    button->setMinimumWidth(model_.rounds[index].number >= 0 ? 100 : 126);
    button->setToolTip(QStringLiteral("%1 · %2 个小回合\n点击回到本段第一个小回合开始并暂停。")
      .arg(roundCaption(index)).arg(model_.rounds[index].steps.size()));
    minimumWidth += button->minimumWidth() + (index ? roundLayout_->spacing() : 0);
    roundLayout_->addWidget(button);
    roundButtons_.append(button);
    connect(button, &QToolButton::clicked, this, [this, index] {
      if (index >= model_.rounds.size() || model_.rounds[index].steps.isEmpty()) return;
      emit inspectionStarted();
      emit actionStartRequested(model_.rounds[index].steps.first());
      refreshPosition();
    });
  }
  roundContent_->setMinimumWidth(minimumWidth);
  rebuildUnitOrder();
  unitScroll_->setValue(0);
  setProperty("lifeEventCount", model_.markers.size());
  setProperty("smallTurnCount", model_.actionSteps.size());
  setProperty("roundCount", model_.rounds.size());
  updateGeometryAndScroll();
  refreshPosition();
}
void ReplayLifeTimeline::rebuildUnitOrder() {
  unitOrder_.clear(); unitPositions_.clear();
  for (int index = 0; index < model_.units.size(); ++index) unitOrder_.append(index);
  std::sort(unitOrder_.begin(), unitOrder_.end(), [this](int lhs, int rhs) {
    const auto& a = model_.units[lhs]; const auto& b = model_.units[rhs];
    return std::make_tuple(a.team != ownTeam_, a.team, a.player, a.y, a.x, a.key) <
      std::make_tuple(b.team != ownTeam_, b.team, b.player, b.y, b.x, b.key);
  });
  for (int position = 0; position < unitOrder_.size(); ++position)
    unitPositions_.insert(model_.units[unitOrder_[position]].key, position);
}
QStringList ReplayLifeTimeline::unitKeys() const {
  QStringList keys;
  for (int index : unitOrder_) keys.append(model_.units[index].key);
  return keys;
}
QString ReplayLifeTimeline::unitCaption(int index) const {
  const auto& unit = model_.units[index];
  return (unit.team == ownTeam_ ? QStringLiteral("我方 · ") : QStringLiteral("敌方 · ")) + unit.name;
}
QRect ReplayLifeTimeline::unitRowRect(const QString& key) const {
  if (!unitView_ || !unitPositions_.contains(key)) return {};
  const int row = unitPositions_.value(key) - unitScroll_->value();
  if (row < 0 || row >= visibleUnitRows()) return {};
  return {0, headerHeight() + row * unitRowHeight, std::max(1, width() - 17), unitRowHeight};
}
void ReplayLifeTimeline::setPerspective(int ownTeam) {
  if (ownTeam_ == ownTeam) return;
  closeMenus(); ownTeam_ = ownTeam;
  rebuildUnitOrder(); unitScroll_->setValue(0);
  updateGeometryAndScroll(); update();
}
void ReplayLifeTimeline::setUnitView(bool enabled) {
  if (unitView_ == enabled) return;
  closeMenus(); dragging_ = false; setSliderDown(false);
  unitView_ = enabled; setProperty("unitView", enabled);
  updateGeometryAndScroll(); update();
  emit layoutModeChanged();
}
void ReplayLifeTimeline::refreshPosition() {
  const int current = activeRoundIndex();
  for (int index = 0; index < roundButtons_.size(); ++index) {
    const auto& steps = model_.rounds[index].steps;
    int progress = 0;
    if (!steps.isEmpty() && value() >= steps.first()) {
      if (value() > steps.last()) progress = 1000;
      else if (steps.size() > 1)
        progress = qRound(1000.0 * std::max(0, roundTurnAt(value()) - 1) / (steps.size() - 1));
    }
    static_cast<RoundSegmentButton*>(roundButtons_[index])->setCurrent(index == current, progress);
  }
  if (current != lastRound_) {
    closeMenus(); lastRound_ = current;
    if (auto* button = roundButton(current)) roundStrip_->ensureWidgetVisible(button, 10, 0);
  }
  setProperty("activeRoundIndex", current);
  setAccessibleDescription(current < 0 ? QStringLiteral("未载入战报") :
    QStringLiteral("%1 · 第 %2 小回合").arg(roundCaption(current)).arg(roundTurnAt(value())));
  update();
}
void ReplayLifeTimeline::updateGeometryAndScroll() {
  const int header = headerHeight(), rows = visibleUnitRows();
  roundStrip_->setGeometry(0, 0, width(), header - 4);
  {
    const QSignalBlocker blocker(unitScroll_);
    unitScroll_->setRange(0, std::max(0, int(unitOrder_.size()) - rows));
    unitScroll_->setPageStep(rows);
  }
  unitScroll_->setGeometry(width() - 14, header, 14, rows * unitRowHeight);
  unitScroll_->setVisible(unitView_ && unitScroll_->maximum() > 0);
  if (height() != totalHeight()) setFixedHeight(totalHeight());
  updateGeometry();
}
void ReplayLifeTimeline::resizeEvent(QResizeEvent* event) {
  QSlider::resizeEvent(event);
  updateGeometryAndScroll();
  if (auto* button = roundButton(activeRoundIndex())) roundStrip_->ensureWidgetVisible(button, 10, 0);
}
QRectF ReplayLifeTimeline::track() const {
  const int left = unitView_ ? std::clamp(width() / 4, 112, 262) : 56;
  const int top = headerHeight() + (unitView_ ? visibleUnitRows() * unitRowHeight + 10 : 32);
  return {qreal(left), qreal(top), qreal(std::max(1, width() - left - 22)), 6};
}
qreal ReplayLifeTimeline::xForStep(int step) const {
  const auto area = track();
  const int round = activeRoundIndex();
  if (round < 0) return area.left();
  const auto& steps = model_.rounds[round].steps;
  if (steps.isEmpty() || step <= steps.first()) return area.left();
  if (step >= steps.last()) return area.right();
  const auto after = std::upper_bound(steps.cbegin(), steps.cend(), step);
  const int index = int(std::distance(steps.cbegin(), after)) - 1;
  return area.left() + area.width() * index / std::max(1, int(steps.size()) - 1);
}
int ReplayLifeTimeline::stepForX(qreal x) const {
  const auto area = track();
  const int round = activeRoundIndex();
  if (round < 0 || model_.rounds[round].steps.isEmpty()) return value();
  const auto& steps = model_.rounds[round].steps;
  return steps[qRound(std::clamp((x - area.left()) / area.width(), 0.0, 1.0) * (steps.size() - 1))];
}
QList<ReplayLifeTimeline::Group> ReplayLifeTimeline::groups() const {
  const auto area = track();
  const int count = std::max(1, int(area.width() / 28));
  const qreal span = area.width() / count;
  const int currentRound = activeRoundIndex();
  QMap<int, Group> buckets;
  for (int index = 0; index < model_.markers.size(); ++index) {
    const auto& marker = model_.markers[index];
    if (marker.roundIndex != currentRound) continue;
    int lane = marker.team == ownTeam_ ? 0 : 1;
    int top = headerHeight() + (lane == 0 ? 3 : 45);
    if (unitView_) {
      const auto row = unitRowRect(marker.key);
      if (row.isEmpty()) continue;
      lane = unitPositions_.value(marker.key) - unitScroll_->value();
      top = row.top() + 2;
    }
    const int slot = std::clamp(int((xForStep(marker.step) - area.left()) / span), 0, count - 1);
    auto& group = buckets[lane * count + slot];
    group.lane = lane; group.indices.append(index);
    group.rect = QRect(qRound(area.left() + (slot + 0.5) * span) - 12, top, 24, 22);
  }
  return buckets.values();
}
QRect ReplayLifeTimeline::markerRect(int marker) const {
  for (const auto& group : groups()) if (group.indices.contains(marker)) return group.rect;
  return {};
}
QString ReplayLifeTimeline::description(const Marker& marker) const {
  return (marker.team == ownTeam_ ? QStringLiteral("我方") : QStringLiteral("敌方")) + QStringLiteral(" · ") +
    (marker.round >= 0 ? QStringLiteral("第 %1 回合").arg(marker.round) : QStringLiteral("回合未记录")) +
    QStringLiteral(" · 第 %1 小回合 · %2 · %3").arg(marker.roundTurn).arg(marker.name, marker.label);
}
QString ReplayLifeTimeline::groupCaption(const Group& group) const {
  QStringList glyphs;
  for (int index : group.indices) {
    const auto& marker = model_.markers[index];
    const auto glyph = marker.label == QStringLiteral("死亡") ? QStringLiteral("×") :
      marker.label == QStringLiteral("复活") ? QStringLiteral("+") :
      marker.recovery ? QStringLiteral("↑") : QStringLiteral("0");
    if (!glyphs.contains(glyph)) glyphs.append(glyph);
  }
  return glyphs.join(QString());
}
QString ReplayLifeTimeline::groupDescription(const Group& group) const {
  QStringList lines;
  for (int index : group.indices) {
    lines.append(description(model_.markers[index]));
    if (lines.size() == 12 && group.indices.size() > 12) {
      lines.append(QStringLiteral("共 %1 条记录，点击展开选择。").arg(group.indices.size())); break;
    }
  }
  if (group.indices.size() == 1) lines.append(model_.markers[group.indices.first()].note);
  lines.append(QStringLiteral("点击跳到对应小回合开始并暂停。"));
  return lines.join(QLatin1Char('\n'));
}
void ReplayLifeTimeline::paintEvent(QPaintEvent*) {
  QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
  QFont textFont = font(); textFont.setPixelSize(11); painter.setFont(textFont);
  const auto area = track(); const qreal cursor = xForStep(value());
  if (unitView_) {
    for (int position = unitScroll_->value(); position < std::min(int(unitOrder_.size()), unitScroll_->value() + visibleUnitRows()); ++position) {
      const int index = unitOrder_[position]; const auto& unit = model_.units[index]; const auto row = unitRowRect(unit.key);
      const bool own = unit.team == ownTeam_;
      painter.fillRect(row.adjusted(0, 0, 0, -1), QColor(own ? "#f0f5fc" : "#fbf1f4"));
      painter.setPen(QColor(own ? "#246bb0" : "#b4425c"));
      painter.drawText(row.adjusted(6, 0, -(row.width() - int(area.left()) + 10), 0), Qt::AlignVCenter | Qt::AlignLeft,
        painter.fontMetrics().elidedText(unitCaption(index), Qt::ElideRight, int(area.left()) - 16));
      painter.setPen(QPen(QColor("#dce3ed"), 1));
      painter.drawLine(QPointF(area.left(), row.center().y()), QPointF(area.right(), row.center().y()));
    }
    painter.setPen(QPen(QColor("#3978ce"), 1, Qt::DashLine));
    painter.drawLine(QPointF(cursor, headerHeight()), QPointF(cursor, area.top()));
  } else {
    painter.setPen(QColor("#246bb0"));
    painter.drawText(QRect(0, headerHeight() + 3, 49, 22), Qt::AlignCenter, QStringLiteral("我方"));
    painter.setPen(QColor("#b4425c"));
    painter.drawText(QRect(0, headerHeight() + 45, 49, 22), Qt::AlignCenter, QStringLiteral("敌方"));
    painter.setPen(QPen(QColor("#e5e9f0"), 1));
    painter.drawLine(QPointF(area.left(), headerHeight() + 14), QPointF(area.right(), headerHeight() + 14));
    painter.drawLine(QPointF(area.left(), headerHeight() + 56), QPointF(area.right(), headerHeight() + 56));
  }
  painter.setPen(Qt::NoPen); painter.setBrush(QColor("#dce3ed")); painter.drawRoundedRect(area, 3, 3);
  painter.setBrush(QColor("#3978ce"));
  painter.drawRoundedRect(QRectF(area.left(), area.top(), cursor - area.left(), area.height()), 3, 3);
  const auto visibleGroups = groups();
  for (const auto& group : visibleGroups) {
    bool death = false, recovery = false;
    for (int index : group.indices) { death |= !model_.markers[index].recovery; recovery |= model_.markers[index].recovery; }
    const bool current = std::any_of(group.indices.cbegin(), group.indices.cend(),
      [this](int index) { return model_.markers[index].step == value(); });
    const QColor color = death && recovery ? QColor("#675282") : recovery ? QColor("#268559") : QColor("#b23e51");
    painter.setBrush(current ? QColor("#e7effe") : death && recovery ? QColor("#f2eef8") : recovery ? QColor("#eaf6ef") : QColor("#fff0f2"));
    painter.setPen(QPen(current ? QColor("#3978ce") : color.lighter(155), current ? 2 : 1));
    painter.drawRoundedRect(group.rect, 4, 4); painter.setPen(color);
    QFont glyphFont = textFont; glyphFont.setBold(true);
    const auto caption = groupCaption(group);
    glyphFont.setPixelSize(caption.size() > 2 ? 10 : 15); painter.setFont(glyphFont);
    painter.drawText(group.rect, Qt::AlignCenter, caption);
  }
  painter.setFont(textFont);
  if (!unitView_ && visibleGroups.isEmpty()) {
    painter.setPen(QColor("#748298"));
    painter.drawText(QRectF(area.left(), headerHeight() + 3, area.width(), 22), Qt::AlignCenter,
      model_.actionSteps.isEmpty() ? QStringLiteral("导入战报后显示死亡 × / 复活 +") : QStringLiteral("本回合未记录精灵死亡或复活"));
  }
  painter.setPen(QPen(QColor("#ffffff"), 2)); painter.setBrush(QColor("#3978ce"));
  painter.drawEllipse(QPointF(cursor, area.center().y()), 6, 6);
  if (hasFocus()) {
    painter.setBrush(Qt::NoBrush); painter.setPen(QPen(QColor("#3978ce"), 1, Qt::DotLine));
    painter.drawRoundedRect(rect().adjusted(1, headerHeight(), -2, -2), 4, 4);
  }
}
void ReplayLifeTimeline::activate(const Group& group) {
  emit inspectionStarted();
  const int step = model_.markers[group.indices.first()].step;
  if (group.indices.size() <= 12 && std::all_of(group.indices.cbegin(), group.indices.cend(),
      [this, step](int index) { return model_.markers[index].step == step; })) {
    emit actionStartRequested(step); return;
  }
  auto* menu = new QMenu(this); menu->setObjectName(QStringLiteral("replayLifeEventMenu"));
  menu->setAttribute(Qt::WA_DeleteOnClose);
  for (int index : group.indices) {
    const auto marker = model_.markers[index];
    auto* action = menu->addAction(description(marker).replace(QLatin1Char('&'), QStringLiteral("&&")));
    action->setData(marker.step);
    action->setToolTip(marker.note + QStringLiteral("\n全场第 %1 次行动").arg(marker.turn));
    connect(action, &QAction::triggered, this, [this, marker] { emit actionStartRequested(marker.step); });
  }
  menu->setToolTipsVisible(true);
  menu->popup(mapToGlobal(group.rect.bottomLeft()));
}
void ReplayLifeTimeline::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) { event->ignore(); return; }
  setFocus(Qt::MouseFocusReason);
  for (const auto& group : groups()) if (group.rect.contains(event->position().toPoint())) {
    activate(group); event->accept(); return;
  }
  if (std::abs(event->position().y() - track().center().y()) > 11 || activeRoundIndex() < 0) { event->ignore(); return; }
  dragging_ = true; setSliderDown(true); emit inspectionStarted();
  const int destination = stepForX(event->position().x());
  if (destination == value()) emit actionStartRequested(destination); else setValue(destination);
  event->accept();
}
void ReplayLifeTimeline::mouseMoveEvent(QMouseEvent* event) {
  if (dragging_) { setValue(stepForX(event->position().x())); event->accept(); return; }
  bool onMarker = false;
  for (const auto& group : groups()) onMarker |= group.rect.contains(event->position().toPoint());
  setCursor(onMarker ? Qt::PointingHandCursor : Qt::ArrowCursor);
  event->accept();
}
void ReplayLifeTimeline::mouseReleaseEvent(QMouseEvent* event) {
  if (dragging_ && event->button() == Qt::LeftButton) { dragging_ = false; setSliderDown(false); event->accept(); return; }
  event->ignore();
}
void ReplayLifeTimeline::wheelEvent(QWheelEvent* event) {
  if (unitView_ && unitScroll_->maximum() > 0) {
    const int delta = event->angleDelta().y();
    unitScroll_->setValue(unitScroll_->value() - (delta > 0 ? 1 : delta < 0 ? -1 : 0) * 3);
    event->accept(); return;
  }
  event->ignore();
}
void ReplayLifeTimeline::keyPressEvent(QKeyEvent* event) {
  const int round = activeRoundIndex();
  if (round < 0 || model_.actionSteps.isEmpty()) { QSlider::keyPressEvent(event); return; }
  int step = value();
  if (event->key() == Qt::Key_Home) step = model_.rounds[round].steps.first();
  else if (event->key() == Qt::Key_End) step = model_.rounds[round].steps.last();
  else if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Up) {
    const auto next = std::upper_bound(model_.actionSteps.cbegin(), model_.actionSteps.cend(), value());
    step = next == model_.actionSteps.cend() ? model_.actionSteps.last() : *next;
  } else if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Down) {
    const auto next = std::lower_bound(model_.actionSteps.cbegin(), model_.actionSteps.cend(), value());
    step = next == model_.actionSteps.cbegin() ? model_.actionSteps.first() : *(next - 1);
  } else { QSlider::keyPressEvent(event); return; }
  emit inspectionStarted(); emit actionStartRequested(step); event->accept();
}
bool ReplayLifeTimeline::event(QEvent* event) {
  if (event->type() == QEvent::ToolTip) {
    auto* help = static_cast<QHelpEvent*>(event);
    for (const auto& group : groups()) if (group.rect.contains(help->pos())) {
      QToolTip::showText(help->globalPos(), QStringLiteral("<qt>") +
        groupDescription(group).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")) + QStringLiteral("</qt>"), this, group.rect);
      return true;
    }
    if (unitView_) for (int index : unitOrder_) {
      const auto& unit = model_.units[index]; const auto row = unitRowRect(unit.key);
      if (!row.isEmpty() && row.contains(help->pos())) {
        QToolTip::showText(help->globalPos(), QStringLiteral("<qt>%1<br>玩家：%2<br>空白处表示本回合没有记录死亡或复活事件。</qt>")
          .arg(unitCaption(index).toHtmlEscaped(), unit.player.toHtmlEscaped()), this, row);
        return true;
      }
    }
  }
  return QSlider::event(event);
}
