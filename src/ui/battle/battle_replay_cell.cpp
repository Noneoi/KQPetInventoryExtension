#include "ui/battle/battle_replay_cell.h"
#include "ui/battle/battle_replay_numbers.h"
#include "ui/battle/battle_replay_buff_presentation.h"
#include "ui/battle/battle_replay_identity_presentation.h"
#include "ui/battle/battle_replay_reference_library.h"

#include <QAction>
#include <QEasingCurve>
#include <QHelpEvent>
#include <QGraphicsEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QPointer>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextOption>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <algorithm>
#include <cmath>
#include <functional>

namespace {
QString digits(std::optional<qint64> n) { return n ? QString::number(*n) : QStringLiteral("?"); }
QString publicNumberDetail(const ReplayUnitEvent& event) {
  QString text = event.metric + QLatin1Char(' ') + event.valueText;
  if (event.observedBefore && event.observedAfter)
    text += QStringLiteral("\n已记录 %1 → %2").arg(*event.observedBefore).arg(*event.observedAfter);
  if (!event.harmLabel.isEmpty()) text += QStringLiteral("\n伤害来源：") + event.harmLabel;
  if (event.critical) text += QStringLiteral(" · 暴击");
  return text;
}
QString scrollStyle() {
  return QStringLiteral("QScrollBar:vertical{width:5px;background:transparent;margin:0} QScrollBar::handle:vertical{background:#c8d1de;min-height:14px;border-radius:2px} QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0} QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent}");
}

// Action mode leaves the real balance inside its bar and exposes ordered
// changes to the board callouts; the bar retains their full inspectable list.
// Independent phase previews keep the original compact in-bar change row.
class ValueBar final : public QProgressBar {
public:
  explicit ValueBar(QWidget* parent) : QProgressBar(parent) {
    setProperty("observedSequence", QStringList());
    setProperty("hasActionMeterContext", false); setProperty("actionMetersPending", false);
    setProperty("actionGhostSegments", QVariantList());
  }
  QList<ReplayUnitEvent> events;
  std::function<void(int)> activate;
  std::function<void(int, int)> activateRecorded;
  std::function<void()> diagnose;
  void setMeterColors(const QColor& fill, const QColor& background) {
    fill_ = fill; background_ = background;
  }
  void setRecordedBalance(std::optional<qint64> current, std::optional<qint64> reference) {
    recordedCurrent_ = current; recordedReference_ = reference;
  }
  void setNumberText(const QString& text) {
    numberText_ = text; setFormat(ReplayNumbers::display(numberText_, groupedNumbers_));
  }
  void setGroupedNumbers(bool enabled) {
    groupedNumbers_ = enabled; setFormat(ReplayNumbers::display(numberText_, enabled)); update();
  }
  void setCompact(bool compact, bool crowded = false) {
    const int wanted = events.isEmpty() || actionContext_ ? (compact ? 16 : 17) : (crowded ? 20 : compact ? 22 : 26);
    if (height() == wanted && compact_ == compact) return;
    compact_ = compact;
    setStyleSheet(styleSheet() + QStringLiteral(" QProgressBar{min-height:%1px;max-height:%1px}").arg(wanted));
    setFixedHeight(wanted); update();
  }
  void setMarker(const QString& marker, const QString& explanation) {
    marker_ = marker;
    setTextVisible(events.isEmpty() && marker_.isEmpty());
    if (!marker_.isEmpty()) {
      setAccessibleName(accessibleName() + QStringLiteral(" · ") + explanation);
      setToolTip(toolTip() + QLatin1Char('\n') + explanation);
    }
    update();
  }
  void setDiagnostic(bool unknown, std::function<void()> callback) {
    unknown_ = unknown; diagnose = std::move(callback);
    setProperty("diagnosticAvailable", unknown);
    setTextVisible(events.isEmpty() && marker_.isEmpty() && !unknown_);
    if (unknown_) {
      setCursor(Qt::PointingHandCursor);
      setAccessibleName(accessibleName() + QStringLiteral("；点击 ? 查看未知字段与依据"));
      setToolTip(toolTip() + QStringLiteral("\n点击本条的 ? 查看未知字段与依据。"));
    }
    update();
  }
  void animateFrom(std::optional<qint64> before, std::optional<qint64> reference, int duration,
                   bool playbackDriven, qreal playbackProgress) {
    if (actionContext_) return;
    if (!before || !reference || *reference <= 0 || duration <= 0) return;
    transitionStart_ = int(std::clamp(double(*before) / double(*reference), 0.0, 1.0) * 10000);
    transitionEnd_ = value();
    observedWidths_.clear(); setProperty("observedSequence", QStringList());
    // Only a fully observed, contiguous chain can describe intermediate
    // balances. A missing link, changed maximum, or mixed-in maximum-health
    // record leaves the original endpoint animation in place.
    bool continuous = !events.isEmpty() && recordedCurrent_ && recordedReference_ == reference;
    QList<qint64> observations{*before};
    qint64 previous = *before;
    if (continuous) for (const auto& event : events) {
      if (!event.observedBefore || !event.observedAfter || *event.observedBefore != previous) {
        continuous = false; break;
      }
      previous = *event.observedAfter; observations.append(previous);
    }
    if (continuous && previous == *recordedCurrent_) {
      QStringList sequence;
      for (const auto observed : observations) {
        sequence.append(QString::number(observed));
        observedWidths_.append(int(std::clamp(double(observed) / double(*reference), 0.0, 1.0) * 10000));
      }
      // Resolve to the exact existing meter endpoint, rather than maintaining
      // a second interpretation of the domain's final balance.
      observedWidths_.last() = transitionEnd_;
      setProperty("observedSequence", sequence);
      setToolTip(toolTip() + QStringLiteral("\n已记录数值顺序：") + sequence.join(QStringLiteral(" → ")) +
        QStringLiteral("\n条宽按记录顺序变化并短暂停留；这是阅读节奏，不代表原版动画帧数或战斗时刻。"));
    }
    hasTransition_ = transitionStart_ != transitionEnd_;
    if (!observedWidths_.isEmpty())
      hasTransition_ = std::any_of(observedWidths_.cbegin(), observedWidths_.cend(),
        [&](int width) { return width != observedWidths_.first(); });
    if (!hasTransition_) return;
    // The progress width may interpolate; its format remains the exact logged
    // balance. Never display an invented intermediate health or vigor number.
    if (playbackDriven) { setPlaybackProgress(playbackProgress); return; }
    animation_ = new QVariantAnimation(this);
    animation_->setObjectName(QStringLiteral("replayMeterAnimation"));
    animation_->setDuration(duration); animation_->setStartValue(0.0); animation_->setEndValue(1.0);
    connect(animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) { applyProgress(value.toReal()); });
    applyProgress(0); animation_->start();
  }
  void setPlaybackProgress(qreal progress) {
    if (animation_) animation_->stop();
    applyProgress(progress);
  }
  void setEvents(const QList<ReplayUnitEvent>& all, const QString& metric) {
    for (const auto& event : all) if (event.metric == metric) events.append(event);
    if (events.isEmpty()) return;
    setStyleSheet(styleSheet() + QStringLiteral(" QProgressBar{min-height:26px;max-height:26px}"));
    setFixedHeight(26); setTextVisible(false); setCursor(Qt::PointingHandCursor);
    QStringList labels, details; QVariantList evidence;
    for (const auto& event : events) {
      labels.append(eventText(event));
      details.append(publicNumberDetail(event));
      evidence.append(QVariantMap{{"eventIndex", event.index}, {"recordStep", event.recordStep},
        {"buffId", event.buffId}, {"detail", event.detail}});
    }
    setAccessibleName(accessibleName() + QStringLiteral("\n变化：") + labels.join(QStringLiteral("；")));
    setProperty("rawEvidence", evidence);
    setToolTip(toolTip() + QStringLiteral("\n按日志顺序，点击本条查看完整记录：\n") + details.join(QLatin1Char('\n')));
  }
  void setActionMeter(bool pending, std::optional<qint64> before,
                      std::optional<qint64> beforeReference, const QString& metric,
                      const QList<ReplayUnitEvent>& active) {
    actionContext_ = true;
    actionSegments_.clear();
    if (animation_) animation_->stop();
    hasTransition_ = false; observedWidths_.clear();
    setProperty("observedSequence", QStringList());
    setProperty("hasActionMeterContext", true); setProperty("actionMetersPending", pending);
    setProperty("actionMeterEventCount", events.size());
    setProperty("activeMeterEventCount", active.size());
    // The display scale may include previously recorded balances. In
    // particular, a shield has no invented capacity and vigor can exceed its
    // official full threshold. All printed values keep the actual denominator.
    qint64 largest = 0;
    const auto include = [&](std::optional<qint64> number) { if (number && *number > largest) largest = *number; };
    include(recordedCurrent_); include(recordedReference_); include(before); include(beforeReference);
    for (const auto& cue : events) { include(cue.observedBefore); include(cue.observedAfter); }
    actionReference_ = largest > 0 ? std::optional<qint64>(largest) : std::nullopt;
    setProperty("actionMeterReference", actionReference_ ? QVariant::fromValue(*actionReference_) : QVariant());
    setValue(int(std::lround(actionRatio(recordedCurrent_.value_or(0)) * 10000)));
    QVariantList segments;
    if (pending && actionReference_) for (const auto& cue : active) {
      // No missing observation is reconstructed from the endpoint or the
      // signed text. Such a record still appears in the numeric callout/menu.
      if (!cue.observedBefore || !cue.observedAfter || *cue.observedBefore == *cue.observedAfter) continue;
      const bool addition = *cue.observedAfter > *cue.observedBefore;
      const auto lower = std::min(*cue.observedBefore, *cue.observedAfter);
      const auto upper = std::max(*cue.observedBefore, *cue.observedAfter);
      const qreal from = actionRatio(lower), to = actionRatio(upper);
      if (to <= from) continue;
      actionSegments_.append({from, to, addition});
      segments.append(QVariantMap{{QStringLiteral("before"), QVariant::fromValue(*cue.observedBefore)},
        {QStringLiteral("after"), QVariant::fromValue(*cue.observedAfter)},
        {QStringLiteral("kind"), addition ? QStringLiteral("add") : QStringLiteral("remove")},
        {QStringLiteral("eventIndex"), cue.index}, {QStringLiteral("recordStep"), cue.recordStep},
        {QStringLiteral("from"), from}, {QStringLiteral("to"), to}});
    }
    setProperty("actionGhostSegments", segments);
    QString guidance = QStringLiteral("虚色只标出本条最近一次已确认变化：增加用浅色加号边框，减少用斜纹；本条下次变化、切换施放来源或行动结束时收尾。条内数字始终是当前这一段的已记录余额，虚色不表示尚未生效。点击可查看本次行动至今的全部记录。");
    if (metric == QStringLiteral("护盾")) guidance += QStringLiteral("\n护盾没有已知上限；条宽仅以本次已记录余额范围作为显示参考，不表示护盾上限。");
    else if (metric == QStringLiteral("气势")) guidance += QStringLiteral("\n条宽使用官方满气势值与本次已记录余额范围中的较大值；分母仍是官方满气势值，不把超出部分封顶。");
    else if (actionReference_ && recordedReference_ && *actionReference_ > *recordedReference_)
      guidance += QStringLiteral("\n本次生命上限或已记录余额发生变化，条宽使用已记录范围；分母仍显示当前生命上限。");
    setToolTip(toolTip() + QLatin1Char('\n') + guidance);
    setAccessibleName(accessibleName() + QLatin1Char('\n') + (pending ? QStringLiteral("本段变化痕迹；") : QStringLiteral("行动已结束；")) + guidance);
    setTextVisible(false); setCompact(compact_); update();
  }
protected:
  void paintEvent(QPaintEvent* event) override {
    if (!actionContext_) QProgressBar::paintEvent(event);
    if (!actionContext_ && events.isEmpty() && marker_.isEmpty() && !unknown_) return;
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    if (actionContext_) paintActionMeter(painter);
    auto font = painter.font(); font.setPixelSize(compact_ ? 10 : 11); font.setWeight(QFont::DemiBold); painter.setFont(font);
    const int balanceHeight = events.isEmpty() || actionContext_ ? height() : height() / 2;
    const int deltaHeight = height() - balanceHeight;
    const int markerWidth = unknown_ ? 16 : marker_.isEmpty() ? 0 : QFontMetrics(font).horizontalAdvance(marker_) + 7;
    const QRect balance(3, 0, std::max(0, width() - 6 - markerWidth), balanceHeight);
    // Retain exact large balances, shrinking only the font when necessary.
    while (font.pixelSize() > 8 && QFontMetrics(font).horizontalAdvance(text()) > balance.width()) {
      font.setPixelSize(font.pixelSize() - 1); painter.setFont(font);
    }
    painter.setPen(QColor("#23384f")); painter.drawText(balance, Qt::AlignCenter, text());
    if (!marker_.isEmpty() || unknown_) {
      const QRect badge(width() - markerWidth - 2, 1, markerWidth, balanceHeight - 2);
      font.setPixelSize(10); painter.setFont(font);
      painter.setPen(Qt::NoPen); painter.setBrush(QColor(unknown_ ? "#526984" : "#8b5419")); painter.drawRoundedRect(badge, 2, 2);
      painter.setPen(Qt::white); painter.drawText(badge, Qt::AlignCenter, unknown_ ? QStringLiteral("?") : marker_);
    }
    hitAreas_.clear();
    if (events.isEmpty() || actionContext_) return;
    int start = 0;
    for (int i = 0; i < events.size(); ++i) if (events[i].selected) start = i;
    if (start > 0) { hitAreas_.append({QRect(2, balanceHeight, 15, deltaHeight), -1}); painter.drawText(hitAreas_.last().first, Qt::AlignCenter, QStringLiteral("…")); }
    int x = start ? 18 : 3;
    for (int i = start; i < events.size(); ++i) {
      const auto& cue = events[i];
      font.setPixelSize(compact_ ? (cue.selected ? 11 : 10) : (cue.selected ? 12 : 11)); font.setWeight(QFont::Bold); painter.setFont(font);
      const auto text = eventText(cue);
      const int w = QFontMetrics(font).horizontalAdvance(text) + 8;
      const int reserve = i + 1 < events.size() ? 30 : 2;
      if (x + w + reserve > width() && i > start) {
        const QRect rest(x, balanceHeight, width() - x - 2, deltaHeight);
        hitAreas_.append({rest, -1}); painter.setPen(QColor("#314b69"));
        painter.drawText(rest, Qt::AlignCenter, QStringLiteral("…%1").arg(events.size() - i)); break;
      }
      const QRect chip(x, balanceHeight, std::min(w, width() - x - 2), deltaHeight);
      painter.setPen(Qt::NoPen); painter.setBrush(QColor(255, 255, 255, cue.selected ? 245 : 205));
      painter.drawRoundedRect(chip.adjusted(0, 0, 0, -1), 3, 3);
      painter.setPen(QColor(cue.color));
      // Extremely long qint64 values remain available exactly in the tooltip.
      painter.drawText(chip.adjusted(3, 0, -2, 0), Qt::AlignCenter, QFontMetrics(font).elidedText(text, Qt::ElideRight, chip.width() - 5));
      hitAreas_.append({chip, cue.index}); x += w + 3;
    }
    for (const auto& cue : events) if (cue.selected) {
      painter.setPen(QPen(QColor(cue.color), 1.5)); painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(QRectF(rect()).adjusted(.75, .75, -.75, -.75), 3, 3); break;
    }
  }
  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && unknown_ &&
        (actionContext_ ? event->position().x() >= width() - 20 : events.isEmpty() || event->position().y() < height() / 2)) {
      if (diagnose) diagnose(); event->accept(); return;
    }
    if (event->button() == Qt::LeftButton && !events.isEmpty()) {
      for (const auto& hit : hitAreas_) if (hit.first.contains(event->position().toPoint()) && hit.second >= 0) {
        if (activate) activate(hit.second); event->accept(); return;
      }
      if (events.size() == 1 && !actionContext_) { if (activate) activate(events.first().index); }
      else {
        auto* menu = new QMenu(this); menu->setObjectName(QStringLiteral("replayMeterEventMenu"));
        menu->setAttribute(Qt::WA_DeleteOnClose); menu->setToolTipsVisible(true);
        for (const auto& cue : events) {
          auto* item = menu->addAction(publicNumberDetail(cue).replace(QLatin1Char('\n'), QStringLiteral(" · ")));
          item->setToolTip(QStringLiteral("点击查看这次数值变化及原始详情。")); item->setProperty("rawEvidence", cue.detail);
          item->setData(cue.index); item->setProperty("recordStep", cue.recordStep);
          connect(item, &QAction::triggered, this, [this, index = cue.index, step = cue.recordStep] {
            if (actionContext_ && step >= 0 && activateRecorded) activateRecorded(step, index);
            else if (activate) activate(index);
          });
        }
        menu->popup(mapToGlobal(QPoint(0, height())));
      }
      event->accept(); return;
    }
    QProgressBar::mouseReleaseEvent(event);
  }
private:
  struct PendingSegment { qreal from = 0, to = 0; bool addition = false; };
  qreal actionRatio(qint64 amount) const {
    return actionReference_ ? std::clamp(qreal(amount) / qreal(*actionReference_), qreal(0), qreal(1)) : 0;
  }
  void paintActionMeter(QPainter& painter) const {
    const QRectF bounds = QRectF(rect()).adjusted(.5, .5, -.5, -.5);
    QPainterPath clip; clip.addRoundedRect(bounds, 3, 3);
    painter.save(); painter.setClipPath(clip); painter.fillRect(bounds, background_);
    const qreal ratio = actionRatio(recordedCurrent_.value_or(0));
    painter.fillRect(QRectF(bounds.left(), bounds.top(), bounds.width() * ratio, bounds.height()), fill_);
    const QColor edge = fill_.darker(180);
    // Only the current meter segment reaches this painter. Proven sequential
    // changes replace each other; entries in an indivisible group stay together.
    for (const auto& segment : actionSegments_) {
      const QRectF band(bounds.left() + bounds.width() * segment.from, bounds.top(),
        bounds.width() * (segment.to - segment.from), bounds.height());
      painter.save(); painter.setClipRect(band, Qt::IntersectClip);
      painter.fillRect(band, background_);
      QColor tint = fill_; tint.setAlpha(segment.addition ? 92 : 66); painter.fillRect(band, tint);
      if (segment.addition) {
        painter.setPen(QPen(edge, 1.1)); painter.setBrush(Qt::NoBrush);
        painter.drawRect(band.adjusted(.6, .6, -.6, -.6));
        if (band.width() >= 9) {
          auto markFont = painter.font(); markFont.setPixelSize(9); markFont.setBold(true); painter.setFont(markFont);
          painter.drawText(QRectF(band.right() - 10, band.top() - 2, 9, 12), Qt::AlignCenter, QStringLiteral("+"));
        }
      } else {
        QColor hatch = edge; hatch.setAlpha(135); painter.setPen(QPen(hatch, 1));
        for (qreal x = band.left() - band.height(); x < band.right() + band.height(); x += 5)
          painter.drawLine(QPointF(x, band.bottom()), QPointF(x + band.height(), band.top()));
        painter.setPen(QPen(edge, .8, Qt::DashLine)); painter.setBrush(Qt::NoBrush);
        painter.drawRect(band.adjusted(.5, .5, -.5, -.5));
      }
      painter.restore();
    }
    painter.restore();
  }
  static QString eventText(const ReplayUnitEvent& event) {
    // Keep the original numeric text first if a narrow card must elide the
    // official harm label. The complete label remains clickable/accessibile.
    return event.valueText + (event.harmLabel.isEmpty() ? QString() : QStringLiteral(" · ") + event.harmLabel) +
      (event.critical ? QStringLiteral(" · 暴击") : QString());
  }
  void applyProgress(qreal progress) {
    if (actionContext_) return;
    if (!hasTransition_) return;
    progress = std::isfinite(progress) ? std::clamp(progress, qreal(0), qreal(1)) : 1;
    if (observedWidths_.size() >= 2) {
      if (progress >= 1) { setValue(transitionEnd_); return; }
      const int segments = static_cast<int>(observedWidths_.size()) - 1;
      const qreal position = progress * segments;
      const int segment = std::min(int(position), segments - 1);
      // Reserve the last 30% of each reading segment for its recorded result.
      // In particular, a known zero remains visible before the known recovery.
      const qreal local = std::clamp((position - segment) / .7, qreal(0), qreal(1));
      const auto eased = QEasingCurve(QEasingCurve::OutCubic).valueForProgress(local);
      setValue(int(std::lround(observedWidths_[segment] +
        (observedWidths_[segment + 1] - observedWidths_[segment]) * eased)));
      return;
    }
    const auto eased = QEasingCurve(QEasingCurve::OutCubic).valueForProgress(progress);
    setValue(int(std::lround(transitionStart_ + (transitionEnd_ - transitionStart_) * eased)));
  }
  QList<QPair<QRect, int>> hitAreas_;
  QVariantAnimation* animation_ = nullptr;
  int transitionStart_ = 0, transitionEnd_ = 0;
  QList<int> observedWidths_;
  std::optional<qint64> recordedCurrent_, recordedReference_;
  std::optional<qint64> actionReference_;
  QList<PendingSegment> actionSegments_;
  QColor fill_, background_;
  QString numberText_;
  bool groupedNumbers_ = false;
  bool actionContext_ = false;
  bool hasTransition_ = false;
  bool compact_ = false;
  bool unknown_ = false;
  QString marker_;
};

class CompactLabel final : public QLabel {
public:
  explicit CompactLabel(QWidget* parent = nullptr) : QLabel(parent) {
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setFixedHeight(14);
  }
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this); painter.setFont(font()); painter.setPen(palette().color(QPalette::WindowText));
    painter.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
      QFontMetrics(font()).elidedText(text(), Qt::ElideRight, width()));
  }
};

// Visual emphasis leaves the fixed formation and controls in place.
// The effect covers the entire cell, including all meters, Buffs and borders.
class CellEmphasis final : public QGraphicsEffect {
public:
  explicit CellEmphasis(QObject* parent) : QGraphicsEffect(parent) {}
  bool actor = false, dead = false, target = false;
  qreal pulse = 0;
  void setState(bool active, bool fallen, bool affected = false) {
    actor = active; dead = fallen; target = affected;
    setEnabled(actor || dead || (target && pulse > 0)); updateBoundingRect(); update();
  }
  void setPulse(qreal value) {
    if (pulse == value) return;
    pulse = value;
    if (!target) return;
    setEnabled(actor || dead || pulse > 0); update();
  }
  QRectF boundingRectFor(const QRectF& rect) const override {
    const qreal dx = actor ? rect.width() * .08 + 6 : 0;
    const qreal dy = actor ? rect.height() * .08 + 6 : 0;
    return rect.adjusted(-dx, -dy, dx, dy);
  }
protected:
  void draw(QPainter* painter) override {
    QPoint offset; auto pixmap = sourcePixmap(Qt::LogicalCoordinates, &offset, QGraphicsEffect::NoPad);
    if (pixmap.isNull()) return;
    if (dead) {
      auto image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
      for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
          // Keep fallen cards clearly dimmed without burying small numeric
          // labels and Buff evidence in a near-black dense card.
          const int gray = qGray(row[x]) * 3 / 4;
          row[x] = qRgba(gray, gray, gray, qAlpha(row[x]));
        }
      }
      pixmap = QPixmap::fromImage(image);
    }
    const auto bounds = sourceBoundingRect(Qt::LogicalCoordinates);
    painter->save(); painter->setRenderHint(QPainter::Antialiasing);
    if (actor) {
      painter->translate(bounds.center()); painter->scale(1.16, 1.16); painter->translate(-bounds.center());
      painter->setBrush(Qt::NoBrush); painter->setPen(QPen(QColor(40, 106, 194, 105), 9));
      painter->drawRoundedRect(bounds.adjusted(1, 1, -1, -1), 5, 5);
    }
    painter->drawPixmap(offset, pixmap);
    if (target && pulse > 0) {
      // A short, shared-clock wash marks an impact without obscuring any
      // numbers. Fallen units keep their dark treatment throughout the pulse.
      painter->setPen(Qt::NoPen);
      painter->setBrush(QColor(241, 177, 74, int(pulse * (dead ? 12 : 29))));
      painter->drawRoundedRect(bounds.adjusted(1, 1, -1, -1), 5, 5);
    }
    painter->restore();
  }
};

ValueBar* meter(QWidget* parent, const QString& kind, const QString& label,
                    std::optional<qint64> current, std::optional<qint64> reference,
                    const QString& fill, const QString& background, const QString& explanation) {
  auto* bar = new ValueBar(parent);
  bar->setRecordedBalance(current, reference);
  bar->setMeterColors(QColor(fill), QColor(background));
  bar->setObjectName(kind); bar->setFixedHeight(17);
  bar->setRange(0, 10000);
  // Saturate the painted width, never the logged value displayed as text.
  const double ratio = current && reference && *reference > 0 ? double(*current) / double(*reference) : 0;
  bar->setValue(int(std::clamp(ratio, 0.0, 1.0) * 10000));
  bar->setNumberText(digits(current) + QLatin1Char('/') + digits(reference));
  bar->setAlignment(Qt::AlignCenter); bar->setTextVisible(true);
  bar->setAccessibleName(label + QLatin1Char(' ') + bar->format());
  bar->setToolTip(bar->accessibleName() + QLatin1Char('\n') + explanation + QStringLiteral("\n数值超出分母时保留真实数值；? 表示尚无可靠依据。"));
  bar->setStyleSheet(QStringLiteral("QProgressBar{min-height:17px;max-height:17px;color:#172b42;background:%1;border:0;border-radius:3px;font-size:11px;font-weight:600;text-align:center} QProgressBar::chunk{background:%2;border-radius:3px}").arg(background, fill));
  return bar;
}

// QPushButton has no wordWrap property. Keep its normal clicked/accessibility
// behavior, and use the same wrapping policy for measuring and drawing text.
class WrappedNameButton final : public QPushButton {
public:
  WrappedNameButton(const QString& text, const QColor& color, QWidget* parent)
      : QPushButton(text, parent), color_(color) {
    setFlat(true); setCursor(Qt::PointingHandCursor);
    auto f = font(); f.setPixelSize(12); f.setWeight(QFont::DemiBold); setFont(f);
    auto policy = QSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setSizePolicy(policy); setFixedHeight(28);
    setAccessibleName(text);
    setStyleSheet(QStringLiteral("QPushButton{padding:0;border:0;background:transparent}"));
  }
  QSize sizeHint() const override { return {148, 28}; }
  QSize minimumSizeHint() const override { return {0, 28}; }
protected:
  void paintEvent(QPaintEvent*) override {
    QStylePainter painter(this);
    QStyleOptionButton style; initStyleOption(&style); style.text.clear();
    painter.drawControl(QStyle::CE_PushButton, style);
    auto f = font(); f.setUnderline(underMouse()); painter.setFont(f); painter.setPen(color_);
    QTextOption option; option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    option.setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    painter.drawText(QRectF(rect().adjusted(2, 2, -2, -2)), text(), option);
  }
private:
  QColor color_;
};

// Every occupant gets an always-visible, stable relation anchor. Selecting a
// detail card never turns a different occupant into the endpoint of its line.
class UnitMiniRow final : public QPushButton {
public:
  UnitMiniRow(const QString& name, QWidget* parent) : QPushButton(name, parent) {
    setObjectName(QStringLiteral("unitMiniRow"));
    setFlat(true); setCheckable(true); setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setStyleSheet(QStringLiteral("QPushButton{border:0;padding:0;background:transparent}"));
  }
  QString presence, role, changes;
  bool actor = false, target = false, dead = false, pending = false;
  qreal pulse = 0;
  QColor outcomeColor;
  std::function<void(QPainter&, const QRectF&)> paintOutcome;
  std::function<void()> inspect;
  QSize minimumSizeHint() const override { return {0, 14}; }
  QSize sizeHint() const override { return {130, 24}; }
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    QColor background = dead ? QColor("#d1d5db") : actor ? QColor("#dbeaff") : target ? QColor("#fff0de") : QColor("#edf2f7");
    painter.setPen(QPen(isChecked() ? QColor("#4d709a") : QColor("#ced7e2"), isChecked() ? 1.3 : .7));
    painter.setBrush(background); painter.drawRoundedRect(QRectF(rect()).adjusted(.6, .6, -.6, -.6), 2, 2);
    if (target && pulse > 0) {
      painter.setPen(Qt::NoPen); painter.setBrush(QColor(238, 170, 74, int(pulse * 35)));
      painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 2, 2);
    }
    if (paintOutcome && outcomeColor.isValid()) {
      auto tint = outcomeColor; tint.setAlpha(18 + int(pulse * 40));
      painter.setPen(Qt::NoPen); painter.setBrush(tint);
      painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 2, 2);
    }
    auto font = painter.font(); font.setPixelSize(10); font.setWeight(QFont::DemiBold); painter.setFont(font);
    const bool twoLines = height() >= 23;
    const int nameHeight = twoLines ? height() / 2 : height();
    const auto shortPresence = presence == QStringLiteral("存活") ? QStringLiteral("活") :
      presence == QStringLiteral("阵亡") ? QStringLiteral("亡") : presence == QStringLiteral("待召唤") ? QStringLiteral("待召") :
      presence == QStringLiteral("待分身") ? QStringLiteral("待分") : presence;
    const auto label = twoLines ? (role.isEmpty() ? presence : role) :
      (role.isEmpty() ? QString() : role + QStringLiteral("·")) + shortPresence;
    const int badgeWidth = std::min(width() / 2, QFontMetrics(font).horizontalAdvance(label) + 4);
    const int gutter = paintOutcome ? (property("effectCount").toInt() > 1 && width() >= 110 ? 29 : 16) : 11;
    const QRect nameRect(gutter, 0, std::max(0, width() - gutter - 4 - badgeWidth), nameHeight);
    painter.setPen(dead ? QColor("#616a74") : pending ? QColor("#778697") : QColor("#254766"));
    painter.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(font).elidedText(text(), Qt::ElideMiddle, nameRect.width()));
    const QRect badge(width() - badgeWidth - 3, 0, badgeWidth, nameHeight);
    painter.setPen(actor ? QColor("#1d5aa0") : target ? QColor("#9c521b") : QColor("#677587"));
    painter.drawText(badge, Qt::AlignRight | Qt::AlignVCenter, QFontMetrics(font).elidedText(label, Qt::ElideRight, badge.width()));
    // Keep the unit's own outcome symbol in the left gutter, clear of its
    // name. Without a visual outcome, retain the action/target relation dot.
    if (paintOutcome) {
      const qreal size = std::min(qreal(13), qreal(nameHeight - 1));
      paintOutcome(painter, QRectF(1, (nameHeight - size) / 2, gutter - 3, size));
    } else {
      painter.setPen(Qt::NoPen); painter.setBrush(actor ? QColor("#286ac2") : target ? QColor("#d58a39") : QColor("#91a0b1"));
      painter.drawEllipse(QPointF(5, nameHeight / 2.0), actor || target ? 2.5 : 1.6, actor || target ? 2.5 : 1.6);
    }
    if (twoLines) {
      font.setPixelSize(9); font.setWeight(QFont::Normal); painter.setFont(font); painter.setPen(QColor("#586a7e"));
      const auto summary = presence + (changes.isEmpty() ? QString() : QStringLiteral(" · ") + changes);
      const QRect summaryRect(11, nameHeight, std::max(0, width() - 14), height() - nameHeight);
      painter.drawText(summaryRect, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(font).elidedText(summary, Qt::ElideRight, summaryRect.width()));
    } else if (!changes.isEmpty() && !paintOutcome) {
      // At the smallest density a dot explicitly marks an additional recorded
      // change; clicking this occupant exposes its own exact meters and events.
      painter.setPen(QColor("#9c521b")); painter.drawText(QRect(0, height() - 9, 10, 9), Qt::AlignCenter, QStringLiteral("•"));
    }
  }
  void mouseDoubleClickEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && inspect) { inspect(); event->accept(); return; }
    QPushButton::mouseDoubleClickEvent(event);
  }
};

QString plainOfficial(QString text) {
  static const QRegularExpression colorStart(QStringLiteral("\\$c\\w?"));
  text.remove(colorStart); text.replace(QStringLiteral("c$"), QString());
  QTextDocument document; document.setHtml(text);
  return document.toPlainText();
}

QString attributeNames(const BattleReplay::Fighter& fighter, const QJsonObject& catalog) {
  if (!fighter.attributesKnown) return QStringLiteral("属性待核实");
  QStringList names;
  for (const auto& id : fighter.attributes) {
    names.append(replayPublicReferenceName(catalog, QStringLiteral("attributes"), id));
  }
  return names.isEmpty() ? QStringLiteral("无属性") : names.join(QLatin1Char('/'));
}

QString skillName(const QJsonObject& catalog, const QString& id) {
  if (id.isEmpty()) return QStringLiteral("待核实");
  return replayPublicReferenceName(catalog, QStringLiteral("skills"), id);
}

QString occupantState(const BattleReplay::Fighter& fighter) {
  if (fighter.playerRemoved) return QStringLiteral("已移除");
  if (!fighter.entryKnown && !fighter.displayLayersRestored) return QStringLiteral("状态未记");
  if (!BattleReplay::presentedOnField(fighter)) return fighter.clone ? QStringLiteral("待分身") : fighter.summoned ? QStringLiteral("待召唤") : QStringLiteral("未登场");
  if (!fighter.hp) return QStringLiteral("生命未记");
  return *fighter.hp > 0 ? QStringLiteral("存活") : QStringLiteral("阵亡");
}

QStringList officialBuffCues(const QJsonObject& config) {
  // These are the exact vocabularies consumed by BattleAvatar.playBuffEffect,
  // getBufferEffect and getArtNum (BufferConsts). They describe official
  // presentation labels, not a guessed beneficial/harmful classification.
  static const QSet<QString> harmSources{
    QStringLiteral("爆炸"), QStringLiteral("烧伤"), QStringLiteral("冻伤"), QStringLiteral("电伤"), QStringLiteral("中毒"),
    QStringLiteral("溅射"), QStringLiteral("轮回之门"), QStringLiteral("魔王气息"), QStringLiteral("魔神气息"),
    QStringLiteral("青龙之怒"), QStringLiteral("天龙之怒"), QStringLiteral("锁链"), QStringLiteral("天火"),
    QStringLiteral("吸血"), QStringLiteral("阵术"), QStringLiteral("血拼"), QStringLiteral("诅咒"), QStringLiteral("反弹"),
    QStringLiteral("痛苦"), QStringLiteral("分身偷袭"), QStringLiteral("分身袭击"), QStringLiteral("奇迹圣门"),
    QStringLiteral("奇迹之门"), QStringLiteral("天堂之门"), QStringLiteral("天国之门"), QStringLiteral("潜能爆发"),
    QStringLiteral("自爆"), QStringLiteral("守护"), QStringLiteral("过载攻击"), QStringLiteral("七宿之怒"),
    QStringLiteral("七宿龙怒"), QStringLiteral("血量上限"), QStringLiteral("复仇"), QStringLiteral("天劫"),
    QStringLiteral("浩劫之门"), QStringLiteral("雷霆惩戒"), QStringLiteral("炎龙之怒"), QStringLiteral("审判之罪"),
    QStringLiteral("女神圣灵"), QStringLiteral("红莲烈心"), QStringLiteral("无烬爆炸"), QStringLiteral("激光"), QStringLiteral("修罗冥狱")};
  auto result = replayBuffStateCues(config);
  const auto harm = config[QStringLiteral("bhs")].toString();
  if (harmSources.contains(harm)) result.append(QStringLiteral("伤害标注·") + harm);
  result.removeDuplicates();
  return result;
}

QString buffId(const QUrl& url) {
  auto plain = url; plain.setFragment({});
  return plain.scheme() == QStringLiteral("buff") ? QUrl::fromPercentEncoding(plain.toEncoded().mid(5)) : QString();
}

class BuffBrowser final : public QTextBrowser {
public:
  explicit BuffBrowser(QWidget* parent) : QTextBrowser(parent) {
    setOpenLinks(false); setOpenExternalLinks(false); setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    document()->setDocumentMargin(1);
    setStyleSheet(QStringLiteral("QTextBrowser{background:#f6f8fc;border:0;border-radius:5px;color:#465a72;font-size:11px}") + scrollStyle());
  }
  QMap<QString, QString> tips;
protected:
  bool viewportEvent(QEvent* event) override {
    if (event->type() == QEvent::ToolTip) {
      auto* help = static_cast<QHelpEvent*>(event);
      const auto anchor = anchorAt(help->pos());
      const auto id = buffId(QUrl(anchor));
      const auto key = tips.contains(anchor) ? anchor : id;
      if (tips.contains(key)) {
        QToolTip::showText(help->globalPos(), tips.value(key), this);
        event->accept(); return true;
      }
      QToolTip::hideText(); event->ignore(); return true;
    }
    return QTextBrowser::viewportEvent(event);
  }
};

QString roleTag(const QString& text, const QString& background, const QString& color) {
  return QStringLiteral("<span style='background-color:%1;color:%2;font-weight:600'>&nbsp;%3&nbsp;</span>")
    .arg(background, color, text.toHtmlEscaped());
}

struct BuffOrigin {
  QString label, background, color, detail;
};
BuffOrigin buffOrigin(const QString& source, const QString& sourceName, int sourceTeam,
                      int ownTeam, const QStringList& paths) {
  Q_UNUSED(paths);
  BuffOrigin result{QStringLiteral("来源未记"), QStringLiteral("#e9edf1"), QStringLiteral("#65717e"), {}};
  const auto side = replayBuffSourceSide(source, sourceTeam, ownTeam);
  const bool known = side != ReplayBuffSourceSide::Unknown;
  if (known) {
    const bool own = side == ReplayBuffSourceSide::Own;
    result.label = own ? QStringLiteral("我方施加") : QStringLiteral("敌方施加");
    result.background = own ? QStringLiteral("#dceaff") : QStringLiteral("#fde1e7");
    result.color = own ? QStringLiteral("#245da7") : QStringLiteral("#af3550");
  }
  result.detail = QStringLiteral("施加者：") + (source.isEmpty() ? QStringLiteral("未记录") :
    sourceName.isEmpty() || sourceName == source ? QStringLiteral("名称未记录") : replayPublicFighterNameText(sourceName));
  if (known) result.detail += QStringLiteral("\n施加方：") + result.label;
  else result.detail += QStringLiteral("\n施加方：未记录；不按 Buff 持有方或本次行动者归属");
  return result;
}

struct BuffGroup {
  BattleReplay::BuffMember member;
  QStringList paths;
  int count = 0;
};
QList<BuffGroup> buffGroups(const BattleReplay::Fighter& fighter) {
  QList<BuffGroup> groups;
  for (const auto& member : BattleReplay::recordedBuffMembers(fighter)) {
    auto it = std::find_if(groups.begin(), groups.end(), [&](const BuffGroup& group) {
      return group.member.id == member.id && group.member.source == member.source && group.member.sourceTeam == member.sourceTeam &&
        (!member.source.isEmpty() || group.member.sourceName == member.sourceName);
    });
    if (it == groups.end()) { groups.append({member, {}, 0}); it = groups.end() - 1; }
    ++it->count;
    if (it->member.sourceName.isEmpty()) it->member.sourceName = member.sourceName;
    if (!member.path.isEmpty() && !it->paths.contains(member.path)) it->paths.append(member.path);
  }
  return groups;
}

int buffSide(const QString& source, int sourceTeam, int ownTeam) {
  return static_cast<int>(replayBuffSourceSide(source, sourceTeam, ownTeam));
}

QString compactBuffName(const QJsonObject& catalog, const QString& id) {
  const auto config = catalog[id].toObject();
  // These reviewed animation entries also carry an actual official message.
  // Use that message verbatim in the cell; keep the internal name in details.
  if ((id == QStringLiteral("2879") || id == QStringLiteral("2880") || id == QStringLiteral("6523")) &&
      !config[QStringLiteral("mess")].toString().trimmed().isEmpty())
    return plainOfficial(replayBuffMessage(config[QStringLiteral("mess")].toString(), QStringLiteral("本精灵")));
  return plainOfficial(replayPublicReferenceName(QJsonObject{{QStringLiteral("buffs"), catalog}}, QStringLiteral("buffs"), id));
}

QString publicBuffEventLabel(const ReplayUnitEvent& event, const QJsonObject& catalog) {
  const auto name = compactBuffName(catalog, event.buffId);
  if (event.restoresDisplayLayers) return QStringLiteral("恢复显示");
  return (event.buffTriggered ? QStringLiteral("触发 · ") : event.removing ? QStringLiteral("移除 · ") : QStringLiteral("获得 · ")) + name;
}

struct BuffEventGroup {
  QList<ReplayUnitEvent> events;
};
QString buffEventLink(const ReplayUnitEvent& event) {
  return event.recordStep >= 0 ? QStringLiteral("round-effect:%1:%2").arg(event.recordStep).arg(event.index)
    : QStringLiteral("effect:%1").arg(event.index);
}
QString buffEventAnchor(const ReplayUnitEvent& event) {
  return event.recordStep >= 0 ? QStringLiteral("round-event%1-%2").arg(event.recordStep).arg(event.index)
    : QStringLiteral("event%1").arg(event.index);
}
QList<BuffEventGroup> buffEventGroups(const QList<ReplayUnitEvent>& events) {
  QList<BuffEventGroup> groups;
  for (const auto& event : events) {
    if (event.buffId.isEmpty()) continue;
    // Numeric events already belong to their bars. Page supplies a separate,
    // explicitly recorded trigger cue when that Buff should also be named.
    if (!event.metric.isEmpty()) continue;
    const auto match = [&](const BuffEventGroup& group) {
      const auto& other = group.events.first();
      return event.buffId == other.buffId && event.buffSource == other.buffSource &&
        event.buffSourceTeam == other.buffSourceTeam && event.buffSourceName == other.buffSourceName &&
        event.buffTriggered == other.buffTriggered && event.removing == other.removing &&
        event.restoresDisplayLayers == other.restoresDisplayLayers && event.label == other.label;
    };
    auto found = std::find_if(groups.begin(), groups.end(), match);
    if (found == groups.end()) groups.append({{event}});
    else found->events.append(event);
  }
  return groups;
}

struct OutcomeVisual {
  QString label, kind;
  QColor color;
  bool derived = false;
  QString source, detail;
  QString applier, applierName;
  int applierTeam = -1, eventIndex = -1;
  QString symbol;
  bool showApplier = false;
  QString buffId;
};

QColor outcomeColor(const QString& kind) {
  static const QMap<QString, QColor> colors{
    {QStringLiteral("block"), QColor("#2277b5")}, {QStringLiteral("immune"), QColor("#8651c2")},
    {QStringLiteral("critical"), QColor("#d34e26")}, {QStringLiteral("miss"), QColor("#347ea0")},
    {QStringLiteral("invalid"), QColor("#b4475c")}, {QStringLiteral("attack-disabled"), QColor("#95676b")},
    {QStringLiteral("counter-marker"), QColor("#986837")}, {QStringLiteral("counter"), QColor("#c47721")},
    {QStringLiteral("fallen"), QColor("#5b6474")}, {QStringLiteral("recovery"), QColor("#22925f")},
    {QStringLiteral("health-limit"), QColor("#258b88")}, {QStringLiteral("clone"), QColor("#7960b1")},
    {QStringLiteral("summon"), QColor("#249060")}, {QStringLiteral("entry"), QColor("#258a91")},
    {QStringLiteral("transform"), QColor("#9659b8")}, {QStringLiteral("attributes"), QColor("#668141")},
    {QStringLiteral("skill-swap"), QColor("#7655b2")}, {QStringLiteral("reveal"), QColor("#ac7d27")},
    {QStringLiteral("empowerment"), QColor("#b44882")}, {QStringLiteral("self-transform"), QColor("#8b59aa")},
    {QStringLiteral("unverified"), QColor("#8b754e")}, {QStringLiteral("affinity-strong"), QColor("#bc7629")},
    {QStringLiteral("affinity-weak"), QColor("#477fb2")}, {QStringLiteral("god-job-attack"), QColor("#9b6923")},
    {QStringLiteral("god-job-defend"), QColor("#6761ae")}, {QStringLiteral("tianqi"), QColor("#aa7026")},
    {QStringLiteral("attribute-up"), QColor("#327f9f")}, {QStringLiteral("attribute-down"), QColor("#a87543")},
    {QStringLiteral("status-sleep"), QColor("#6872b4")}, {QStringLiteral("status-control"), QColor("#a56662")},
    {QStringLiteral("status-bind"), QColor("#8b6f91")}, {QStringLiteral("status-dispel"), QColor("#477b95")},
    {QStringLiteral("status-burn"), QColor("#bd622d")}, {QStringLiteral("status-frost"), QColor("#328cad")},
    {QStringLiteral("status-poison"), QColor("#658b3a")}, {QStringLiteral("status-restraint"), QColor("#8d7450")},
    {QStringLiteral("status-blind"), QColor("#746484")}, {QStringLiteral("status-physical-seal"), QColor("#9d6571")},
    {QStringLiteral("status-stun"), QColor("#b58a25")}, {QStringLiteral("status-paralysis"), QColor("#a08a24")},
    {QStringLiteral("war-soul"), QColor("#a75e39")}, {QStringLiteral("indomitable-will"), QColor("#986b24")},
    {QStringLiteral("mark"), QColor("#9062ad")}, {QStringLiteral("potential"), QColor("#c07526")},
    {QStringLiteral("buff-add"), QColor("#687c8d")},
    {QStringLiteral("buff-remove"), QColor("#697789")}, {QStringLiteral("buff-trigger"), QColor("#9a7045")},
    {QStringLiteral("power-full"), QColor("#bd731b")}, {QStringLiteral("buff-state"), QColor("#7d64a7")},
    {QStringLiteral("harm-source"), QColor("#ad6650")}};
  return colors.value(kind, QColor("#53758c"));
}

OutcomeVisual outcomeVisual(const QString& label, bool derived) {
  OutcomeVisual result{label, derived ? QStringLiteral("client-hint") : QStringLiteral("record"), QColor("#53758c"), derived,
    derived ? QStringLiteral("official-client") : QStringLiteral("recorded"), {}};
  // Legacy badges contain a fixed presentation vocabulary. Do not classify a
  // pet/skill name containing “变身”, “属性”, etc. as a state transition.
  static const QMap<QString, QString> kinds{
    {QStringLiteral("格挡"), QStringLiteral("block")}, {QStringLiteral("免疫"), QStringLiteral("immune")},
    {QStringLiteral("暴击"), QStringLiteral("critical")}, {QStringLiteral("被暴击"), QStringLiteral("critical")},
    {QStringLiteral("未命中"), QStringLiteral("miss")}, {QStringLiteral("闪避"), QStringLiteral("miss")},
    {QStringLiteral("技能未生效"), QStringLiteral("invalid")}, {QStringLiteral("未生效"), QStringLiteral("invalid")},
    {QStringLiteral("无效"), QStringLiteral("invalid")}, {QStringLiteral("反击标记"), QStringLiteral("counter-marker")},
    {QStringLiteral("反击"), QStringLiteral("counter")}, {QStringLiteral("生命归零"), QStringLiteral("fallen")},
    {QStringLiteral("生命恢复"), QStringLiteral("recovery")}, {QStringLiteral("生命上限变化"), QStringLiteral("health-limit")},
    {QStringLiteral("分身登场"), QStringLiteral("clone")}, {QStringLiteral("召唤登场"), QStringLiteral("summon")},
    {QStringLiteral("登场"), QStringLiteral("entry")}, {QStringLiteral("变身"), QStringLiteral("transform")},
    {QStringLiteral("属性变化"), QStringLiteral("attributes")}, {QStringLiteral("技能已换"), QStringLiteral("skill-swap")},
    {QStringLiteral("显现"), QStringLiteral("reveal")}, {QStringLiteral("状态待核实"), QStringLiteral("unverified")}};
  result.kind = kinds.value(label, result.kind);
  if (label.endsWith(QStringLiteral("攻击不能"))) result.kind = QStringLiteral("attack-disabled");
  if (derived) {
    if (label == QStringLiteral("强效") || label.startsWith(QStringLiteral("强效·"))) result.kind = QStringLiteral("affinity-strong");
    else if (label == QStringLiteral("弱效")) result.kind = QStringLiteral("affinity-weak");
    else if (label == QStringLiteral("神职攻击")) result.kind = QStringLiteral("god-job-attack");
    else if (label == QStringLiteral("神职受击")) result.kind = QStringLiteral("god-job-defend");
    else if (label == QStringLiteral("天启")) result.kind = QStringLiteral("tianqi");
  }
  result.color = outcomeColor(result.kind);
  return result;
}

OutcomeVisual typedOutcomeVisual(const ReplayVisualCue& cue) {
  OutcomeVisual result{cue.label, cue.kind, outcomeColor(cue.kind), cue.derived, cue.evidenceType, cue.detail,
    cue.applier, cue.applierName, cue.applierTeam, cue.eventIndex};
  result.showApplier = !cue.applier.isEmpty() || cue.applierTeam >= 0 || cue.kind == QStringLiteral("empowerment");
  return result;
}

std::optional<OutcomeVisual> knownBuffStatus(const QString& id, const QJsonObject& config) {
  struct Status { const char* id; const char* kind; QString name, effect, message; };
  static const QList<Status> statuses{
    {"1", "status-burn", QStringLiteral("燃烧"), QStringLiteral("烧伤"), QStringLiteral("#pet#进入燃烧状态，每回合扣除50点生命")},
    {"2", "status-frost", QStringLiteral("冰冻"), QStringLiteral("冻伤"), QStringLiteral("#pet#进入冰冻状态，每回合扣除50点生命")},
    {"3", "status-poison", QStringLiteral("中毒"), QStringLiteral("中毒"), QStringLiteral("#pet#进入中毒状态，每回合扣除50点生命")},
    {"4", "status-restraint", QStringLiteral("束缚"), QStringLiteral("束缚"), QStringLiteral("#pet#进入束缚状态，#pet#无法进行近身攻击")},
    {"5", "status-blind", QStringLiteral("失明"), QStringLiteral("命中↓↓↓，失明"), QStringLiteral("#pet#进入失明状态，#pet#进行远程攻击必定被闪避")},
    {"6", "status-physical-seal", QStringLiteral("物攻封印"), QStringLiteral("物攻封印"), QStringLiteral("#pet#进入物攻封印状态，#pet#无法使用物攻")},
    {"8", "status-stun", QStringLiteral("晕眩"), QStringLiteral("晕眩"), QStringLiteral("#pet#进入晕眩状态，#pet#无法行动")},
    {"9", "status-paralysis", QStringLiteral("麻痹"), QStringLiteral("麻痹"), QStringLiteral("#pet#进入麻痹状态，#pet#无法行动")}};
  for (const auto& status : statuses) {
    if (id != QLatin1String(status.id) || config[QStringLiteral("did")].toVariant().toString() != id ||
        config[QStringLiteral("nm")].toString() != status.name || config[QStringLiteral("beffect")].toString() != status.effect ||
        plainOfficial(config[QStringLiteral("mess")].toString()) != status.message) continue;
    return OutcomeVisual{status.name, QLatin1String(status.kind), outcomeColor(QLatin1String(status.kind)), true,
      QStringLiteral("buff-config"), QStringLiteral("已核官方 Buff [%1] / did=%1：%2\n%3\n仅标记本阶段进入该状态；不据配置推算未记录的扣血或攻击结果。")
        .arg(id, status.name, status.message)};
  }
  return std::nullopt;
}

QString harmSymbol(const QString& label) {
  // Pictograms distinguish official labels; they do not infer an additional
  // damage event or a cause beyond the numeric event's verified harmLabel.
  static const QMap<QString, QString> symbols{
    {QStringLiteral("烧伤"), QStringLiteral("flame")}, {QStringLiteral("天火"), QStringLiteral("flame")},
    {QStringLiteral("炎龙之怒"), QStringLiteral("flame")}, {QStringLiteral("红莲烈心"), QStringLiteral("flame")},
    {QStringLiteral("冻伤"), QStringLiteral("frost")}, {QStringLiteral("中毒"), QStringLiteral("poison")},
    {QStringLiteral("电伤"), QStringLiteral("lightning")}, {QStringLiteral("雷霆惩戒"), QStringLiteral("lightning")},
    {QStringLiteral("天劫"), QStringLiteral("lightning")}, {QStringLiteral("爆炸"), QStringLiteral("explosion")},
    {QStringLiteral("自爆"), QStringLiteral("explosion")}, {QStringLiteral("无烬爆炸"), QStringLiteral("explosion")},
    {QStringLiteral("溅射"), QStringLiteral("splash")}, {QStringLiteral("锁链"), QStringLiteral("chain")},
    {QStringLiteral("反弹"), QStringLiteral("reflect")}, {QStringLiteral("吸血"), QStringLiteral("drain")},
    {QStringLiteral("诅咒"), QStringLiteral("curse")}, {QStringLiteral("痛苦"), QStringLiteral("curse")},
    {QStringLiteral("分身偷袭"), QStringLiteral("clone")}, {QStringLiteral("分身袭击"), QStringLiteral("clone")},
    {QStringLiteral("潜能爆发"), QStringLiteral("potential")}, {QStringLiteral("血量上限"), QStringLiteral("health-limit")},
    {QStringLiteral("激光"), QStringLiteral("beam")}, {QStringLiteral("守护"), QStringLiteral("guard")},
    {QStringLiteral("轮回之门"), QStringLiteral("portal")}, {QStringLiteral("奇迹圣门"), QStringLiteral("portal")},
    {QStringLiteral("奇迹之门"), QStringLiteral("portal")}, {QStringLiteral("天堂之门"), QStringLiteral("portal")},
    {QStringLiteral("天国之门"), QStringLiteral("portal")}, {QStringLiteral("浩劫之门"), QStringLiteral("portal")}};
  return symbols.value(label, QStringLiteral("named-harm"));
}

QList<OutcomeVisual> phaseBuffVisuals(const QList<ReplayUnitEvent>& events, const QJsonObject& buffCatalog) {
  QList<OutcomeVisual> result;
  for (const auto& event : events) {
    const auto append = [&](OutcomeVisual visual) {
      visual.applier = event.buffSource; visual.applierName = event.buffSourceName;
      visual.applierTeam = event.buffSourceTeam; visual.eventIndex = event.index; visual.showApplier = true;
      visual.buffId = event.buffId;
      if (!event.detail.isEmpty()) visual.detail += (visual.detail.isEmpty() ? QString() : QStringLiteral("\n")) + event.detail;
      result.append(visual);
    };
    // The page supplies harmLabel only on a real numeric event after checking
    // the official bhs vocabulary. A Buff's bhs field alone is not a hit.
    if (!event.metric.isEmpty() && !event.harmLabel.isEmpty()) {
      OutcomeVisual harm{event.harmLabel, QStringLiteral("harm-source"), outcomeColor(QStringLiteral("harm-source")), true,
        QStringLiteral("recorded-harm"), QStringLiteral("已发生的%1数值记录 %2；来源名称由官方 bhs 核对。").arg(event.metric, event.valueText)};
      harm.symbol = harmSymbol(event.harmLabel); append(harm);
    }
    if (event.buffId.isEmpty() || !event.metric.isEmpty()) continue;
    const auto config = buffCatalog[event.buffId].toObject();
    if (event.restoresDisplayLayers) {
      append({QStringLiteral("恢复显示"), QStringLiteral("reveal"), outcomeColor(QStringLiteral("reveal")), true,
        QStringLiteral("client-display-command"), QStringLiteral("官方新增 Buff 的等待分支调用 summonAppear，恢复显示层；不表示生命恢复或新的召唤成功。")});
    }
    // Unknown registrations remain in their own drawer. A recorded numerical
    // trigger is a different role and stays visible even without a description.
    if (!event.buffTriggered && !replayBuffHasExplanation(config)) continue;
    if (event.removing || event.buffTriggered) {
      const auto kind = event.removing ? QStringLiteral("buff-remove") : QStringLiteral("buff-trigger");
      const auto verb = event.removing ? QStringLiteral("移除") : QStringLiteral("触发");
      append({verb + QStringLiteral("·") + compactBuffName(buffCatalog, event.buffId), kind, outcomeColor(kind), false,
        QStringLiteral("recorded-buff"), QStringLiteral("本阶段记录 Buff [%1] %2。%3").arg(event.buffId, verb,
          event.removing ? QStringLiteral("不据此推定由谁驱散或其他成员是否仍存在。") : QStringLiteral("不把触发当作新增成员。"))});
      continue;
    }
    const auto firstBuffVisual = result.size();
    if (const auto known = knownBuffStatus(event.buffId, config)) append(*known);
    for (const auto& cue : officialBuffCues(config)) {
      if (cue.startsWith(QStringLiteral("伤害标注·"))) continue;
      // These are typed independently of outcomeVisual: 暴击↑ is an attribute
      // change, never evidence that this attack was a critical hit. Colors
      // distinguish directions; they do not classify a Buff as beneficial.
      OutcomeVisual visual{cue, QStringLiteral("buff-state"), QColor("#7d64a7"), true,
        QStringLiteral("buff-config"), QStringLiteral("新增 Buff [%1] 的官方 beffect 展示：%2").arg(event.buffId, cue)};
      if (cue.endsWith(QChar(0x2191))) visual.kind = QStringLiteral("attribute-up");
      else if (cue.endsWith(QChar(0x2193))) visual.kind = QStringLiteral("attribute-down");
      else if (cue == QStringLiteral("睡眠")) visual.kind = QStringLiteral("status-sleep");
      else if (cue == QStringLiteral("控制")) visual.kind = QStringLiteral("status-control");
      else if (cue == QStringLiteral("禁锢")) visual.kind = QStringLiteral("status-bind");
      else if (cue == QStringLiteral("清除增益")) visual.kind = QStringLiteral("status-dispel");
      else if (cue == QStringLiteral("战魂")) visual.kind = QStringLiteral("war-soul");
      else if (cue == QStringLiteral("不灭意志")) visual.kind = QStringLiteral("indomitable-will");
      else if (cue == QStringLiteral("潜能爆发")) visual.kind = QStringLiteral("potential");
      else if (cue.startsWith(QStringLiteral("九天印记*")) || cue.startsWith(QStringLiteral("印记*"))) visual.kind = QStringLiteral("mark");
      visual.color = outcomeColor(visual.kind); append(visual);
    }
    if (result.size() == firstBuffVisual) {
      // A recorded acquisition stays visible even when the official client
      // supplies no name, description or dedicated presentation vocabulary.
      // The plus denotes one added member, never a beneficial-effect claim.
      const bool listed = buffCatalog.contains(event.buffId);
      const auto name = plainOfficial(config[QStringLiteral("nm")].toString());
      const auto caption = compactBuffName(buffCatalog, event.buffId);
      QString detail = QStringLiteral("本阶段记录新增 Buff [%1]；只表示新增一条成员记录，不判定增益、减益或技能层数。")
        .arg(event.buffId);
      if (!listed) detail += QStringLiteral("\n当前已核对的字典未收录此 Buff 配置；新增记录仍保留。");
      else if (name.isEmpty()) detail += QStringLiteral("\n编号对应的 Buff 定义已收录；官方未提供名称，不等于记录无法解析。");
      const auto presentation = replayBuffPublicPresentation(config);
      if (!presentation.explanations.isEmpty()) detail += QLatin1Char('\n') + presentation.explanations.join(QLatin1Char('\n'));
      append({QStringLiteral("获得效果\n%1").arg(caption), QStringLiteral("buff-add"),
        outcomeColor(QStringLiteral("buff-add")), false, QStringLiteral("recorded-buff"), detail});
    }
  }
  return result;
}

bool criticalBesideDamage(const ReplayUnitOverview& change) {
  const auto& active = change.activeMeterEvents ? *change.activeMeterEvents : change.actionMeterEvents;
  return change.hasActionMeterContext && std::any_of(active.cbegin(), active.cend(),
    [](const ReplayUnitEvent& event) {
      if (!event.critical || (event.metric != QStringLiteral("生命") && event.metric != QStringLiteral("护盾"))) return false;
      // critical is supplied only by the page's explicit direct-damage
      // association; numeric direction alone never establishes a critical hit.
      return (event.observedBefore && event.observedAfter && *event.observedAfter < *event.observedBefore) ||
        event.valueText.startsWith(QLatin1Char('-')) || event.valueText.startsWith(QChar(0x2212));
    });
}

QString withoutCriticalBadge(const QString& text) {
  QStringList groups;
  for (const auto& group : text.split(QStringLiteral(" · "), Qt::SkipEmptyParts)) {
    QStringList labels;
    for (const auto& raw : group.split(QStringLiteral(" → "), Qt::SkipEmptyParts)) {
      const auto label = raw.trimmed();
      if (label != QStringLiteral("暴击") && label != QStringLiteral("被暴击")) labels.append(raw);
    }
    if (!labels.isEmpty()) groups.append(labels.join(QStringLiteral(" → ")));
  }
  return groups.join(QStringLiteral(" · "));
}

// A card-local visual layer occupies only the existing Buff viewport. The
// underlying records, membership, labels and all three meters stay intact.
// Every symbol is selected from an explicit cue, never from a Buff's name.
class OutcomeOverlay final : public QWidget {
public:
  OutcomeOverlay(QWidget* viewport, QWidget* compactHost, QWidget* compactAnchor, const QString& key,
                 const ReplayUnitOverview& change, const QList<OutcomeVisual>& phaseBuffCues, int ownTeam,
                 std::optional<qint64> finalHp, bool playbackDriven, qreal progress, std::function<void()> pause)
      : QWidget(viewport), pause_(std::move(pause)), viewport_(viewport), compactHost_(compactHost), compactAnchor_(compactAnchor),
        animated_(change.animate), playbackDriven_(playbackDriven), ownTeam_(ownTeam) {
    setObjectName(QStringLiteral("unitOutcomeOverlay")); setProperty("fighterKey", key);
    setAttribute(Qt::WA_TranslucentBackground); setFocusPolicy(Qt::StrongFocus);
    const bool criticalOnDamage = criticalBesideDamage(change);
    const auto appendVisual = [&](const OutcomeVisual& visual) {
      if (criticalOnDamage && visual.kind == QStringLiteral("critical")) return;
      auto found = std::find_if(effects_.begin(), effects_.end(), [&](const auto& e) {
        return e.label == visual.label && e.kind == visual.kind && e.source == visual.source && e.buffId == visual.buffId &&
          e.applier == visual.applier && e.applierTeam == visual.applierTeam &&
          (!e.applier.isEmpty() || e.applierName == visual.applierName);
      });
      if (found == effects_.end()) effects_.append(visual);
      else if (!visual.detail.isEmpty() && !found->detail.contains(visual.detail)) found->detail += QLatin1Char('\n') + visual.detail;
    };
    const auto append = [&](const QString& text, bool derived) {
      for (const auto& group : text.split(QStringLiteral(" · "), Qt::SkipEmptyParts))
        for (const auto& raw : group.split(QStringLiteral(" → "), Qt::SkipEmptyParts)) {
          const auto label = raw.trimmed(); if (label.isEmpty()) continue;
          const auto visual = outcomeVisual(label, derived);
          // A typed before/after cue carries more information than its legacy
          // short badge. Keep the legacy label in the existing raw badge row.
          const bool typed = !derived && std::any_of(change.visuals.begin(), change.visuals.end(),
            [&](const auto& cue) { return !cue.kind.isEmpty() && cue.kind == visual.kind; });
          if (!typed) appendVisual(visual);
        }
    };
    append(change.visualBadge.value_or(change.badge), false);
    for (const auto& cue : change.visuals) if (!cue.label.isEmpty()) appendVisual(typedOutcomeVisual(cue));
    for (const auto& visual : phaseBuffCues) appendVisual(visual);
    append(change.clientBadge, true);
    // A phase can cross zero more than once. Its settled emphasis must agree
    // with the final recorded balance, while all transition icons remain.
    for (int i = 0; i < effects_.size(); ++i) {
      const auto& kind = effects_[i].kind;
      if (kind == QStringLiteral("fallen") || kind == QStringLiteral("recovery")) settled_ = i;
    }
    if (settled_ >= 0 && finalHp) {
      const auto lastKind = *finalHp <= 0 ? QStringLiteral("fallen") : QStringLiteral("recovery");
      for (int i = 0; i < effects_.size(); ++i) if (effects_[i].kind == lastKind) settled_ = i;
    }
    if (settled_ < 0) {
      for (int i = 0; i < effects_.size(); ++i) if (!effects_[i].derived) { settled_ = i; break; }
      if (settled_ < 0) settled_ = 0;
    }
    QStringList labels, kinds, sources, descriptions, appliers, applierNames, origins, symbols, buffIds;
    QVariantList applierTeams, eventIndices, evidence;
    for (const auto& effect : effects_) {
      labels.append(effect.label); kinds.append(effect.kind);
      sources.append(effect.source);
      appliers.append(effect.applier); applierNames.append(effect.applierName); applierTeams.append(effect.applierTeam);
      origins.append(effect.showApplier ? origin(effect).label : QString()); symbols.append(effect.symbol.isEmpty() ? effect.kind : effect.symbol);
      eventIndices.append(effect.eventIndex);
      buffIds.append(effect.buffId);
      evidence.append(QVariantMap{{"label", effect.label}, {"detail", effect.detail}, {"buffId", effect.buffId},
        {"eventIndex", effect.eventIndex}, {"applier", effect.applier}, {"applierName", effect.applierName}, {"applierTeam", effect.applierTeam}});
      const auto origin = effect.source == QStringLiteral("buff-config") ? QStringLiteral("官方 Buff 配置（本阶段新增）：") :
        effect.source == QStringLiteral("recorded-harm") ? QStringLiteral("本阶段数值记录的官方伤害标注：") :
        effect.derived ? QStringLiteral("官方客户端提示：") : QStringLiteral("战斗记录：");
      descriptions.append(origin + effect.label +
        (effect.showApplier ? QLatin1Char('\n') + this->origin(effect).detail : QString()));
    }
    setProperty("effectLabels", labels); setProperty("effectKinds", kinds); setProperty("effectSources", sources);
    setProperty("effectAppliers", appliers); setProperty("effectApplierNames", applierNames); setProperty("effectApplierTeams", applierTeams);
    setProperty("effectOriginLabels", origins); setProperty("effectSymbols", symbols); setProperty("effectEventIndices", eventIndices);
    setProperty("effectBuffIds", buffIds); setProperty("rawEvidence", evidence); setProperty("rawClientEvidence", change.clientDetail);
    setProperty("settledEffectKind", effects_.isEmpty() ? QString() : effects_[settled_].kind);
    setProperty("compact", false);
    setProperty("effectCount", effects_.size()); setProperty("animated", animated_);
    setProperty("playbackDriven", playbackDriven_);
    setAccessibleName(descriptions.join(QStringLiteral("；")));
    QString help = descriptions.join(QLatin1Char('\n'));
    if (criticalOnDamage) {
      const auto note = QStringLiteral("本次行动有已记录暴击伤害；小暴击标识显示在对应伤害数字之后。") + change.badge;
      setAccessibleName(accessibleName() + QLatin1Char('\n') + note);
      help += QLatin1Char('\n') + note;
    }
    if (kinds.contains(QStringLiteral("counter-marker"))) help += QStringLiteral("\n反击标记仅表示日志标记，实际反击出手单独播放。");
    if (!change.clientDetail.isEmpty()) help += QStringLiteral("\n官方显示提示；点击精灵可查看说明与原始详情。");
    if (effects_.size() > 1) help += QStringLiteral("\n各图标均为本阶段记录，当前状态以数值条为准。阶段结束优先最后生命状态。点击图标选择，左右方向键切换；+N 或向下键查看全部。手动选择会暂停并保持。");
    setToolTip(help); setAccessibleDescription(help);
    setCursor(effects_.size() > 1 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    viewport->installEventFilter(this); compactHost->installEventFilter(this); compactAnchor->installEventFilter(this);
    setGeometry(viewport->rect());
    applyProgress(progress); setVisible(!effects_.isEmpty()); raise();
    if (animated_ && !playbackDriven_ && !effects_.isEmpty()) {
      animation_ = new QVariantAnimation(this); animation_->setObjectName(QStringLiteral("replayOutcomeAnimation"));
      animation_->setDuration(std::max(change.transitionMs, int(effects_.size()) * 650));
      animation_->setStartValue(0.0); animation_->setEndValue(1.0);
      connect(animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) { applyProgress(value.toReal()); });
      applyProgress(0); animation_->start();
    }
  }
  void setPlaybackDriven(bool enabled) {
    if (animation_) animation_->stop();
    playbackDriven_ = enabled; setProperty("playbackDriven", enabled); applyProgress(1);
  }
  void setPlaybackProgress(qreal progress) {
    if (animation_) animation_->stop();
    applyProgress(progress);
  }
  void setExpanded(bool expanded) {
    compact_ = expanded; setProperty("compact", compact_);
    QWidget* host = compact_ ? compactHost_.data() : viewport_.data();
    if (host && parentWidget() != host) setParent(host);
    place(); setVisible(!effects_.isEmpty()); raise();
  }
  void attachSummary(UnitMiniRow* row) {
    summary_ = row;
    const QPointer<OutcomeOverlay> self(this);
    row->paintOutcome = [self](QPainter& painter, const QRectF& bounds) {
      if (!self || self->effects_.isEmpty()) return;
      const qreal local = self->animated_ && self->progress_ < 1
        ? std::fmod(self->progress_ * std::max(1, int(self->effects_.size())), qreal(1)) : 1;
      const qreal size = std::min(bounds.height(), bounds.width());
      const QRectF first(bounds.left(), bounds.top(), size, size);
      drawSymbol(painter, first, self->effects_[self->current_], local, self->animated_);
      self->drawApplier(painter, first, self->effects_[self->current_], true);
      if (bounds.width() >= size * 1.9 && self->effects_.size() > 1) {
        int other = self->settled_ != self->current_ ? self->settled_ : (self->current_ + 1) % self->effects_.size();
        const QRectF second(bounds.right() - size, bounds.top(), size, size);
        drawSymbol(painter, second, self->effects_[other], 1, false);
        self->drawApplier(painter, second, self->effects_[other], true);
      }
    };
    for (const auto* property : {"effectLabels", "effectKinds", "effectSources", "effectCount", "animated", "effectAppliers", "effectApplierNames", "effectApplierTeams", "effectOriginLabels", "effectSymbols", "effectEventIndices"})
      row->setProperty(property, this->property(property));
    row->setProperty("outcomeAnimated", animated_); row->setProperty("outcomeActive", !effects_.isEmpty());
    row->setAccessibleName(row->accessibleName() + QStringLiteral("\n本阶段效果图标：") + accessibleName());
    row->setToolTip(row->toolTip() + QStringLiteral("\n本阶段效果图标：") + toolTip());
    connect(this, &QObject::destroyed, row, [row] {
      row->paintOutcome = {}; row->outcomeColor = QColor(); row->setProperty("outcomeActive", false); row->update();
    });
    publishCurrent();
  }
protected:
  bool eventFilter(QObject* object, QEvent* event) override {
    if ((object == viewport_ || object == compactHost_ || object == compactAnchor_) &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show || event->type() == QEvent::Move)) {
      place(); raise();
    }
    return QWidget::eventFilter(object, event);
  }
  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && effects_.size() > 1) {
      if (overflowRect_.contains(event->position())) { showAll(); event->accept(); return; }
      int next = (current_ + 1) % effects_.size();
      for (int i = 0; i < iconRects_.size(); ++i) if (iconRects_[i].contains(event->position())) { next = visibleIndices_.value(i, next); break; }
      select(next); event->accept(); return;
    }
    QWidget::mouseReleaseEvent(event);
  }
  void keyPressEvent(QKeyEvent* event) override {
    if (event->key() == Qt::Key_Down && effects_.size() > 1) { showAll(); event->accept(); return; }
    if (effects_.size() > 1 && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right || event->key() == Qt::Key_Space)) {
      select((current_ + (event->key() == Qt::Key_Left ? effects_.size() - 1 : 1)) % effects_.size()); event->accept(); return;
    }
    QWidget::keyPressEvent(event);
  }
  void paintEvent(QPaintEvent*) override {
    if (effects_.isEmpty() || width() < 6 || height() < 6) return;
    paintedLines_.clear();
    const auto& effect = effects_[current_];
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    const QRectF bounds = QRectF(rect()).adjusted(.5, .5, -.5, -.5);
    const QColor background = effect.color.lighter(190);
    painter.setBrush(background);
    QPen border(effect.color.lighter(135), 1.2); if (effect.derived) border.setStyle(Qt::DashLine);
    painter.setPen(border); painter.drawRoundedRect(bounds, 5, 5);
    // Even a 14 px viewport gets simultaneous icons, rather than one cycling
    // label. The large emphasis is an optional use of additional space.
    const bool large = !compact_ && bounds.height() >= 43;
    const int railColumns = std::max(1, int(bounds.width() / 18));
    const int neededRows = std::max(1, (int(effects_.size()) + railColumns - 1) / railColumns);
    const int availableRows = large ? std::max(1, std::min(3, int((bounds.height() - 26) / 18))) :
      std::max(1, std::min(3, int(bounds.height() / 14)));
    const int railRows = std::min(neededRows, availableRows);
    const qreal railHeight = large && effects_.size() == 1 ? 0 : large ? std::min(bounds.height() - 23, railRows * 18.0) : bounds.height();
    const QRectF rail(bounds.left(), bounds.bottom() - railHeight, bounds.width(), railHeight);
    if (large) {
      const QRectF main(bounds.left() + 3, bounds.top() + 1, bounds.width() - 6, bounds.height() - railHeight - 2);
      const int originWidth = effect.showApplier || effect.derived ? 17 : 0;
      const QRectF content(main.left(), main.top(), main.width() - originWidth, main.height());
      if (!drawTransition(painter, content, effect)) {
        const qreal iconSize = std::min(qreal(34), std::max(qreal(14), main.height() - 2));
        const QRectF icon(main.left(), main.center().y() - iconSize / 2, iconSize, iconSize);
        drawSymbol(painter, icon, effect, localProgress(), animated_);
        const QRectF text(icon.right() + 3, main.top(), std::max(qreal(0), main.right() - icon.right() - 3 - originWidth), main.height());
        const auto lines = displayLabel(effect, false).split(QLatin1Char('\n'));
        auto font = painter.font(); font.setPixelSize(lines.size() > 1 ? 12 : main.height() >= 27 ? 17 : 14); font.setBold(true);
        if (lines.size() > 1) font.setPixelSize(std::max(9, std::min(font.pixelSize(), int(main.height() / lines.size()) - 1)));
        painter.setFont(font); painter.setPen(effect.color.darker(118));
        const qreal lineHeight = text.height() / std::max(1, int(lines.size()));
        for (int i = 0; i < lines.size(); ++i) {
          const QRectF line(text.left(), text.top() + i * lineHeight, text.width(), lineHeight);
          drawEffectText(painter, line, Qt::AlignVCenter | Qt::AlignLeft,
            QFontMetrics(font).elidedText(lines[i], i ? Qt::ElideMiddle : Qt::ElideRight, int(line.width())));
        }
      }
      if (effect.showApplier) drawApplier(painter, QRectF(main.right() - 17, main.top(), 17, main.height()), effect, false);
      if (effect.derived && (!effect.showApplier || main.height() >= 27)) {
        const auto evidence = effect.source == QStringLiteral("buff-config") ? QStringLiteral("配") :
          effect.source == QStringLiteral("recorded-harm") ? QStringLiteral("标") : QStringLiteral("示");
        auto font = painter.font(); font.setPixelSize(9); font.setBold(false); painter.setFont(font); painter.setPen(effect.color.darker(110));
        drawEffectText(painter, QRectF(main.right() - 17, main.top(), 17, 11), Qt::AlignCenter, evidence);
      }
    }
    if (railHeight > 0) drawChips(painter, rail, railRows);
    else {
      iconRects_ = {bounds}; visibleIndices_ = {0}; overflowRect_ = {};
      setProperty("visibleEffectLabels", QStringList{effect.label}); setProperty("visibleEffectKinds", QStringList{effect.kind});
      setProperty("overflowCount", 0);
    }
    setProperty("paintedEffectLines", paintedLines_);
  }
private:
  void drawEffectText(QPainter& painter, const QRectF& bounds, int flags, const QString& text) {
    painter.drawText(bounds, flags, text);
    if (!text.isEmpty()) paintedLines_.append(text);
  }
  bool drawTransition(QPainter& painter, const QRectF& content, const OutcomeVisual& effect) {
    static const QSet<QString> transitionKinds{QStringLiteral("skill-swap"), QStringLiteral("transform"),
      QStringLiteral("attributes"), QStringLiteral("empowerment"), QStringLiteral("self-transform")};
    if (!transitionKinds.contains(effect.kind)) return false;
    const auto lines = effect.label.split(QLatin1Char('\n'));
    int valueLine = -1, arrow = -1;
    for (int i = 0; i < lines.size(); ++i) {
      arrow = lines[i].indexOf(QStringLiteral(" → "));
      if (arrow >= 0) { valueLine = i; break; }
    }
    if (valueLine < 0) return false;
    const QString before = lines[valueLine].left(arrow).trimmed();
    const QString after = lines[valueLine].mid(arrow + 3).trimmed();
    const QString title = valueLine > 0 ? lines.mid(0, valueLine).join(QStringLiteral(" · ")) : displayLabel(effect, true);
    painter.save(); painter.setPen(effect.color.darker(118));
    const bool titleRow = content.height() >= 35;
    const qreal titleHeight = titleRow ? std::min(qreal(14), content.height() * .29) : 0;
    QRectF values = content;
    if (titleRow) {
      const qreal iconSize = std::min(qreal(13), titleHeight);
      drawSymbol(painter, QRectF(content.left(), content.top(), iconSize, iconSize), effect, localProgress(), animated_);
      auto font = painter.font(); font.setPixelSize(std::max(9, int(titleHeight) - 1)); font.setBold(true); painter.setFont(font);
      const QRectF heading(content.left() + iconSize + 3, content.top(), content.width() - iconSize - 3, titleHeight);
      drawEffectText(painter, heading, Qt::AlignLeft | Qt::AlignVCenter,
        QFontMetrics(font).elidedText(title, Qt::ElideRight, int(heading.width())));
      values.setTop(content.top() + titleHeight);
    } else {
      // The type remains visible in the icon strip; reserve the small main
      // region for both endpoints instead of losing the changed-to identity.
      const qreal iconSize = std::min(qreal(18), content.height() - 1);
      drawSymbol(painter, QRectF(content.left(), content.center().y() - iconSize / 2, iconSize, iconSize), effect, localProgress(), animated_);
      values.setLeft(content.left() + iconSize + 3);
    }
    const qreal lineHeight = values.height() / 2;
    auto font = painter.font(); font.setPixelSize(std::max(9, std::min(13, int(lineHeight) - 1))); font.setBold(true); painter.setFont(font);
    const QFontMetrics metrics(font);
    const auto drawEndpoint = [&](const QString& prefix, const QString& name, int row) {
      const int nameWidth = std::max(0, int(values.width()) - metrics.horizontalAdvance(prefix));
      // Only the individual name is shortened. The original/changed-to
      // distinction and arrow are always outside the elided string.
      const auto text = prefix + metrics.elidedText(name, Qt::ElideMiddle, nameWidth);
      drawEffectText(painter, QRectF(values.left(), values.top() + row * lineHeight, values.width(), lineHeight), Qt::AlignLeft | Qt::AlignVCenter, text);
    };
    drawEndpoint(QStringLiteral("原 "), before, 0);
    drawEndpoint(QStringLiteral("→ "), after, 1);
    painter.restore(); return true;
  }
  BuffOrigin origin(const OutcomeVisual& effect) const {
    return buffOrigin(effect.applier, effect.applierName, effect.applierTeam, ownTeam_, {});
  }
  static QString displayLabel(const OutcomeVisual& effect, bool chip) {
    if (!chip) {
      if (effect.label == QStringLiteral("技能未生效")) return QStringLiteral("未生效");
      return effect.label;
    }
    if (effect.kind == QStringLiteral("buff-add")) return QStringLiteral("获得");
    static const QMap<QString, QString> shortLabels{
      {QStringLiteral("critical"), QStringLiteral("暴击")}, {QStringLiteral("fallen"), QStringLiteral("归零")},
      {QStringLiteral("recovery"), QStringLiteral("恢复")}, {QStringLiteral("health-limit"), QStringLiteral("上限")},
      {QStringLiteral("counter-marker"), QStringLiteral("反标")}, {QStringLiteral("attack-disabled"), QStringLiteral("禁攻")},
      {QStringLiteral("invalid"), QStringLiteral("无效")}, {QStringLiteral("skill-swap"), QStringLiteral("换技")},
      {QStringLiteral("empowerment"), QStringLiteral("元能")}, {QStringLiteral("self-transform"), QStringLiteral("幻元")},
      {QStringLiteral("god-job-attack"), QStringLiteral("神攻")}, {QStringLiteral("god-job-defend"), QStringLiteral("神受")},
      {QStringLiteral("power-full"), QStringLiteral("满气")}, {QStringLiteral("buff-remove"), QStringLiteral("移除")},
      {QStringLiteral("buff-trigger"), QStringLiteral("触发")}, {QStringLiteral("indomitable-will"), QStringLiteral("不灭")}};
    return shortLabels.value(effect.kind, effect.label.section(QLatin1Char('\n'), 0, 0));
  }
  qreal localProgress() const {
    return animated_ && progress_ < 1 ? std::fmod(progress_ * std::max(1, int(effects_.size())), qreal(1)) : 1;
  }
  void place() {
    if (compact_ && compactAnchor_) setGeometry(compactAnchor_->geometry());
    else if (viewport_) setGeometry(viewport_->rect());
    update();
  }
  void drawApplier(QPainter& painter, const QRectF& area, const OutcomeVisual& effect, bool tiny) const {
    if (!effect.showApplier) return;
    const auto side = replayBuffSourceSide(effect.applier, effect.applierTeam, ownTeam_);
    const auto info = origin(effect);
    const QString letter = side == ReplayBuffSourceSide::Own ? QStringLiteral("我") :
      side == ReplayBuffSourceSide::Enemy ? QStringLiteral("敌") : QStringLiteral("?");
    const qreal size = tiny ? (area.width() >= 24 ? std::min(qreal(11), area.height() - 1) : std::min(qreal(8), area.height() * .63)) :
      std::min(qreal(15), area.height());
    const QRectF badge(area.right() - size, area.bottom() - size, size, size);
    painter.save(); painter.setPen(QPen(QColor(info.color), .7)); painter.setBrush(QColor(info.background));
    painter.drawRoundedRect(badge, 1.5, 1.5);
    auto font = painter.font(); font.setPixelSize(std::max(6, int(size - 2))); font.setBold(true); painter.setFont(font);
    painter.drawText(badge, Qt::AlignCenter, letter); painter.restore();
  }
  void drawChips(QPainter& painter, const QRectF& rail, int maxRows) {
    iconRects_.clear(); visibleIndices_.clear(); overflowRect_ = {};
    const int columns = std::max(1, int(rail.width() / 18));
    const int capacity = std::max(1, columns * maxRows);
    const bool overflow = effects_.size() > capacity;
    const int visibleCount = std::min(int(effects_.size()), std::max(1, capacity - (overflow ? 1 : 0)));
    for (int i = 0; i < visibleCount; ++i) visibleIndices_.append(i);
    if (!visibleIndices_.contains(current_) && !visibleIndices_.isEmpty()) visibleIndices_.last() = current_;
    const int slotCount = visibleCount + (overflow ? 1 : 0);
    const int rows = std::max(1, std::min(maxRows, (slotCount + columns - 1) / columns));
    const int rowColumns = std::max(1, (slotCount + rows - 1) / rows);
    const qreal slotWidth = rail.width() / rowColumns, slotHeight = rail.height() / rows;
    QStringList visibleLabels, visibleKinds;
    for (int slot = 0; slot < slotCount; ++slot) {
      const QRectF area(rail.left() + (slot % rowColumns) * slotWidth, rail.top() + (slot / rowColumns) * slotHeight, slotWidth, slotHeight);
      if (slot == visibleCount) {
        overflowRect_ = area;
        painter.setPen(QPen(QColor("#78869a"), .8)); painter.setBrush(QColor("#edf1f6")); painter.drawRoundedRect(area.adjusted(.5, .5, -.5, -.5), 2, 2);
        auto font = painter.font(); font.setPixelSize(std::max(8, std::min(11, int(area.height() - 1)))); font.setBold(true); painter.setFont(font);
        drawEffectText(painter, area, Qt::AlignCenter, QStringLiteral("+%1").arg(effects_.size() - visibleCount)); continue;
      }
      const int index = visibleIndices_[slot]; const auto& effect = effects_[index];
      iconRects_.append(area); visibleLabels.append(effect.label); visibleKinds.append(effect.kind);
      const auto fill = effect.color.lighter(index == current_ ? 173 : 194);
      painter.setBrush(fill); painter.setPen(QPen(effect.color.lighter(index == current_ ? 102 : 145), index == current_ ? 1.1 : .6,
        effect.derived ? Qt::DashLine : Qt::SolidLine));
      painter.drawRoundedRect(area.adjusted(.5, .5, -.5, -.5), 2, 2);
      const qreal size = std::max(qreal(6), std::min({qreal(17), area.height() - 1, area.width() - 2}));
      const int sourceWidth = effect.showApplier ? 11 : 0;
      const bool textFits = area.width() - size - sourceWidth >= 17;
      const QRectF icon(textFits ? area.left() + 1 : area.center().x() - size / 2, area.center().y() - size / 2, size, size);
      drawSymbol(painter, icon, effect, index == current_ ? localProgress() : 1, animated_ && index == current_);
      if (textFits) {
        auto font = painter.font(); font.setPixelSize(std::max(8, std::min(11, int(area.height() - 2)))); font.setBold(true); painter.setFont(font); painter.setPen(effect.color.darker(125));
        const QRectF label(icon.right() + 1, area.top(), area.right() - icon.right() - 2 - sourceWidth, area.height());
        drawEffectText(painter, label, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(font).elidedText(displayLabel(effect, true), Qt::ElideRight, int(label.width())));
      }
      drawApplier(painter, area.adjusted(0, 0, -1, -1), effect, true);
    }
    setProperty("visibleEffectLabels", visibleLabels); setProperty("visibleEffectKinds", visibleKinds);
    setProperty("overflowCount", effects_.size() - visibleCount);
  }
  void showAll() {
    if (animation_) animation_->pause();
    if (pause_) pause_();
    auto* menu = new QMenu(this); menu->setObjectName(QStringLiteral("unitOutcomeMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose); menu->setToolTipsVisible(true);
    for (int i = 0; i < effects_.size(); ++i) {
      const auto& effect = effects_[i];
      QPixmap image(24, 24); image.fill(Qt::transparent); QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing);
      drawSymbol(painter, QRectF(1, 1, 22, 22), effect, 1, false); painter.end();
      auto* item = menu->addAction(QIcon(image), QString(effect.label).replace(QLatin1Char('\n'), QStringLiteral(" · ")) +
        (effect.showApplier ? QStringLiteral(" · ") + origin(effect).label : QString()));
      item->setCheckable(true); item->setChecked(i == current_); item->setData(i);
      item->setToolTip(effect.label + (effect.showApplier ? QLatin1Char('\n') + origin(effect).detail : QString()));
      item->setProperty("rawEvidence", effect.detail); item->setProperty("buffId", effect.buffId);
      connect(item, &QAction::triggered, this, [this, i] { select(i); });
    }
    menu->popup(mapToGlobal(QPoint(0, height())));
  }
  void select(int index) {
    if (animation_) animation_->pause();
    if (pause_) pause_();
    selected_ = current_ = index; publishCurrent(); update();
  }
  void applyProgress(qreal progress) {
    progress = std::isfinite(progress) ? std::clamp(progress, qreal(0), qreal(1)) : 1;
    progress_ = progress; setProperty("playbackProgress", progress_);
    // A user's choice stays put for this phase, including at progress=1.
    // Automatically settled emphasis uses the last life state, not the first
    // death/critical event, while every event remains in the icon strip.
    current_ = selected_ >= 0 ? selected_ : animated_ && progress_ < 1 && !effects_.isEmpty()
      ? std::min(int(progress_ * effects_.size()), int(effects_.size()) - 1) : settled_;
    publishCurrent(); update();
  }
  void publishCurrent() {
    setProperty("currentEffectIndex", effects_.isEmpty() ? -1 : current_);
    setProperty("currentEffectKind", effects_.isEmpty() ? QString() : effects_[current_].kind);
    setProperty("currentEffectLabel", effects_.isEmpty() ? QString() : effects_[current_].label);
    setProperty("currentEffectDerived", !effects_.isEmpty() && effects_[current_].derived);
    setProperty("currentEffectSource", effects_.isEmpty() ? QString() : effects_[current_].source);
    setProperty("currentEffectApplier", effects_.isEmpty() ? QString() : effects_[current_].applier);
    setProperty("currentEffectApplierTeam", effects_.isEmpty() ? -1 : effects_[current_].applierTeam);
    setProperty("currentEffectOriginLabel", effects_.isEmpty() || !effects_[current_].showApplier ? QString() : origin(effects_[current_]).label);
    setProperty("selectedEffectIndex", selected_);
    if (summary_) {
      summary_->outcomeColor = effects_.isEmpty() ? QColor() : effects_[current_].color;
      for (const auto* property : {"currentEffectIndex", "currentEffectKind", "currentEffectLabel", "currentEffectDerived", "currentEffectSource", "currentEffectApplier", "currentEffectApplierTeam", "currentEffectOriginLabel", "playbackProgress", "playbackDriven"})
        summary_->setProperty(property, this->property(property));
      summary_->update();
    }
  }
  static void drawSymbol(QPainter& painter, const QRectF& rect, const OutcomeVisual& effect, qreal progress, bool animate) {
    painter.save(); painter.translate(rect.topLeft()); painter.scale(rect.width() / 32, rect.height() / 32);
    const qreal pulse = animate && progress < 1 ? std::sin(progress * 3.14159265358979323846) : 0;
    const qreal flutter = animate && progress < 1 ? std::sin(progress * 6.28318530717958647692) : 0;
    painter.translate(16, 16);
    if (effect.kind == QStringLiteral("miss")) painter.translate(pulse * 4, 0);
    else if (effect.kind == QStringLiteral("attribute-up")) painter.translate(0, -pulse * 3);
    else if (effect.kind == QStringLiteral("attribute-down")) painter.translate(0, pulse * 3);
    else if (effect.kind == QStringLiteral("status-sleep")) { painter.translate(0, -pulse * 2); painter.rotate(flutter * 5); }
    else if (effect.kind == QStringLiteral("status-control") || effect.kind == QStringLiteral("status-bind")) painter.translate(flutter * 1.5, 0);
    else if (effect.kind == QStringLiteral("status-dispel")) painter.translate(flutter * 3, 0);
    else if (effect.kind == QStringLiteral("harm-source")) painter.translate(0, -pulse * 2);
    else if (effect.kind == QStringLiteral("recovery") || effect.kind == QStringLiteral("summon") || effect.kind == QStringLiteral("entry")) painter.translate(0, -pulse * 3);
    else if (effect.kind == QStringLiteral("fallen")) painter.translate(0, pulse * 3);
    else if (effect.kind == QStringLiteral("block")) painter.translate(flutter * 2, 0);
    else if (effect.kind == QStringLiteral("invalid")) painter.rotate(flutter * 7);
    else if (effect.kind == QStringLiteral("counter")) painter.rotate(-pulse * 22);
    else if (effect.kind == QStringLiteral("transform") || effect.kind == QStringLiteral("attributes") || effect.kind == QStringLiteral("self-transform")) painter.rotate(pulse * 28);
    else if (effect.kind == QStringLiteral("buff-add")) painter.translate(0, -pulse * 2);
    else if (effect.kind == QStringLiteral("buff-remove")) painter.translate(flutter * 2, -pulse * 2);
    else if (effect.kind == QStringLiteral("power-full") || effect.kind == QStringLiteral("potential")) painter.translate(0, -pulse * 3);
    else if (effect.kind == QStringLiteral("critical")) painter.scale(1 + pulse * .12, 1 + pulse * .12);
    else painter.scale(1 + pulse * .07, 1 + pulse * .07);
    painter.translate(-16, -16);
    painter.setPen(QPen(effect.color.darker(112), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); painter.setBrush(effect.color);
    const auto line = [&](qreal x1, qreal y1, qreal x2, qreal y2) { painter.drawLine(QPointF(x1, y1), QPointF(x2, y2)); };
    const auto star = [&](int points, qreal outer, qreal inner) {
      QPolygonF polygon;
      for (int i = 0; i < points * 2; ++i) {
        const qreal angle = i * 3.14159265358979323846 / points - 1.57079632679489661923;
        const qreal radius = i % 2 ? inner : outer; polygon << QPointF(16 + std::cos(angle) * radius, 16 + std::sin(angle) * radius);
      }
      painter.drawPolygon(polygon);
    };
    const auto heart = [&] {
      QPainterPath path; path.moveTo(16, 28); path.cubicTo(2, 18, 1, 8, 9, 6); path.cubicTo(12, 5, 15, 7, 16, 10);
      path.cubicTo(21, 1, 33, 7, 29, 17); path.cubicTo(27, 22, 21, 26, 16, 28); painter.drawPath(path);
    };
    const auto returnArrow = [&](bool marker) {
      painter.setBrush(Qt::NoBrush); painter.setPen(QPen(effect.color, marker ? 2.4 : 3.5, marker ? Qt::DashLine : Qt::SolidLine, Qt::RoundCap));
      painter.drawArc(QRectF(5, 5, 22, 22), 30 * 16, 285 * 16);
      painter.setPen(Qt::NoPen); painter.setBrush(effect.color); painter.drawPolygon(QPolygonF{QPointF(25, 3), QPointF(29, 13), QPointF(19, 10)});
      if (marker) { painter.setBrush(Qt::white); painter.setPen(QPen(effect.color, 1.8)); painter.drawRoundedRect(QRectF(11, 12, 9, 9), 2, 2); }
      else { painter.setPen(QPen(effect.color, 3)); line(10, 16, 22, 16); line(10, 16, 14, 12); line(10, 16, 14, 20); }
    };
    auto kind = effect.kind;
    if (kind == QStringLiteral("harm-source") && !effect.symbol.isEmpty()) kind = effect.symbol;
    if (kind == QStringLiteral("status-burn")) kind = QStringLiteral("flame");
    else if (kind == QStringLiteral("status-frost")) kind = QStringLiteral("frost");
    else if (kind == QStringLiteral("status-poison")) kind = QStringLiteral("poison");
    else if (kind == QStringLiteral("status-restraint")) kind = QStringLiteral("chain");
    else if (kind == QStringLiteral("status-paralysis")) kind = QStringLiteral("lightning");
    if (kind == QStringLiteral("flame")) {
      QPainterPath flame; flame.moveTo(16, 1); flame.cubicTo(23, 7, 18, 12, 25, 12);
      flame.cubicTo(34, 22, 23, 31, 16, 30); flame.cubicTo(3, 31, 1, 18, 9, 11);
      flame.cubicTo(7, 20, 16, 13, 16, 1); painter.drawPath(flame);
      painter.setPen(Qt::NoPen); painter.setBrush(effect.color.lighter(185)); QPainterPath core;
      core.moveTo(16, 14); core.cubicTo(23, 23, 21, 28, 16, 28); core.cubicTo(8, 27, 13, 21, 16, 14); painter.drawPath(core);
    } else if (kind == QStringLiteral("frost")) {
      painter.setBrush(Qt::NoBrush); painter.setPen(QPen(effect.color, 2.1, Qt::SolidLine, Qt::RoundCap));
      for (int i = 0; i < 6; ++i) {
        painter.save(); painter.translate(16, 16); painter.rotate(i * 60); line(0, 0, 0, -14); line(0, -9, -4, -12); line(0, -9, 4, -12); painter.restore();
      }
      painter.setBrush(effect.color.lighter(175)); painter.drawPolygon(QPolygonF{QPointF(16, 10), QPointF(21, 16), QPointF(16, 22), QPointF(11, 16)});
    } else if (kind == QStringLiteral("poison") || kind == QStringLiteral("drain")) {
      QPainterPath drop; drop.moveTo(16, 2); drop.cubicTo(11, 10, 4, 16, 5, 22); drop.cubicTo(5, 33, 28, 33, 27, 22);
      drop.cubicTo(27, 15, 20, 7, 16, 2); painter.drawPath(drop);
      painter.setBrush(Qt::white); painter.setPen(Qt::NoPen);
      if (kind == QStringLiteral("poison")) {
        painter.drawEllipse(QRectF(9, 17, 5, 5)); painter.drawEllipse(QRectF(18, 17, 5, 5));
        painter.setPen(QPen(Qt::white, 2)); line(12, 26, 20, 26); line(14, 24, 14, 28); line(18, 24, 18, 28);
      } else {
        painter.setPen(QPen(Qt::white, 2.5)); line(9, 21, 23, 21); line(9, 21, 14, 16); line(9, 21, 14, 26);
      }
    } else if (kind == QStringLiteral("chain")) {
      painter.setBrush(Qt::NoBrush); painter.setPen(QPen(effect.color, 3.1));
      painter.save(); painter.translate(16, 16); painter.rotate(-36);
      painter.drawRoundedRect(QRectF(-14, -6, 18, 12), 5, 5); painter.drawRoundedRect(QRectF(-4, -6, 18, 12), 5, 5);
      painter.setPen(QPen(effect.color.lighter(160), 1)); line(-2, -3, 2, -3); painter.restore();
    } else if (kind == QStringLiteral("status-blind")) {
      painter.setBrush(effect.color.lighter(175)); QPainterPath eye; eye.moveTo(1, 16); eye.quadTo(16, 1, 31, 16); eye.quadTo(16, 31, 1, 16); painter.drawPath(eye);
      painter.setBrush(effect.color); painter.drawEllipse(QRectF(11, 10, 10, 12));
      painter.setPen(QPen(Qt::white, 5, Qt::SolidLine, Qt::RoundCap)); line(5, 27, 27, 5);
      painter.setPen(QPen(effect.color.darker(135), 2.7, Qt::SolidLine, Qt::RoundCap)); line(5, 27, 27, 5);
    } else if (kind == QStringLiteral("status-physical-seal")) {
      painter.drawPolygon(QPolygonF{QPointF(23, 2), QPointF(28, 2), QPointF(28, 7), QPointF(13, 22), QPointF(9, 18)});
      painter.setPen(QPen(effect.color, 3.3)); line(5, 17, 15, 27); line(4, 28, 10, 22);
      painter.setBrush(Qt::NoBrush); painter.setPen(QPen(effect.color.darker(125), 2.2)); painter.drawEllipse(QRectF(1, 1, 30, 30)); line(6, 6, 26, 26);
    } else if (kind == QStringLiteral("status-stun")) {
      painter.setBrush(effect.color.lighter(175)); painter.drawEllipse(QRectF(9, 14, 14, 16));
      painter.setBrush(Qt::NoBrush); painter.drawEllipse(QRectF(1, 4, 30, 13));
      for (const auto& point : {QPointF(5, 5), QPointF(26, 6), QPointF(16, 15)}) {
        painter.setBrush(effect.color); painter.drawPolygon(QPolygonF{point + QPointF(0, -4), point + QPointF(1.3, -1.1), point + QPointF(4, 0), point + QPointF(1.3, 1.1), point + QPointF(0, 4), point + QPointF(-1.3, 1.1), point + QPointF(-4, 0), point + QPointF(-1.3, -1.1)});
      }
    } else if (kind == QStringLiteral("lightning")) {
      painter.drawPolygon(QPolygonF{QPointF(18, 1), QPointF(5, 18), QPointF(13, 18), QPointF(11, 31), QPointF(28, 11), QPointF(19, 11)});
      if (effect.kind == QStringLiteral("status-paralysis")) { painter.setBrush(Qt::NoBrush); painter.setPen(QPen(effect.color, 1.8, Qt::DashLine)); painter.drawEllipse(QRectF(2, 2, 28, 28)); }
    } else if (kind == QStringLiteral("buff-add") || kind == QStringLiteral("buff-remove") || kind == QStringLiteral("buff-trigger")) {
      painter.setBrush(effect.color.lighter(175)); painter.drawRoundedRect(QRectF(4, 3, 21, 25), 3, 3);
      painter.setPen(QPen(effect.color, 2)); line(9, 10, 20, 10); line(9, 16, 16, 16);
      if (kind == QStringLiteral("buff-add") || kind == QStringLiteral("buff-remove")) {
        painter.setBrush(effect.color); painter.setPen(Qt::NoPen); painter.drawEllipse(QRectF(15, 16, 17, 16));
        painter.setPen(QPen(Qt::white, 2.6)); line(19, 24, 28, 24);
        if (kind == QStringLiteral("buff-add")) line(23.5, 19.5, 23.5, 28.5);
      } else {
        painter.setPen(QPen(effect.color, 1.6)); line(26, 3, 30, 0); line(28, 9, 32, 9);
        painter.setBrush(effect.color); painter.drawPolygon(QPolygonF{QPointF(24, 10), QPointF(16, 23), QPointF(22, 23), QPointF(20, 31), QPointF(31, 18), QPointF(25, 18)});
      }
    } else if (kind == QStringLiteral("war-soul")) {
      QPainterPath helmet; helmet.moveTo(6, 28); helmet.lineTo(6, 13); helmet.quadTo(5, 3, 16, 4); helmet.quadTo(27, 3, 26, 13); helmet.lineTo(26, 28);
      helmet.lineTo(20, 25); helmet.lineTo(20, 15); helmet.lineTo(12, 15); helmet.lineTo(12, 25); helmet.closeSubpath(); painter.drawPath(helmet);
      painter.setBrush(effect.color.lighter(160)); painter.drawPolygon(QPolygonF{QPointF(12, 4), QPointF(16, 0), QPointF(20, 4), QPointF(18, 14), QPointF(14, 14)});
      painter.setPen(QPen(Qt::white, 2)); line(8, 12, 13, 13); line(19, 13, 24, 12);
    } else if (kind == QStringLiteral("indomitable-will")) {
      painter.setBrush(effect.color.lighter(160)); painter.drawPolygon(QPolygonF{QPointF(2, 5), QPointF(11, 10), QPointF(16, 25), QPointF(5, 19)});
      painter.drawPolygon(QPolygonF{QPointF(30, 5), QPointF(21, 10), QPointF(16, 25), QPointF(27, 19)});
      painter.setBrush(effect.color); QPainterPath shield; shield.moveTo(16, 3); shield.lineTo(25, 9); shield.lineTo(23, 23); shield.lineTo(16, 31); shield.lineTo(9, 23); shield.lineTo(7, 9); shield.closeSubpath(); painter.drawPath(shield);
      painter.setPen(QPen(Qt::white, 2.1)); painter.setBrush(Qt::NoBrush); painter.drawEllipse(QRectF(10, 13, 7, 7)); painter.drawEllipse(QRectF(15, 13, 7, 7));
    } else if (kind == QStringLiteral("mark")) {
      painter.setBrush(effect.color.lighter(180)); painter.drawEllipse(QRectF(2, 2, 28, 28)); painter.setBrush(effect.color); star(6, 12, 8);
      painter.setPen(Qt::white); auto font = painter.font(); font.setPixelSize(16); font.setBold(true); painter.setFont(font);
      // The numeral is the exact official beffect label, not an inferred Buff
      // stack count. Membership multiplicity remains a separate record count.
      painter.drawText(QRectF(8, 6, 16, 21), Qt::AlignCenter, effect.label.section(QLatin1Char('*'), 1));
    } else if (kind == QStringLiteral("potential")) {
      star(9, 15, 8); painter.setPen(Qt::NoPen); painter.setBrush(Qt::white);
      painter.drawPolygon(QPolygonF{QPointF(16, 6), QPointF(23, 16), QPointF(19, 16), QPointF(19, 25), QPointF(13, 25), QPointF(13, 16), QPointF(9, 16)});
    } else if (kind == QStringLiteral("power-full")) {
      painter.setBrush(effect.color.lighter(185)); painter.drawRoundedRect(QRectF(7, 3, 18, 27), 3, 3);
      painter.setBrush(effect.color); painter.drawRoundedRect(QRectF(10, 8, 12, 19), 1, 1); painter.drawRect(QRectF(12, 0, 8, 3));
      painter.setPen(QPen(Qt::white, 2.5)); line(12, 18, 15, 21); line(15, 21, 21, 13);
      painter.setPen(QPen(effect.color, 1.4)); line(1, 10, 4, 12); line(28, 12, 31, 10);
    } else if (kind == QStringLiteral("explosion")) {
      star(9, 15, 6); painter.setPen(QPen(Qt::white, 2)); painter.setBrush(Qt::NoBrush); painter.drawEllipse(QRectF(10, 10, 12, 12));
    } else if (kind == QStringLiteral("splash")) {
      painter.drawEllipse(QRectF(11, 15, 10, 11)); painter.drawEllipse(QRectF(1, 2, 6, 6)); painter.drawEllipse(QRectF(25, 2, 6, 6));
      painter.setPen(QPen(effect.color, 2.4)); line(12, 15, 7, 9); line(20, 15, 25, 9); line(16, 12, 16, 2); line(16, 27, 16, 31);
    } else if (kind == QStringLiteral("reflect")) {
      painter.setBrush(effect.color.lighter(160)); painter.drawRoundedRect(QRectF(24, 2, 5, 28), 1, 1);
      painter.setPen(QPen(effect.color, 3.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); painter.setBrush(Qt::NoBrush);
      QPainterPath bounce; bounce.moveTo(2, 6); bounce.lineTo(23, 16); bounce.lineTo(3, 26); painter.drawPath(bounce);
      line(3, 26, 5, 18); line(3, 26, 11, 28);
    } else if (kind == QStringLiteral("curse")) {
      painter.setBrush(effect.color.lighter(170)); painter.drawPolygon(QPolygonF{QPointF(16, 1), QPointF(30, 16), QPointF(16, 31), QPointF(2, 16)});
      painter.setBrush(effect.color); painter.drawEllipse(QRectF(8, 7, 16, 16)); painter.drawRect(QRectF(11, 20, 10, 6));
      painter.setBrush(Qt::white); painter.setPen(Qt::NoPen); painter.drawEllipse(QRectF(11, 12, 4, 5)); painter.drawEllipse(QRectF(18, 12, 4, 5));
    } else if (kind == QStringLiteral("portal")) {
      painter.setBrush(Qt::NoBrush); painter.setPen(QPen(effect.color, 3)); painter.drawEllipse(QRectF(7, 1, 18, 29));
      painter.setPen(QPen(effect.color.lighter(145), 2)); painter.drawEllipse(QRectF(12, 6, 8, 20));
      painter.setPen(QPen(effect.color, 1.5)); line(1, 11, 6, 11); line(3.5, 8.5, 3.5, 13.5); line(26, 23, 31, 23); line(28.5, 20.5, 28.5, 25.5);
    } else if (kind == QStringLiteral("beam")) {
      painter.setPen(QPen(effect.color, 4)); line(1, 16, 31, 16);
      painter.setPen(QPen(effect.color.lighter(140), 1.5)); line(2, 9, 27, 12); line(2, 23, 27, 20);
      painter.setBrush(effect.color); painter.drawEllipse(QRectF(23, 9, 8, 14));
    } else if (kind == QStringLiteral("named-harm")) {
      painter.setBrush(effect.color.lighter(175)); painter.drawEllipse(QRectF(2, 2, 28, 28));
      painter.setPen(effect.color.darker(130)); auto font = painter.font(); font.setPixelSize(13); font.setBold(true); painter.setFont(font);
      painter.drawText(QRectF(3, 3, 26, 26), Qt::AlignCenter, effect.label.left(2));
    } else if (kind == QStringLiteral("attribute-up") || kind == QStringLiteral("attribute-down")) {
      if (kind == QStringLiteral("attribute-down")) { painter.translate(16, 16); painter.rotate(180); painter.translate(-16, -16); }
      painter.drawPolygon(QPolygonF{QPointF(16, 2), QPointF(28, 14), QPointF(21, 14), QPointF(21, 28), QPointF(11, 28), QPointF(11, 14), QPointF(4, 14)});
      painter.setPen(QPen(Qt::white, 1.8)); line(14, 19, 18, 19); line(14, 23, 18, 23);
    } else if (kind == QStringLiteral("status-sleep")) {
      QPainterPath moon, opening; moon.addEllipse(QRectF(3, 3, 24, 25)); opening.addEllipse(QRectF(11, -1, 23, 24));
      painter.drawPath(moon.subtracted(opening));
      auto font = painter.font(); font.setPixelSize(14); font.setBold(true); painter.setFont(font); painter.setPen(effect.color.darker(110));
      painter.drawText(QRectF(16, 11, 14, 18), Qt::AlignCenter, QStringLiteral("Z"));
    } else if (kind == QStringLiteral("status-control") || kind == QStringLiteral("status-bind")) {
      painter.setBrush(Qt::NoBrush); QPainterPath shackle; shackle.moveTo(10, 14); shackle.lineTo(10, 9); shackle.quadTo(10, 3, 16, 3);
      shackle.quadTo(22, 3, 22, 9); shackle.lineTo(22, 14); painter.drawPath(shackle);
      painter.setBrush(effect.color); painter.drawRoundedRect(QRectF(6, 13, 20, 16), 3, 3);
      painter.setBrush(Qt::white); painter.setPen(Qt::NoPen); painter.drawEllipse(QRectF(13.5, 17, 5, 5)); painter.drawRect(QRectF(15, 20, 2, 5));
      if (kind == QStringLiteral("status-bind")) { painter.setPen(QPen(effect.color, 2.5)); line(1, 19, 6, 19); line(26, 23, 31, 23); }
    } else if (kind == QStringLiteral("status-dispel")) {
      painter.setBrush(effect.color.lighter(170)); painter.drawRoundedRect(QRectF(3, 3, 18, 23), 2, 2);
      painter.setPen(QPen(effect.color, 1.5)); line(7, 8, 16, 8); line(7, 13, 16, 13);
      painter.setBrush(effect.color); painter.drawPolygon(QPolygonF{QPointF(11, 25), QPointF(25, 9), QPointF(31, 15), QPointF(18, 30)});
      painter.setPen(QPen(Qt::white, 2)); line(14, 23, 21, 28);
    } else if (kind == QStringLiteral("buff-state")) {
      painter.setBrush(effect.color.lighter(170)); painter.drawEllipse(QRectF(3, 3, 26, 26));
      painter.setBrush(effect.color); painter.drawPolygon(QPolygonF{QPointF(16, 7), QPointF(24, 16), QPointF(16, 25), QPointF(8, 16)});
      painter.setPen(QPen(Qt::white, 1.8)); line(13, 16, 19, 16); line(16, 12, 16, 20);
    } else if (kind == QStringLiteral("harm-source")) {
      painter.setBrush(Qt::NoBrush); painter.drawEllipse(QRectF(7, 7, 18, 18));
      for (int i = 0; i < 6; ++i) {
        const qreal angle = i * 3.14159265358979323846 / 3;
        line(16 + std::cos(angle) * 12, 16 + std::sin(angle) * 12, 16 + std::cos(angle) * 15, 16 + std::sin(angle) * 15);
      }
      painter.setBrush(effect.color); painter.drawEllipse(QRectF(12, 12, 8, 8));
    } else if (kind == QStringLiteral("block") || kind == QStringLiteral("immune") || kind == QStringLiteral("guard")) {
      QPainterPath shield; shield.moveTo(16, 3); shield.lineTo(28, 8); shield.lineTo(26, 20); shield.quadTo(22, 27, 16, 30);
      shield.quadTo(9, 27, 6, 20); shield.lineTo(4, 8); shield.closeSubpath(); painter.drawPath(shield);
      painter.setPen(QPen(Qt::white, 2.7));
      if (kind == QStringLiteral("immune")) { painter.setBrush(Qt::NoBrush); painter.drawEllipse(QRectF(10, 10, 12, 12)); line(11, 21, 21, 11); }
      else { line(16, 8, 16, 24); line(9, 13, 23, 13); }
      if (kind == QStringLiteral("immune") && pulse > 0) { auto halo = effect.color; halo.setAlpha(120); painter.setPen(QPen(halo, 1.3)); painter.setBrush(Qt::NoBrush); painter.drawEllipse(QPointF(16, 16), 13 + pulse * 3, 13 + pulse * 3); }
    } else if (kind == QStringLiteral("critical")) {
      star(8, 14, 8); painter.setPen(QPen(Qt::white, 3.2)); line(16, 9, 16, 18); painter.drawPoint(QPointF(16, 23));
    } else if (kind == QStringLiteral("miss")) {
      painter.setBrush(Qt::NoBrush); painter.setPen(QPen(effect.color, 2.3, Qt::DashLine)); painter.drawEllipse(QRectF(17, 3, 9, 9));
      QPainterPath figure; figure.moveTo(18, 14); figure.quadTo(27, 12, 28, 27); figure.lineTo(14, 27); figure.quadTo(13, 21, 18, 14); painter.drawPath(figure);
      painter.setPen(QPen(effect.color, 2.5, Qt::SolidLine, Qt::RoundCap)); line(2, 8, 11, 8); line(0, 16, 10, 16); line(3, 24, 9, 24);
    } else if (kind == QStringLiteral("invalid")) {
      painter.setBrush(Qt::NoBrush); painter.drawEllipse(QRectF(3, 3, 26, 26)); painter.setPen(QPen(effect.color, 3.5, Qt::SolidLine, Qt::RoundCap)); line(10, 10, 22, 22); line(10, 22, 22, 10);
    } else if (kind == QStringLiteral("attack-disabled")) {
      painter.drawPolygon(QPolygonF{QPointF(10, 2), QPointF(22, 2), QPointF(30, 10), QPointF(30, 22), QPointF(22, 30), QPointF(10, 30), QPointF(2, 22), QPointF(2, 10)});
      painter.setPen(QPen(Qt::white, 4, Qt::SolidLine, Qt::RoundCap)); line(9, 16, 23, 16);
    } else if (kind == QStringLiteral("counter-marker") || kind == QStringLiteral("counter")) {
      returnArrow(kind == QStringLiteral("counter-marker"));
    } else if (kind == QStringLiteral("skill-swap")) {
      painter.setBrush(effect.color.lighter(165)); painter.drawRoundedRect(QRectF(1, 2, 12, 19), 2, 2);
      painter.setBrush(effect.color); painter.drawRoundedRect(QRectF(19, 11, 12, 19), 2, 2);
      painter.setBrush(Qt::NoBrush); painter.setPen(QPen(effect.color, 2.5));
      line(13, 5, 27, 5); line(27, 5, 23, 1); line(27, 5, 23, 9);
      line(19, 27, 5, 27); line(5, 27, 9, 23); line(5, 27, 9, 31);
    } else if (kind == QStringLiteral("fallen") || kind == QStringLiteral("recovery") || kind == QStringLiteral("health-limit")) {
      heart(); painter.setPen(QPen(Qt::white, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      if (kind == QStringLiteral("fallen")) { QPainterPath crack; crack.moveTo(17, 7); crack.lineTo(13, 15); crack.lineTo(19, 17); crack.lineTo(15, 26); painter.drawPath(crack); }
      else if (kind == QStringLiteral("recovery")) { line(10, 16, 22, 16); line(16, 10, 16, 22); }
      else { line(16, 10, 16, 22); line(12, 14, 16, 10); line(20, 14, 16, 10); line(12, 22, 20, 22); }
    } else if (kind == QStringLiteral("clone")) {
      painter.setBrush(effect.color.lighter(170)); painter.drawRoundedRect(QRectF(2, 3, 17, 21), 3, 3);
      painter.setBrush(effect.color); painter.drawRoundedRect(QRectF(12, 10, 17, 21), 3, 3); painter.setPen(QPen(Qt::white, 2)); line(17, 16, 24, 16); line(17, 22, 24, 22);
    } else if (kind == QStringLiteral("summon") || kind == QStringLiteral("entry")) {
      painter.setBrush(effect.color.lighter(170)); painter.drawEllipse(QRectF(3, 21, 26, 8)); painter.setBrush(effect.color);
      painter.drawPolygon(QPolygonF{QPointF(16, 2), QPointF(25, 13), QPointF(20, 13), QPointF(20, 23), QPointF(12, 23), QPointF(12, 13), QPointF(7, 13)});
      if (kind == QStringLiteral("summon")) { painter.setPen(QPen(effect.color, 1.7)); line(3, 7, 7, 7); line(5, 5, 5, 9); line(26, 17, 30, 17); line(28, 15, 28, 19); }
    } else if (kind == QStringLiteral("transform")) {
      painter.setBrush(effect.color.lighter(170)); painter.drawPolygon(QPolygonF{QPointF(9, 3), QPointF(18, 15), QPointF(9, 27), QPointF(0, 15)});
      painter.setBrush(effect.color); painter.drawPolygon(QPolygonF{QPointF(22, 5), QPointF(31, 17), QPointF(22, 29), QPointF(13, 17)});
      painter.setPen(QPen(Qt::white, 2)); line(18, 17, 26, 17); line(22, 13, 22, 21);
    } else if (kind == QStringLiteral("empowerment")) {
      painter.setBrush(effect.color.lighter(170)); painter.drawEllipse(QRectF(1, 3, 11, 11));
      painter.setBrush(effect.color); painter.drawEllipse(QRectF(18, 18, 13, 13));
      painter.setBrush(Qt::NoBrush); painter.setPen(QPen(effect.color, 2.7));
      QPainterPath transfer; transfer.moveTo(9, 15); transfer.quadTo(8, 24, 17, 25); painter.drawPath(transfer);
      line(17, 25, 12, 20); line(17, 25, 11, 28);
      painter.setPen(QPen(effect.color, 1.5)); line(23, 1, 23, 11); line(18, 6, 28, 6);
    } else if (kind == QStringLiteral("attributes") || kind == QStringLiteral("self-transform")) {
      const QList<QColor> colors{QColor("#dc7661"), QColor("#6194d3"), QColor("#7ba655"), QColor("#a876c0")};
      for (int i = 0; i < 4; ++i) { painter.setPen(QPen(Qt::white, 1.3)); painter.setBrush(colors[i]); painter.drawPie(QRectF(3, 3, 26, 26), (45 + i * 90) * 16, 90 * 16); }
      painter.setBrush(Qt::white); painter.drawEllipse(QRectF(12, 12, 8, 8));
      if (kind == QStringLiteral("self-transform")) {
        painter.setBrush(Qt::NoBrush); painter.setPen(QPen(effect.color.darker(125), 2)); painter.drawArc(QRectF(1, 1, 30, 30), 40 * 16, 260 * 16);
        painter.setBrush(effect.color); painter.setPen(Qt::NoPen); painter.drawPolygon(QPolygonF{QPointF(27, 3), QPointF(30, 12), QPointF(22, 9)});
      }
    } else if (kind == QStringLiteral("reveal")) {
      painter.setBrush(effect.color.lighter(170)); QPainterPath eye; eye.moveTo(1, 16); eye.quadTo(16, 0, 31, 16); eye.quadTo(16, 32, 1, 16); painter.drawPath(eye);
      painter.setBrush(effect.color); painter.drawEllipse(QRectF(11, 10, 10, 12));
    } else if (kind == QStringLiteral("affinity-strong")) {
      painter.drawPolygon(QPolygonF{QPointF(18, 1), QPointF(6, 18), QPointF(14, 18), QPointF(12, 31), QPointF(27, 12), QPointF(19, 12)});
    } else if (kind == QStringLiteral("affinity-weak")) {
      painter.setBrush(Qt::NoBrush); painter.setPen(QPen(effect.color, 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      line(6, 8, 16, 17); line(16, 17, 26, 8); line(6, 17, 16, 26); line(16, 26, 26, 17);
    } else if (kind == QStringLiteral("god-job-attack")) {
      painter.setBrush(effect.color.lighter(170)); painter.drawEllipse(QRectF(1, 1, 30, 30)); painter.setBrush(effect.color);
      painter.drawPolygon(QPolygonF{QPointF(23, 1), QPointF(29, 1), QPointF(29, 7), QPointF(12, 24), QPointF(6, 18)});
      painter.setPen(QPen(effect.color.darker(130), 3)); line(3, 17, 15, 29); line(2, 30, 8, 24);
      painter.setPen(QPen(Qt::white, 1.3)); line(21, 5, 25, 5); line(23, 3, 23, 7);
    } else if (kind == QStringLiteral("god-job-defend")) {
      QPainterPath shield; shield.moveTo(16, 1); shield.lineTo(29, 7); shield.lineTo(26, 22); shield.lineTo(16, 31); shield.lineTo(6, 22); shield.lineTo(3, 7); shield.closeSubpath(); painter.drawPath(shield);
      painter.setPen(QPen(Qt::white, 1)); painter.setBrush(Qt::white); star(5, 8, 3.4);
    } else if (kind == QStringLiteral("tianqi")) {
      painter.setBrush(effect.color.lighter(160)); painter.drawEllipse(QRectF(3, 1, 26, 10));
      painter.setBrush(effect.color); painter.drawPolygon(QPolygonF{QPointF(16, 9), QPointF(24, 19), QPointF(21, 29), QPointF(11, 29), QPointF(8, 19)});
      painter.drawPolygon(QPolygonF{QPointF(1, 12), QPointF(12, 19), QPointF(10, 28), QPointF(4, 24)});
      painter.drawPolygon(QPolygonF{QPointF(31, 12), QPointF(20, 19), QPointF(22, 28), QPointF(28, 24)});
      painter.setPen(QPen(Qt::white, 2)); line(16, 15, 16, 25); line(12, 20, 20, 20);
    } else {
      painter.setBrush(effect.color.lighter(165)); painter.setPen(QPen(effect.color, 2, effect.derived || kind == QStringLiteral("unverified") ? Qt::DashLine : Qt::SolidLine));
      painter.drawPolygon(QPolygonF{QPointF(16, 2), QPointF(30, 16), QPointF(16, 30), QPointF(2, 16)});
      painter.setPen(effect.color.darker(115)); auto font = painter.font(); font.setPixelSize(21); font.setBold(true); painter.setFont(font);
      painter.drawText(QRectF(6, 4, 20, 24), Qt::AlignCenter, kind == QStringLiteral("unverified") ? QStringLiteral("?") : QStringLiteral("!"));
    }
    // These small accents distinguish a configured entered state from an
    // actual numeric source annotation when their pictogram is similar.
    if (effect.kind == QStringLiteral("harm-source")) {
      painter.setPen(QPen(effect.color.darker(125), 2.3)); line(2, 29, 9, 29); line(6, 25, 9, 29); line(6, 32, 9, 29);
    }
    painter.restore();
  }
  QList<OutcomeVisual> effects_;
  QStringList paintedLines_;
  QList<QRectF> iconRects_;
  QList<int> visibleIndices_;
  QRectF overflowRect_;
  std::function<void()> pause_;
  QVariantAnimation* animation_ = nullptr;
  QPointer<UnitMiniRow> summary_;
  QPointer<QWidget> viewport_, compactHost_, compactAnchor_;
  bool animated_ = false, playbackDriven_ = false, compact_ = false;
  qreal progress_ = 1;
  int current_ = 0, selected_ = -1, settled_ = -1, ownTeam_ = 0;
};

class ResultBadges final : public QLabel {
public:
  explicit ResultBadges(QWidget* parent) : QLabel(parent) {}
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    auto f = font(); f.setPixelSize(10); f.setWeight(QFont::DemiBold); painter.setFont(f);
    const auto metrics = QFontMetrics(f);
    const auto labels = text().split(QStringLiteral(" · "), Qt::SkipEmptyParts);
    const bool clientHint = property("clientPresentation").toBool();
    int x = 0;
    const auto draw = [&](const QString& label, const QColor& color, int w) {
      const QRect chip(x, 1, w, height() - 2);
      painter.setPen(clientHint ? QPen(QColor("#cbbfe0")) : QPen(Qt::NoPen));
      painter.setBrush(clientHint ? QColor("#eee8f6") : color); painter.drawRoundedRect(chip, 4, 4);
      painter.setPen(clientHint ? QColor("#665082") : QColor(Qt::white)); painter.drawText(chip.adjusted(3, 0, -3, 0), Qt::AlignCenter, label);
    };
    for (int i = 0; i < labels.size(); ++i) {
      const auto more = QStringLiteral("+%1").arg(labels.size() - i);
      const int reserve = i + 1 < labels.size() ? metrics.horizontalAdvance(more) + 10 : 0;
      const int desired = metrics.horizontalAdvance(labels[i]) + 8;
      if (x + desired + reserve > width() && i > 0) {
        draw(more, QColor("#65748a"), std::max(0, std::min(width() - x, metrics.horizontalAdvance(more) + 8))); break;
      }
      const int w = std::max(0, std::min(desired, width() - x - reserve));
      const auto background = property("clientPresentation").toBool() ? QColor("#776496") : labels[i] == QStringLiteral("免疫") ? QColor("#7555a6") : labels[i] == QStringLiteral("格挡") ? QColor("#246ca5") :
        labels[i] == QStringLiteral("被暴击") ? QColor("#b63851") : QColor("#646b77");
      draw(metrics.elidedText(labels[i], Qt::ElideRight, std::max(0, w - 6)), background, w);
      x += w + 3;
    }
  }
};
}

ReplayCell::ReplayCell(int side, QWidget* parent) : QFrame(parent) {
  setMinimumWidth(148); setFixedHeight(204);
  setProperty("playbackDriven", false); setProperty("playbackProgress", 1.0);
  setGraphicsEffect(new CellEmphasis(this)); graphicsEffect()->setEnabled(false);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setStyleSheet(QStringLiteral("ReplayCell{background:%1;border:1px solid %2;border-radius:5px}")
    .arg(side ? QStringLiteral("#fff8fa") : QStringLiteral("#f4f9ff"), side ? QStringLiteral("#ead8df") : QStringLiteral("#d1e2f2")));
  auto* box = new QVBoxLayout(this); box->setContentsMargins(7, 4, 7, 5); box->setSpacing(2);
  heading_ = new QWidget(this);
  auto* heading = new QHBoxLayout(heading_); heading->setContentsMargins(0, 0, 0, 0); heading->setSpacing(2);
  occupants_ = new CompactLabel(heading_); occupants_->setObjectName(QStringLiteral("cellOtherUnits"));
  occupants_->setStyleSheet(QStringLiteral("color:#687990;font-size:10px"));
  heading->addWidget(occupants_, 1);
  unitSwitcher_ = new QToolButton(this); unitSwitcher_->setObjectName(QStringLiteral("cellUnitSwitcher"));
  unitSwitcher_->setFixedSize(48, 16); unitSwitcher_->setPopupMode(QToolButton::InstantPopup);
  unitSwitcher_->setStyleSheet(QStringLiteral("QToolButton{border:0;border-radius:2px;background:#e8edf4;color:#5b6c81;font-size:10px;padding-right:8px} QToolButton::menu-indicator{width:7px}"));
  unitSwitcher_->setMenu(new QMenu(unitSwitcher_)); unitSwitcher_->hide(); heading->addWidget(unitSwitcher_);
  box->addWidget(heading_); heading_->hide();
  unitRows_ = new QWidget(this); unitRows_->setObjectName(QStringLiteral("cellUnitRows"));
  auto* rows = new QGridLayout(unitRows_); rows->setContentsMargins(0, 0, 0, 0); rows->setSpacing(0);
  unitRows_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  box->addWidget(unitRows_); unitRows_->hide();
  stack_ = new QStackedWidget(this); stack_->setObjectName(QStringLiteral("cellUnits"));
  stack_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  box->addWidget(stack_, 1);
}

void ReplayCell::display(int slot, const QList<BattleReplay::Fighter>& fighters, const QJsonObject& catalog,
                         const QString& actor, const QSet<QString>& changed,
                         const QString& source, const QString& target, const QString& targetText,
                         const QSet<QString>& empowered, const QMap<QString, ReplayUnitOverview>& overview, int ownTeam) {
  setProperty("formationSlot", slot);
  meterChanges_.clear();
  QStringList plain;
  setProperty("actorEmphasis", false); setProperty("deadUnit", false); setProperty("targetEmphasis", false); setProperty("visualScale", 1.0);
  static_cast<CellEmphasis*>(graphicsEffect())->setState(false, false);
  hasSummoned_ = hasEmpowered_ = false;
  bool summonedEntered = false;
  for (const auto& f : fighters) {
    hasSummoned_ = hasSummoned_ || f.summoned;
    summonedEntered = summonedEntered || (f.summoned && (f.entered || f.displayLayersRestored));
    hasEmpowered_ = hasEmpowered_ || empowered.contains(f.key);
  }
  summonPending_ = hasSummoned_ && !summonedEntered;
  setProperty("summonPending", summonPending_);
  QStringList cellTips;
  if (hasSummoned_) cellTips.append(summonPending_ ? QStringLiteral("绿虚线：召唤单位尚未登场") : QStringLiteral("绿框：包含召唤单位"));
  if (hasEmpowered_) cellTips.append(QStringLiteral("粉框：元能技指定单位；官方配置与开局属性交叉核对"));
  if (fighters.size() > 1) cellTips.append(QStringLiteral("同格 %1 名；点击各自名称条切换下方详情。每条连线对应具体单位，状态、数值和 Buff 独立保留。").arg(fighters.size()));
  if (fighters.size() > 1) plain.append(QStringLiteral("同格 %1 名").arg(fighters.size()));
  setToolTip(cellTips.join(QLatin1Char('\n')));
  unitSwitcher_->menu()->clear();
  // The workbench raises generic button heights at page creation. Keep this
  // compact selector from shifting every meter when a cell has two units.
  unitSwitcher_->setFixedSize(48, 16);
  unitSwitcher_->hide(); heading_->hide();
  unitRows_->setVisible(fighters.size() > 1);
  unitSwitcher_->setText(QStringLiteral("同格%1").arg(fighters.size()));
  unitSwitcher_->setToolTip(QStringLiteral("切换查看同格单位，包括已退场、待召唤与分身"));
  unitNames_.clear(); unitCards_.clear(); unitAnchors_.clear();
  while (auto* item = unitRows_->layout()->takeAt(0)) { delete item->widget(); delete item; }
  delete unitRows_->layout();
  auto* rows = new QGridLayout(unitRows_); rows->setContentsMargins(0, 0, 0, 0); rows->setSpacing(0);
  occupants_->clear(); occupants_->setToolTip({});
  while (stack_->count()) { auto* old = stack_->widget(0); stack_->removeWidget(old); delete old; }
  if (fighters.isEmpty()) {
    auto* empty = new QLabel(QStringLiteral("空位"), stack_); empty->setAlignment(Qt::AlignCenter);
    empty->setStyleSheet(QStringLiteral("color:#98a4b3;font-size:12px")); stack_->addWidget(empty);
    plain.append(empty->text());
  }
  const auto buffCatalog = catalog[QStringLiteral("buffs")].toObject();
  // Presentation priority only: every original unit remains available.
  auto displayed = fighters;
  const auto priority = [&](const BattleReplay::Fighter& f) {
    if ((!target.isEmpty() && f.key == target) || overview.value(f.key).target) return 4;
    if (!actor.isEmpty() && f.key == actor) return 3;
    if (!source.isEmpty() && f.key == source) return 2;
    return BattleReplay::presentedOnField(f) && (!f.hp || *f.hp > 0) ? 1 : 0;
  };
  std::stable_sort(displayed.begin(), displayed.end(), [&](const auto& a, const auto& b) { return priority(a) > priority(b); });
  for (const auto& f : displayed) {
    const auto change = overview.value(f.key);
    const bool isSource = !source.isEmpty() && f.key == source;
    const bool isTarget = (!target.isEmpty() && f.key == target) || change.target;
    auto* card = new QFrame(stack_); card->setObjectName(QStringLiteral("replayUnitCard"));
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    card->setProperty("fighterKey", f.key);
    card->setProperty("isActor", f.key == actor);
    card->setProperty("isDead", BattleReplay::presentedOnField(f) && f.hp && *f.hp <= 0);
    card->setProperty("entryConfirmed", f.entered);
    card->setProperty("displayLayersRestored", f.displayLayersRestored);
    // An announced target alone is not yet an impact. This prevents the
    // wind-up phase from flashing a result that has not happened yet.
    card->setProperty("isImpactTarget", isTarget && change.animate && (!change.events.isEmpty() || !change.badge.isEmpty()));
    if (fighters.size() > 1) {
      auto* effect = new CellEmphasis(card); card->setGraphicsEffect(effect);
      effect->setState(false, card->property("isDead").toBool(), card->property("isImpactTarget").toBool());
      effect->setPulse(playbackDriven_ && playbackProgress_ < 1 ? std::sin(playbackProgress_ * 3.14159265358979323846) : 0);
    }
    QStringList otherNames, otherCounts;
    QMap<QString, int> counts;
    for (const auto& other : fighters) if (other.key != f.key) {
      const auto status = occupantState(other); ++counts[status];
      otherNames.append(replayFighterName(other, catalog) + QStringLiteral(" · ") + status +
        QStringLiteral(" · 生命 %1/%2").arg(digits(other.hp), digits(other.maxHp)));
    }
    for (const auto& status : {QStringLiteral("存活"), QStringLiteral("待召唤"), QStringLiteral("待分身"),
        QStringLiteral("阵亡"), QStringLiteral("未登场"), QStringLiteral("已移除"), QStringLiteral("生命未记"), QStringLiteral("状态未记")})
      if (counts.value(status)) otherCounts.append(status + QString::number(counts.value(status)));
    card->setProperty("otherUnitsSummary", otherNames.isEmpty() ? QString() : QStringLiteral("另%1 · %2").arg(otherNames.size()).arg(otherCounts.join(QLatin1Char(' '))));
    card->setProperty("otherUnitsDetail", otherNames.join(QLatin1Char('\n')));
    const auto background = isSource && isTarget ? QStringLiteral("#f0eafa") : isTarget ? QStringLiteral("#fff0e3") : isSource || f.key == actor ? QStringLiteral("#e3efff") : QStringLiteral("transparent");
    card->setStyleSheet(QStringLiteral("QFrame#replayUnitCard{background:%1;border:0;border-radius:6px}").arg(background));
    auto* box = new QVBoxLayout(card); box->setContentsMargins(2, 0, 2, 1); box->setSpacing(1);
    const auto currentName = replayFighterName(f, catalog);
    const bool transformed = currentName != f.name || (!f.formPath.isEmpty() && f.formPath != f.path);
    const auto originText = transformed ? QStringLiteral("\n原精灵：") + replayPublicFighterNameText(f.name) : QString();
    const auto lineage = f.originRaceId.isEmpty() ? QString() :
      (f.originRelation == QStringLiteral("chaser") ? QStringLiteral("关联本体：") : QStringLiteral("变身来源：")) + replayPublicReferenceName(catalog, QStringLiteral("pets"), f.originRaceId);
    card->setProperty("originRaceId", f.originRaceId); card->setProperty("originFighterKey", f.originFighterKey);
    const bool normalReplaced = !f.normalSkill.isEmpty() && f.raw.contains(QStringLiteral("nskill")) && f.normalSkill != BattleReplay::valueText(f.raw[QStringLiteral("nskill")]);
    const bool uniqueReplaced = !f.uniqueSkill.isEmpty() && f.raw.contains(QStringLiteral("uskills")) && f.uniqueSkill != BattleReplay::valueText(f.raw[QStringLiteral("uskills")]);
    const bool skillsReplaced = normalReplaced || uniqueReplaced;
    const auto currentSkills = QStringLiteral("当前普通技能：%1\n当前超杀：%2").arg(skillName(catalog, f.normalSkill), skillName(catalog, f.uniqueSkill));
    const auto nameText = (f.key == actor ? QStringLiteral("[行动] ") : QString()) + currentName + originText;
    const bool absent = !BattleReplay::presentedOnField(f) || (f.hp && *f.hp <= 0);
    const QColor nameColor(absent ? QStringLiteral("#79879a") : isSource || f.key == actor ? QStringLiteral("#155cab") : isTarget || changed.contains(f.key) ? QStringLiteral("#9e3251") : QStringLiteral("#293a4c"));
    auto* name = new WrappedNameButton(currentName, nameColor, card);
    name->setObjectName(QStringLiteral("unitName")); name->setProperty("fighterKey", f.key);
    name->setProperty("rawEvidence", replayFighterNameEvidence(f, catalog) + QLatin1Char('\n') + f.originPath);
    name->setToolTip(currentName + originText + (lineage.isEmpty() ? QString() : QLatin1Char('\n') + lineage) +
      QLatin1Char('\n') + currentSkills + QStringLiteral("\n点击查看精灵、技能与效果详情"));
    connect(name, &QPushButton::clicked, this, [this, key = f.key] { emit unitActivated(key); });
    box->addWidget(name); unitNames_.insert(f.key, name); plain.append(nameText);
    if (fighters.size() > 1) {
      name->hide();
      auto* mini = new UnitMiniRow(currentName, unitRows_);
      mini->actor = f.key == actor; mini->target = isTarget;
      mini->dead = card->property("isDead").toBool(); mini->pending = !BattleReplay::presentedOnField(f);
      mini->presence = occupantState(f);
      if (mini->actor) mini->role = QStringLiteral("行动");
      if (isSource && !mini->actor) mini->role = QStringLiteral("来源");
      if (isTarget) mini->role += (mini->role.isEmpty() ? QString() : QStringLiteral("·")) + QStringLiteral("目标");
      QStringList recorded;
      int added = 0, removed = 0, triggered = 0;
      for (const auto& event : change.events) {
        if (!event.metric.isEmpty()) recorded.append(event.metric + event.valueText + (event.harmLabel.isEmpty() ? QString() : QStringLiteral("·") + event.harmLabel));
        else if (!event.buffId.isEmpty()) (event.buffTriggered ? triggered : event.removing ? removed : added)++;
      }
      if (added) recorded.append(QStringLiteral("Buff新增%1条").arg(added));
      if (removed) recorded.append(QStringLiteral("Buff移除%1条").arg(removed));
      if (triggered) recorded.append(QStringLiteral("Buff触发%1条").arg(triggered));
      if (!change.badge.isEmpty()) recorded.append(change.badge);
      mini->changes = recorded.join(QStringLiteral(" · "));
      mini->setProperty("fighterKey", f.key); mini->setProperty("isActor", mini->actor);
      mini->setProperty("isTarget", isTarget); mini->setProperty("isDead", mini->dead);
      mini->setProperty("phaseChanges", mini->changes); mini->setProperty("presenceText", mini->presence);
      mini->setProperty("relationMiniAnchor", true); mini->setProperty("isImpactTarget", card->property("isImpactTarget"));
      mini->setAccessibleName(currentName + QStringLiteral(" · ") + mini->presence + QStringLiteral(" · ") + mini->role +
        (mini->changes.isEmpty() ? QString() : QStringLiteral("\n本阶段：") + mini->changes));
      mini->setToolTip(mini->accessibleName() + QStringLiteral("\n生命 %1/%2；护盾 %3；气势 %4/%5")
        .arg(digits(f.hp), digits(f.maxHp), digits(f.shield), digits(f.power), digits(f.maxPower)) +
        QStringLiteral("\n点击切换下方完整状态与 Buff；双击查看精灵资料。• 表示本阶段有已记录变化。") + originText);
      mini->inspect = [this, key = f.key] { emit unitActivated(key); };
      connect(mini, &QPushButton::clicked, this, [this, key = f.key] { focusUnit(key); emit unitSelectionChanged(key); });
      int order = 0;
      for (const auto& original : fighters) { if (original.key == f.key) break; ++order; }
      const int columns = fighters.size() > 4 ? 2 : 1;
      rows->addWidget(mini, order / columns, order % columns);
      unitAnchors_.insert(f.key, mini);
    } else unitAnchors_.insert(f.key, this);
    if (!otherNames.isEmpty()) plain.append(QStringLiteral("同格其他单位：") + otherCounts.join(QStringLiteral(" · ")));
    auto* choice = unitSwitcher_->menu()->addAction(currentName + (f.presence.isEmpty() ? QString() : QStringLiteral(" · ") + f.presence));
    choice->setData(f.key); choice->setCheckable(true);
    connect(choice, &QAction::triggered, this, [this, key = f.key] { focusUnit(key); });

    QStringList tags, tagText;
    if (f.key == actor) { tags.append(roleTag(QStringLiteral("正在行动"), QStringLiteral("#286ac2"), QStringLiteral("#ffffff"))); tagText.append(QStringLiteral("正在行动")); }
    if (isSource && f.key != actor) { tags.append(roleTag(QStringLiteral("作用来源"), QStringLiteral("#286ac2"), QStringLiteral("#ffffff"))); tagText.append(QStringLiteral("作用来源")); }
    if (isTarget) {
      const auto text = f.key == actor ? QStringLiteral("目标·自身") : QStringLiteral("目标");
      tags.append(roleTag(text, QStringLiteral("#ba5a28"), QStringLiteral("#ffffff"))); tagText.append(text);
    }
    if (f.summoned) {
      const auto text = f.entered || f.displayLayersRestored ? QStringLiteral("召唤") : QStringLiteral("待召唤");
      tags.append(roleTag(text, QStringLiteral("#daf1e3"), QStringLiteral("#267448"))); tagText.append(text);
    }
    if (empowered.contains(f.key)) { tags.append(roleTag(QStringLiteral("元能"), QStringLiteral("#fce0ec"), QStringLiteral("#b1487a"))); tagText.append(QStringLiteral("元能")); }
    if (f.clone) { tags.append(roleTag(QStringLiteral("分身"), QStringLiteral("#e8e4fa"), QStringLiteral("#6c51a0"))); tagText.append(QStringLiteral("分身")); }
    if (f.chaser) { tags.append(roleTag(QStringLiteral("追击"), QStringLiteral("#e1edf4"), QStringLiteral("#416f88"))); tagText.append(QStringLiteral("追击")); }
    if (BattleReplay::presentedOnField(f) && f.hp && *f.hp <= 0) { tags.append(roleTag(QStringLiteral("已阵亡"), QStringLiteral("#65748a"), QStringLiteral("#ffffff"))); tagText.append(QStringLiteral("已阵亡")); }
    if (transformed) { tags.append(roleTag(QStringLiteral("变身"), QStringLiteral("#e8e4fa"), QStringLiteral("#6c51a0"))); tagText.append(QStringLiteral("变身")); }
    if (skillsReplaced) { tags.append(roleTag(QStringLiteral("技能已换"), QStringLiteral("#eee6fb"), QStringLiteral("#71528e"))); tagText.append(QStringLiteral("技能已换")); }
    {
      auto* statusLine = new QWidget(card); statusLine->setObjectName(QStringLiteral("unitStatusLine")); statusLine->setFixedHeight(14);
      auto* line = new QHBoxLayout(statusLine); line->setContentsMargins(0, 0, 0, 0); line->setSpacing(3);
      const auto attributes = attributeNames(f, catalog);
      auto* attribute = new CompactLabel(statusLine); attribute->setObjectName(QStringLiteral("unitAttributes"));
      attribute->setText(attributes); attribute->setAccessibleName(QStringLiteral("当前属性：") + attributes);
      attribute->setToolTip(QStringLiteral("当前属性：") + attributes); attribute->setProperty("rawEvidence", f.attributesPath);
      attribute->setStyleSheet(QStringLiteral("color:#5c5484;font-size:10px;font-weight:600"));
      line->addWidget(attribute, 1); plain.append(attribute->accessibleName());
      auto* roles = new QLabel(tags.join(QLatin1Char(' ')), statusLine);
      roles->setObjectName(QStringLiteral("unitRoles")); roles->setTextFormat(Qt::RichText); roles->setFixedHeight(14);
      roles->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
      roles->setToolTip(tagText.join(QStringLiteral(" · ")) + originText + QLatin1Char('\n') + f.presence + QLatin1Char('\n') + currentSkills);
      roles->setAccessibleName(tagText.join(QStringLiteral(" · ")));
      roles->setProperty("rawEvidence", f.displayLayersRestored
        ? QStringLiteral("官方显示恢复：%1\n独立入场标记：%2\n入场依据：%3")
            .arg(f.displayLayersPath, f.entered ? QStringLiteral("是") : QStringLiteral("否"), f.presencePath)
        : f.presencePath);
      roles->setStyleSheet(QStringLiteral("font-size:10px")); line->addWidget(roles, 1); box->addWidget(statusLine);
      plain.append(tagText.join(QStringLiteral(" · ")));
      roles->setVisible(!tags.isEmpty());
    }
    const bool criticalOnDamage = criticalBesideDamage(change);
    card->setProperty("criticalBesideDamage", criticalOnDamage);
    const auto visibleBadge = criticalOnDamage ? withoutCriticalBadge(change.badge) : change.badge;
    const auto visibleClientBadge = criticalOnDamage ? withoutCriticalBadge(change.clientBadge) : change.clientBadge;
    if (criticalOnDamage && visibleBadge.isEmpty() && !change.badge.isEmpty()) plain.append(change.badge);
    if (criticalOnDamage && visibleClientBadge.isEmpty() && !change.clientBadge.isEmpty())
      plain.append(QStringLiteral("官方客户端提示：") + change.clientBadge);
    QHBoxLayout* judgmentLayout = nullptr;
    if (!visibleBadge.isEmpty() || !visibleClientBadge.isEmpty()) {
      auto* judgmentLine = new QWidget(card); judgmentLine->setObjectName(QStringLiteral("unitJudgmentLine"));
      judgmentLine->setFixedHeight(16);
      judgmentLayout = new QHBoxLayout(judgmentLine); judgmentLayout->setContentsMargins(0, 0, 0, 0); judgmentLayout->setSpacing(3);
      box->addWidget(judgmentLine);
    }
    if (!visibleBadge.isEmpty()) {
      auto* result = new ResultBadges(judgmentLayout->parentWidget()); result->setText(visibleBadge); result->setObjectName(QStringLiteral("unitResultBadges"));
      result->setWordWrap(false); result->setTextFormat(Qt::PlainText); result->setFixedHeight(16);
      result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
      result->setStyleSheet(QStringLiteral("font-size:11px;font-weight:700"));
      result->setToolTip(change.badge + (change.badge.contains(QStringLiteral("状态待核实"))
        ? QStringLiteral("\n当前日志含尚无法解释的状态记录，相关余额或状态保留为未知；原文仍可查看。")
        : QStringLiteral("\n日志直接记录的目标判定。格挡不等于没有伤害；技能未生效不否定前后置的 Buff 或数值变化。")));
      result->setAccessibleName(change.badge);
      judgmentLayout->addWidget(result, std::max(1, static_cast<int>(change.badge.size()))); plain.append(change.badge);
    }
    if (isTarget && !targetText.isEmpty()) plain.append(targetText);

    if (!visibleClientBadge.isEmpty()) {
      auto* hints = new ResultBadges(judgmentLayout->parentWidget()); hints->setObjectName(QStringLiteral("unitClientHints"));
      hints->setProperty("clientPresentation", true); hints->setText(visibleClientBadge);
      hints->setTextFormat(Qt::PlainText); hints->setFixedHeight(16);
      hints->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
      hints->setToolTip(QStringLiteral("官方显示提示：") + change.clientBadge + QStringLiteral("\n点击精灵查看完整说明。"));
      hints->setProperty("rawEvidence", change.clientDetail);
      hints->setAccessibleName(QStringLiteral("官方显示提示：") + change.clientBadge);
      judgmentLayout->addWidget(hints, std::max(1, static_cast<int>(change.clientBadge.size()))); plain.append(hints->accessibleName());
    }

    const auto& meterEvents = change.hasActionMeterContext ? change.actionMeterEvents : change.events;
    const auto finishMeter = [&](ValueBar* bar, const QString& metric, std::optional<qint64> before,
                                  std::optional<qint64> beforeReference) {
      bar->setProperty("fighterKey", f.key); bar->setProperty("metric", metric);
      bar->setProperty("displayName", currentName);
      bar->setGroupedNumbers(groupedNumbers_);
      bar->activateRecorded = [this](int step, int index) { emit recordedEffectActivated(step, index); };
      if (!change.hasActionMeterContext) return;
      QList<ReplayUnitEvent> active;
      for (const auto& event : change.activeMeterEvents ? *change.activeMeterEvents : change.actionMeterEvents)
        if (event.metric == metric) active.append(event);
      bar->setActionMeter(change.actionMetersPending, before, beforeReference, metric, active);
      if (change.actionMetersPending) meterChanges_.append({bar, f.key, metric, active});
    };
    auto* hp = meter(card, QStringLiteral("hpBar"), QStringLiteral("生命"), f.hp, f.maxHp, QStringLiteral("#eb7d85"), QStringLiteral("#fae6e8"), QStringLiteral("生命：当前生命 / 当前生命上限"));
    hp->setEvents(meterEvents, QStringLiteral("生命")); hp->activate = [this](int i) { emit effectActivated(i); };
    hp->setDiagnostic(!f.hp || !f.maxHp, [this, key = f.key] { emit diagnosticActivated(key); });
    finishMeter(hp, QStringLiteral("生命"), change.actionHpBefore, change.actionMaxHpBefore);
    if (!change.hasActionMeterContext && change.animate && f.hp && f.maxHp && *f.maxHp > 0) hp->animateFrom(change.hpBefore, change.maxHpBefore, change.transitionMs, playbackDriven_, playbackProgress_);
    box->addWidget(hp); plain.append(hp->accessibleName());
    auto* shield = meter(card, QStringLiteral("shieldValue"), QStringLiteral("护盾"), {}, {}, QStringLiteral("#bbdaf7"), QStringLiteral("#e4f0fe"), {});
    shield->setNumberText(QStringLiteral("护盾 %1").arg(digits(f.shield))); shield->setValue(shield->maximum());
    shield->setRecordedBalance(f.shield, {});
    shield->setAccessibleName(shield->format());
    shield->setToolTip(QStringLiteral("当前已记录护盾；? 表示尚无可靠数值。"));
    shield->setEvents(meterEvents, QStringLiteral("护盾")); shield->activate = [this](int i) { emit effectActivated(i); };
    shield->setDiagnostic(!f.shield, [this, key = f.key] { emit diagnosticActivated(key); });
    finishMeter(shield, QStringLiteral("护盾"), change.actionShieldBefore, {});
    box->addWidget(shield); plain.append(shield->accessibleName());
    auto* power = meter(card, QStringLiteral("powerBar"), QStringLiteral("气势"), f.power, f.maxPower, QStringLiteral("#f3b35f"), QStringLiteral("#fbeed9"), QStringLiteral("气势：当前气势 / 官方满气势值，分母不是数值封顶。"));
    const bool fullPower = f.power && f.maxPower && *f.maxPower > 0 && *f.power >= *f.maxPower;
    power->setProperty("powerFull", fullPower);
    const auto fullPowerDefinition = ReplayReference::powerFull(catalog, f.faceId);
    const auto fullPowerBuffId = ReplayReference::text(fullPowerDefinition[QStringLiteral("buffId")]);
    power->setProperty("officialPowerFullBuffId", fullPowerBuffId);
    power->setProperty("officialPowerFullFaceId", f.faceId);
    if (fullPower) power->setMarker(QStringLiteral("满"), QStringLiteral("满气势：已达到满气势值；是否施放以战斗记录为准。"));
    power->setEvents(meterEvents, QStringLiteral("气势")); power->activate = [this](int i) { emit effectActivated(i); };
    power->setDiagnostic(!f.power || !f.maxPower, [this, key = f.key] { emit diagnosticActivated(key); });
    finishMeter(power, QStringLiteral("气势"), change.actionPowerBefore, change.actionMaxPowerBefore);
    if (!change.hasActionMeterContext && change.animate && f.power && f.maxPower && *f.maxPower > 0) power->animateFrom(change.powerBefore, change.maxPowerBefore, change.transitionMs, playbackDriven_, playbackProgress_);
    box->addWidget(power); plain.append(power->accessibleName());
    plain.append(f.presence); card->setProperty("rawEvidence", change.detail);
    const auto inBar = [](const ReplayUnitEvent& event) {
      return event.metric == QStringLiteral("生命") || event.metric == QStringLiteral("护盾") || event.metric == QStringLiteral("气势");
    };
    for (const auto& event : change.events) {
      if (!event.buffId.isEmpty() && event.metric.isEmpty()) plain.append(publicBuffEventLabel(event, buffCatalog));
      else if (!event.metric.isEmpty()) plain.append(publicNumberDetail(event));
    }

    auto* buffPanel = new QWidget(card); buffPanel->setObjectName(QStringLiteral("unitBuffPanel"));
    auto* panelLayout = new QVBoxLayout(buffPanel); panelLayout->setContentsMargins(0, 0, 0, 0); panelLayout->setSpacing(0);
    auto* buffHeading = new QWidget(buffPanel); buffHeading->setObjectName(QStringLiteral("unitBuffHeading"));
    auto* headingLayout = new QHBoxLayout(buffHeading); headingLayout->setContentsMargins(0, 0, 0, 0); headingLayout->setSpacing(2);
    auto* buffSummary = new CompactLabel(buffHeading); buffSummary->setObjectName(QStringLiteral("unitBuffSummary"));
    buffSummary->setStyleSheet(QStringLiteral("color:#66778b;font-size:10px;font-weight:600"));
    auto* buffToggle = new QToolButton(buffHeading); buffToggle->setObjectName(QStringLiteral("unitBuffToggle"));
    buffToggle->setProperty("fighterKey", f.key); buffToggle->setCheckable(true); buffToggle->setChecked(expandedBuffKeys_.contains(f.key));
    buffToggle->setCursor(Qt::PointingHandCursor); buffToggle->setFixedWidth(42);
    buffToggle->setStyleSheet(QStringLiteral("QToolButton{border:0;border-radius:5px;padding:0 3px;color:#365e8b;background:#e9f0f9;font-size:10px} QToolButton:hover{background:#dce8f7} QToolButton:checked{background:#dce8f7;color:#244d80}"));
    headingLayout->addWidget(buffSummary, 1); headingLayout->addWidget(buffToggle);
    panelLayout->addWidget(buffHeading);
    auto* buffs = new BuffBrowser(buffPanel); buffs->setObjectName(QStringLiteral("unitBuffs"));
    buffs->setProperty("fighterKey", f.key);
    const auto groups = buffGroups(f);
    const auto& buffEvents = change.roundBuffEvents ? *change.roundBuffEvents : change.events;
    const auto eventGroups = buffEventGroups(buffEvents);
    const auto unexplained = [&](const QString& id) {
      return !replayBuffHasExplanation(buffCatalog[id].toObject());
    };
    const auto unknownEvent = [&](const ReplayUnitEvent& event) {
      return !event.buffTriggered && !event.restoresDisplayLayers && unexplained(event.buffId);
    };
    buffs->setProperty("effectRound", change.buffRound);
    buffs->setProperty("roundEffectCount", change.roundBuffEvents ? change.roundBuffEvents->size() : 0);
    int memberCount = 0, hiddenCount = 0, unlistedCount = 0;
    int sourceCounts[3] = {0, 0, 0};
    int primaryEventCount = 0, unknownEventCount = 0;
    QStringList buffText;
    QVariantList recordEvidence, memberEvidence;
    QMap<QString, QList<ReplayUnitEvent>> eventMenus;
    const auto eventDocument = [&](bool unknown) {
      QString html;
      int added = 0, removed = 0, triggered = 0;
      for (const auto& group : eventGroups) {
        const auto& event = group.events.first();
        if (unknownEvent(event) != unknown) continue;
        (event.buffTriggered ? triggered : event.removing ? removed : added) += group.events.size();
      }
      (unknown ? unknownEventCount : primaryEventCount) = added + removed + triggered;
      if (added + removed + triggered == 0) return html;
      QStringList changes;
      if (added) changes.append(QStringLiteral("＋%1").arg(added));
      if (removed) changes.append(QStringLiteral("−%1").arg(removed));
      if (triggered) changes.append(QStringLiteral("触发%1条").arg(triggered));
      html += QStringLiteral("<b style='color:#586f8b'>%1 %2</b><br>")
        .arg(change.roundBuffEvents ? QStringLiteral("本回合") : QStringLiteral("本次"), changes.join(QLatin1Char(' ')));
      for (int side = 0; side < 3; ++side) {
        bool headed = false;
        for (int eventGroupIndex = 0; eventGroupIndex < eventGroups.size(); ++eventGroupIndex) {
          const auto& group = eventGroups[eventGroupIndex];
          const auto& event = group.events.first();
          if (unknownEvent(event) != unknown ||
              buffSide(event.buffSource, event.buffSourceTeam, ownTeam) != side) continue;
          const auto origin = buffOrigin(event.buffSource, event.buffSourceName, event.buffSourceTeam, ownTeam,
            event.buffSourcePath.isEmpty() ? QStringList() : QStringList{event.buffSourcePath});
          if (!headed) {
            html += QStringLiteral("<span style='color:%1;font-size:10px'>%2</span><br>").arg(origin.color, origin.label);
            headed = true;
          }
          int chosen = 0;
          for (int i = 0; i < group.events.size(); ++i) if (group.events[i].selected) chosen = i;
          for (const auto& original : group.events) {
            html += QStringLiteral("<a name='%1'></a>").arg(buffEventAnchor(original));
            recordEvidence.append(QVariantMap{{"buffId", original.buffId}, {"eventIndex", original.index},
              {"recordStep", original.recordStep}, {"label", original.label}, {"detail", original.detail},
              {"source", original.buffSource}, {"sourceName", original.buffSourceName},
              {"sourceTeam", original.buffSourceTeam}, {"sourcePath", original.buffSourcePath}});
            buffs->tips.insert(buffEventLink(original), QStringLiteral("<qt>%1<br><br>%2%3</qt>")
              .arg(publicBuffEventLabel(original, buffCatalog).toHtmlEscaped() + QStringLiteral("<br>点击回看这次变化与原始详情。"),
                origin.detail.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")),
                unknown ? QStringLiteral("<br>本记录的具体效果未解释；名称不作为判定依据，也不将登记次数当成层数。") : QString()));
          }
          const auto label = publicBuffEventLabel(event, buffCatalog);
          html += QStringLiteral("<a href='%1' style='color:%2;text-decoration:none;%3'>%4</a>")
            .arg(buffEventLink(group.events[chosen]), origin.color,
              group.events[chosen].selected ? QStringLiteral("font-weight:700;background:#fff0cd") : QString(), label.toHtmlEscaped());
          if (group.events.size() > 1) {
            const auto menuLink = QStringLiteral("events:%1").arg(eventGroupIndex);
            eventMenus.insert(menuLink, group.events);
            html += QStringLiteral(" <a href='%1' style='color:%2;text-decoration:none'>%3条记录▾</a>")
              .arg(menuLink, origin.color).arg(group.events.size());
            buffs->tips.insert(menuLink, QStringLiteral("%1 条原始%2记录；点击逐条查看，不表示 Buff 层数。")
              .arg(group.events.size()).arg(event.buffTriggered ? QStringLiteral("触发") : QStringLiteral("变动")));
          }
          html += QStringLiteral("<br>");
        }
      }
      return html;
    };
    QString changeHtml = eventDocument(false);
    const QString unknownEventsHtml = eventDocument(true);
    for (const auto& event : change.events) {
      if (!event.buffId.isEmpty() || inBar(event)) continue;
      changeHtml += QStringLiteral("<a name='event%1' href='effect:%1' style='color:%2;text-decoration:none;background:#fff1d8;%3'>%4</a><br>")
        .arg(event.index).arg(event.color, event.selected ? QStringLiteral("font-weight:700") : QString(), event.label.toHtmlEscaped());
      buffs->tips.insert(QStringLiteral("effect:%1").arg(event.index), event.label.toHtmlEscaped() + QStringLiteral("<br>点击回看这次变化与原始详情。"));
    }
    QStringList fullRows, presentationRows;
    QSet<QString> presentationIds;
    QList<int> sides;
    QList<bool> unknownRows;
    int primaryGroupCount = 0, unknownGroupCount = 0, unknownMemberCount = 0;
    for (int groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
      const auto& group = groups[groupIndex];
      const auto& member = group.member;
      const auto id = member.id; const auto config = buffCatalog[id].toObject();
      const auto buffName = compactBuffName(buffCatalog, id);
      const bool unknown = unexplained(id);
      const auto reason = unknown ? QStringLiteral("具体效果尚未解释；增删、数值变化与显示指令仍按实际记录展示。") : QString();
      unknownRows.append(unknown);
      ++(unknown ? unknownGroupCount : primaryGroupCount);
      if (unknown) unknownMemberCount += group.count;
      const bool hidden = BattleReplay::valueText(config[QStringLiteral("hd")]) == QStringLiteral("1");
      const auto text = buffName + QStringLiteral(" · 记录 %1 条").arg(group.count);
      const auto href = QStringLiteral("buff:") + QString::fromLatin1(QUrl::toPercentEncoding(id)) + QStringLiteral("#member%1").arg(groupIndex);
      const auto origin = buffOrigin(member.source, member.sourceName, member.sourceTeam, ownTeam, group.paths);
      memberEvidence.append(QVariantMap{{"buffId", id}, {"source", member.source}, {"sourceName", member.sourceName},
        {"sourceTeam", member.sourceTeam}, {"paths", group.paths}, {"count", group.count}, {"config", config.toVariantMap()}});
      const int side = buffSide(member.source, member.sourceTeam, ownTeam);
      sides.append(side); sourceCounts[side] += group.count; memberCount += group.count;
      if (hidden) hiddenCount += group.count;
      if (!buffCatalog.contains(id)) unlistedCount += group.count;
      bool added = false;
      for (const auto& event : buffEvents)
        if (event.buffId == id && !event.removing && !event.buffTriggered && event.buffSource == member.source && event.buffSourceTeam == member.sourceTeam) added = true;
      auto label = buffName;
      if (group.count > 1) label += QStringLiteral(" · 记录 %1 条").arg(group.count);
      const auto sourceName = member.sourceName.isEmpty() || member.sourceName == member.source
        ? QStringLiteral("施加者名称未记录") : replayPublicFighterNameText(member.sourceName);
      if (!member.source.isEmpty()) label += QStringLiteral(" ← ") + sourceName;
      fullRows.append(QStringLiteral("<a style='color:%1;text-decoration:none;%2' href='%3'>%4%5</a>")
        .arg(origin.color, added ? QStringLiteral("font-weight:600") : QString(), href.toHtmlEscaped(),
          added ? QStringLiteral("＋ ") : QString(), label.toHtmlEscaped()));
      buffText.append(origin.label + QStringLiteral(" · ") + text +
        (member.source.isEmpty() ? QString() : QStringLiteral(" · ") + sourceName));
      const auto cues = officialBuffCues(config);
      if (!cues.isEmpty()) {
        const auto cueText = cues.join(QStringLiteral(" · "));
        if (cueText != buffName)
          fullRows.last() += QStringLiteral(" <a href='%1' style='color:#74668d;text-decoration:none'>· %2</a>").arg(href.toHtmlEscaped(), cueText.toHtmlEscaped());
        buffText.append(cueText);
      }
      auto description = plainOfficial(replayBuffMessage(config[QStringLiteral("mess")].toString(), currentName));
      if (description.isEmpty()) description = buffCatalog.contains(id)
        ? QStringLiteral("官方独立说明字段为空；点击查看已有定义与关联资料")
        : QStringLiteral("当前已核对的字典未收录此 Buff 配置；战报成员记录仍保留");
      const auto presentation = replayBuffPublicPresentation(config);
      if (!presentation.labels.isEmpty() || unknown) {
        // Presentation settings belong to the definition, not each repeated
        // member or applier. Keep one annotation per ID while every source row
        // and every original change remains independently inspectable above.
        if (!presentation.labels.isEmpty() && !presentationIds.contains(id)) {
          presentationIds.insert(id);
          presentationRows.append(QStringLiteral("<a href='%1' style='color:#687b88;text-decoration:none'>%2：%3</a>")
            .arg(href.toHtmlEscaped(), buffName.toHtmlEscaped(), presentation.labels.join(QStringLiteral(" · ")).toHtmlEscaped()));
        }
        description += QLatin1Char('\n') + reason + QLatin1Char('\n') + presentation.explanations.join(QLatin1Char('\n'));
        buffText.append(presentation.explanations.join(QStringLiteral("；")));
      }
      const auto effectNote = plainOfficial(config[QStringLiteral("beffect")].toString());
      if (!effectNote.isEmpty()) description += QStringLiteral("\n官方效果备注：") + effectNote;
      const auto harmNote = plainOfficial(config[QStringLiteral("bhs")].toString());
      if (!harmNote.isEmpty()) description += QStringLiteral("\n官方伤害来源标注：") + harmNote;
      buffs->tips.insert(href, QStringLiteral("<qt><b>%1</b><br>成员记录 %2 条，不表示层数%3<br>%4<br><br>%5<br>点击查看效果详情。</qt>")
        .arg(buffName.toHtmlEscaped()).arg(group.count)
        .arg(member.source.isEmpty() ? QStringLiteral("；来源未记的条目可能属于不同施加者") : QString(),
          origin.detail.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")),
          description.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"))));
    }
    const auto collection = [&](bool unknown) {
      QString html;
      if (!unknown) {
        html += QStringLiteral("<a name='current-buffs'></a>");
        if (!lineage.isEmpty()) html += QStringLiteral("<span style='color:#62557e;font-size:10px'>%1</span><br>").arg(lineage.toHtmlEscaped());
        html += QStringLiteral("<b style='color:#66778b'>当前集合</b><br>");
        if (!f.buffsKnown) html += QStringLiteral("<span style='color:#98622b'>集合待核实 · 仅列已记条目</span><br>");
        if (!primaryGroupCount) html += primaryGroupCount + unknownGroupCount == 0
          ? (f.buffsKnown ? QStringLiteral("当前无 Buff") : QStringLiteral("暂无已知记录"))
          : QStringLiteral("当前仅有未解释登记");
      } else if (unknownGroupCount) html += QStringLiteral("<b style='color:#66778b'>当前未解释登记</b><br>");
      for (int side = 0; side < 3; ++side) {
        bool headed = false;
        for (int i = 0; i < groups.size(); ++i) {
          if (sides[i] != side || unknownRows[i] != unknown) continue;
          if (!headed) {
            const auto& member = groups[i].member;
            const auto origin = buffOrigin(member.source, member.sourceName, member.sourceTeam, ownTeam, {});
            html += QStringLiteral("<span style='color:%1;font-size:10px'>%2</span><br>").arg(origin.color, origin.label);
            headed = true;
          }
          html += fullRows[i] + QStringLiteral("<br>");
        }
      }
      if (!unknown && (transformed || skillsReplaced)) {
        html += QStringLiteral("<br><b style='color:#71528e'>当前形态与技能</b><br>");
        if (transformed) html += QStringLiteral("变身后：%1<br>原精灵：%2<br>").arg(currentName.toHtmlEscaped(), replayPublicFighterNameText(f.name).toHtmlEscaped());
        html += currentSkills.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
      }
      return html;
    };
    const auto compactHtml = QStringLiteral("<div>") + changeHtml + QStringLiteral("</div>");
    const auto fullHtml = QStringLiteral("<div>") + changeHtml + collection(false) + QStringLiteral("</div>");
    const bool hasUnknownDetails = unknownGroupCount || unknownEventCount;
    const auto unknownHtml = QStringLiteral("<a name='unknown-details'></a><div><b>未解释记录</b><br>") +
      unknownEventsHtml + collection(true) + QStringLiteral("</div>");
    QStringList controlRows;
    for (const auto& event : buffEvents) {
      const auto id = event.buffId;
      if (id.isEmpty() || !event.metric.isEmpty()) continue;
      if (event.restoresDisplayLayers) controlRows.append(QStringLiteral("<a href='%1'>恢复显示 · %2</a>")
        .arg(buffEventLink(event), compactBuffName(buffCatalog, id).toHtmlEscaped()));
      if (presentationIds.contains(id)) continue;
      const auto notes = replayBuffPublicPresentation(buffCatalog[id].toObject());
      if (notes.labels.isEmpty()) continue;
      presentationIds.insert(id);
      presentationRows.append(QStringLiteral("<a href='%1' style='color:#687b88;text-decoration:none'>%2：%3</a>")
        .arg(buffEventLink(event), compactBuffName(buffCatalog, id).toHtmlEscaped(), notes.labels.join(QStringLiteral(" · ")).toHtmlEscaped()));
    }
    const bool hasPlaybackDetails = !presentationRows.isEmpty() || !controlRows.isEmpty();
    const auto playbackHtml = QStringLiteral("<a name='playback-details'></a><div><b>播放详情</b><br>") +
      controlRows.join(QStringLiteral("<br>")) + (controlRows.isEmpty() ? QString() : QStringLiteral("<br>")) +
      presentationRows.join(QStringLiteral("<br>")) + QStringLiteral("</div>");
    buffSummary->setText(QStringLiteral("已说明 %1项%2").arg(primaryGroupCount).arg(f.buffsKnown ? QString() : QStringLiteral(" · ?")));
    const auto countsText = QStringLiteral("当前已记录 %1 条、%2 组（相同效果按已记录来源分组，不表示层数）\n已说明 %3 组 · 未解释 %4 组\n我方施加 %5 条 · 敌方施加 %6 条 · 来源未记 %7 条\n隐藏动画 %8 条 · 未收录 %9 条\n来源未记的条目可能来自不同施加者；全部记录可分项展开。")
      .arg(memberCount).arg(groups.size()).arg(primaryGroupCount).arg(unknownGroupCount)
      .arg(sourceCounts[0]).arg(sourceCounts[1]).arg(sourceCounts[2]).arg(hiddenCount).arg(unlistedCount);
    buffSummary->setToolTip(countsText); buffSummary->setAccessibleName(countsText);
    buffs->setProperty("memberCount", memberCount); buffs->setProperty("groupCount", groups.size());
    buffs->setProperty("primaryGroupCount", primaryGroupCount); buffs->setProperty("unknownGroupCount", unknownGroupCount);
    buffs->setProperty("primaryEventCount", primaryEventCount); buffs->setProperty("unknownEventCount", unknownEventCount);
    buffs->setProperty("unknownMemberCount", unknownMemberCount);
    buffs->setProperty("playbackGroupCount", 0); buffs->setProperty("playbackEventCount", controlRows.size());
    buffs->setProperty("sourceOwnCount", sourceCounts[0]); buffs->setProperty("sourceEnemyCount", sourceCounts[1]); buffs->setProperty("sourceUnknownCount", sourceCounts[2]);
    buffs->setProperty("hiddenMemberCount", hiddenCount); buffs->setProperty("unlistedMemberCount", unlistedCount);
    if (!f.buffsKnown) buffText.prepend(QStringLiteral("集合待核实"));
    if (!lineage.isEmpty()) { buffText.prepend(lineage); plain.append(lineage); }
    if (groups.isEmpty()) buffText.append(f.buffsKnown ? QStringLiteral("无") : QStringLiteral("暂无已知记录"));
    buffs->setMinimumHeight(14); buffs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    buffs->setAccessibleName(QStringLiteral("当前 Buff 记录：") + buffText.join(QStringLiteral("；")));
    buffs->setToolTip(countsText);
    buffs->setProperty("rawEvidence", QVariantMap{{"members", memberEvidence}, {"events", recordEvidence}, {"phaseDetail", change.detail}});
    buffs->setProperty("publicTipTexts", buffs->tips.values());
    auto* unknownToggle = new QToolButton(buffPanel);
    unknownToggle->setObjectName(QStringLiteral("unitBuffUnknownToggle"));
    unknownToggle->setProperty("fighterKey", f.key); unknownToggle->setCheckable(true);
    unknownToggle->setChecked(hasUnknownDetails && expandedBuffUnknownKeys_.contains(f.key));
    unknownToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    unknownToggle->setCursor(Qt::PointingHandCursor);
    unknownToggle->setStyleSheet(QStringLiteral("QToolButton{border:0;border-radius:5px;padding:0 3px;color:#61748b;background:#edf1f7;font-size:10px} QToolButton:hover{color:#315e9e;background:#dfe9f6} QToolButton:checked{background:#e1e9f4;color:#36577c}"));
    unknownToggle->setFixedHeight(16);
    unknownToggle->setVisible(hasUnknownDetails);
    unknownToggle->setToolTip(QStringLiteral("当前未解释登记 %1 条；本回合未解释变动 %2 条。\n每条记录均可点击查看详情；记录条数不表示效果层数。")
      .arg(unknownMemberCount).arg(unknownEventCount));
    auto* playbackToggle = new QToolButton(buffPanel);
    playbackToggle->setObjectName(QStringLiteral("unitBuffPlaybackToggle"));
    playbackToggle->setProperty("fighterKey", f.key); playbackToggle->setCheckable(true);
    playbackToggle->setChecked(hasPlaybackDetails && expandedBuffPlaybackKeys_.contains(f.key));
    playbackToggle->setCursor(Qt::PointingHandCursor); playbackToggle->setFixedHeight(16);
    playbackToggle->setStyleSheet(unknownToggle->styleSheet());
    playbackToggle->setVisible(hasPlaybackDetails);
    playbackToggle->setToolTip(QStringLiteral("查看已记录的显示指令和官方播放设置；不把整个 Buff 归类为纯动画。"));
    // Keep a single compact footer, so three meters and the state viewport fit
    // in small cards. Each drawer retains its own full-fighter identity.
    auto* detailFooter = new QWidget(buffPanel);
    auto* detailLayout = new QHBoxLayout(detailFooter);
    detailLayout->setContentsMargins(0, 0, 0, 0); detailLayout->setSpacing(2);
    detailLayout->addWidget(unknownToggle); detailLayout->addStretch(1); detailLayout->addWidget(playbackToggle);
    detailFooter->setVisible(hasUnknownDetails || hasPlaybackDetails);
    const auto refreshBuffDocument = [this, key = f.key, buffs, buffToggle, unknownToggle, playbackToggle,
        compactHtml, fullHtml, unknownHtml, playbackHtml, primaryGroupCount, unknownGroupCount, unknownMemberCount, unknownEventCount]
        (int requested) {
      const bool expanded = buffToggle->isChecked(), unknown = unknownToggle->isChecked(), playback = playbackToggle->isChecked();
      if (expanded) expandedBuffKeys_.insert(key); else expandedBuffKeys_.remove(key);
      if (unknown) expandedBuffUnknownKeys_.insert(key); else expandedBuffUnknownKeys_.remove(key);
      if (playback) expandedBuffPlaybackKeys_.insert(key); else expandedBuffPlaybackKeys_.remove(key);
      buffs->setProperty("expanded", expanded); buffs->setProperty("shownGroupCount", expanded ? primaryGroupCount : 0);
      buffs->setProperty("unknownExpanded", unknown); buffs->setProperty("shownUnknownGroupCount", unknown ? unknownGroupCount : 0);
      buffs->setProperty("playbackExpanded", playback);
      buffToggle->setText(expanded ? QStringLiteral("收起") : QStringLiteral("展开"));
      buffToggle->setAccessibleName(expanded ? QStringLiteral("收起本格 Buff 集合") : QStringLiteral("在本格展开 %1 项有官方说明的登记").arg(primaryGroupCount));
      buffToggle->setToolTip(expanded ? QStringLiteral("收起当前集合，仅保留本回合收到的状态与效果记录。") :
        QStringLiteral("查看有官方说明或明确效果标注的登记；不把说明当成未记录的数值。\n未解释记录与播放设置各自折叠。"));
      unknownToggle->setText(unknown ? QStringLiteral("收起未解释") :
        QStringLiteral("未解释记录 %1条").arg(unknownMemberCount ? unknownMemberCount : unknownEventCount));
      unknownToggle->setAccessibleName(unknownToggle->text());
      playbackToggle->setText(playback ? QStringLiteral("收起播放") : QStringLiteral("播放详情"));
      buffs->setHtml((expanded ? fullHtml : compactHtml) + (unknown ? unknownHtml : QString()) + (playback ? playbackHtml : QString()));
      if (requested == 3 && playback) buffs->scrollToAnchor(QStringLiteral("playback-details"));
      else if (requested == 2 && unknown) buffs->scrollToAnchor(QStringLiteral("unknown-details"));
      else if (requested == 1 && expanded) buffs->scrollToAnchor(QStringLiteral("current-buffs"));
      else buffs->verticalScrollBar()->setValue(0);
    };
    refreshBuffDocument(0);
    connect(buffToggle, &QToolButton::toggled, buffs, [refreshBuffDocument](bool) { refreshBuffDocument(1); });
    connect(unknownToggle, &QToolButton::toggled, buffs, [refreshBuffDocument](bool) { refreshBuffDocument(2); });
    connect(playbackToggle, &QToolButton::toggled, buffs, [refreshBuffDocument](bool) { refreshBuffDocument(3); });
    connect(buffToggle, &QToolButton::clicked, this, [this](bool expanded) { emit buffExpansionChanged(expanded); });
    connect(unknownToggle, &QToolButton::clicked, this, [this](bool expanded) { emit buffExpansionChanged(expanded); });
    connect(playbackToggle, &QToolButton::clicked, this, [this](bool expanded) { emit buffExpansionChanged(expanded); });
    for (const auto& event : buffEvents) if (event.selected && !inBar(event)) {
      if (unknownEvent(event)) unknownToggle->setChecked(true);
      buffs->scrollToAnchor(buffEventAnchor(event));
    }
    connect(buffs, &QTextBrowser::anchorClicked, buffs, [this, buffs, buffToggle, eventMenus, buffCatalog, key = f.key](const QUrl& url) {
      if (url.scheme() == QStringLiteral("toggle")) { if (!buffToggle->isChecked()) buffToggle->click(); return; }
      if (url.scheme() == QStringLiteral("events")) {
        auto* menu = new QMenu(buffs); menu->setObjectName(QStringLiteral("buffEventMenu")); menu->setAttribute(Qt::WA_DeleteOnClose); menu->setToolTipsVisible(true);
        for (const auto& event : eventMenus.value(url.toString())) {
          auto* item = menu->addAction(publicBuffEventLabel(event, buffCatalog));
          item->setData(event.index); item->setToolTip(QStringLiteral("点击回看这次变化与原始详情。")); item->setCheckable(true); item->setChecked(event.selected);
          item->setProperty("rawEvidence", event.detail); item->setProperty("buffId", event.buffId);
          item->setProperty("recordStep", event.recordStep);
          connect(item, &QAction::triggered, this, [this, index = event.index, step = event.recordStep] {
            if (step >= 0) emit recordedEffectActivated(step, index); else emit effectActivated(index);
          });
        }
        menu->popup(buffs->mapToGlobal(QPoint(0, 0))); return;
      }
      if (url.scheme() == QStringLiteral("round-effect")) {
        const auto parts = url.path().split(QLatin1Char(':')); bool stepOk = false, indexOk = false;
        const int step = parts.value(0).toInt(&stepOk), index = parts.value(1).toInt(&indexOk);
        if (parts.size() == 2 && stepOk && indexOk) emit recordedEffectActivated(step, index);
        return;
      }
      if (url.scheme() == QStringLiteral("effect")) { bool ok = false; const auto index = url.toString().section(QLatin1Char(':'), 1).toInt(&ok); if (ok) emit effectActivated(index); return; }
      const auto id = buffId(url); if (!id.isEmpty()) emit buffActivated(key, id);
    });
    panelLayout->addWidget(buffs, 1); panelLayout->addWidget(detailFooter); box->addWidget(buffPanel, 1);
    auto phaseBuffCues = phaseBuffVisuals(change.events, buffCatalog);
    if (f.maxPower && *f.maxPower > 0) {
      // Reaching the official threshold is a display cue, never a predicted
      // skill cast. A fighter that was already full gets no fresh cue.
      std::optional<qint64> before, after;
      int eventIndex = -1;
      QString evidence;
      const bool stableThreshold = !change.maxPowerBefore || *change.maxPowerBefore == *f.maxPower;
      if (stableThreshold) for (const auto& event : change.events) {
        if (event.metric == QStringLiteral("气势") && event.observedBefore && event.observedAfter &&
            *event.observedBefore < *f.maxPower && *event.observedAfter >= *f.maxPower) {
          before = event.observedBefore; after = event.observedAfter; eventIndex = event.index; evidence = event.detail;
        }
      }
      if (!after && stableThreshold && fullPower && change.powerBefore && *change.powerBefore < *f.maxPower) {
        before = change.powerBefore; after = f.power;
      }
      if (before && after) {
        OutcomeVisual cue{QStringLiteral("达到满气势"), QStringLiteral("power-full"), outcomeColor(QStringLiteral("power-full")), true,
          QStringLiteral("official-threshold"), QStringLiteral("本阶段已记录气势 %1 → %2，达到官方满气势值 %3。数值不封顶；是否出手仍以战斗记录为准。\n%4")
            .arg(*before).arg(*after).arg(*f.maxPower).arg(evidence)};
        cue.eventIndex = eventIndex; phaseBuffCues.append(cue);
      }
    }
    if (!change.visualBadge.value_or(change.badge).isEmpty() || !change.clientBadge.isEmpty() || !phaseBuffCues.isEmpty() || !change.visuals.isEmpty()) {
      auto* outcomes = new OutcomeOverlay(buffs->viewport(), buffHeading, buffSummary, f.key, change, phaseBuffCues, ownTeam, f.hp, playbackDriven_, playbackProgress_,
        [this, key = f.key] { emit unitSelectionChanged(key); });
      outcomes->setExpanded(buffToggle->isChecked() || unknownToggle->isChecked() || playbackToggle->isChecked());
      if (fighters.size() > 1 && outcomes->property("effectCount").toInt() > 0)
        outcomes->attachSummary(static_cast<UnitMiniRow*>(unitAnchors_.value(f.key)));
      const auto compactOutcomes = [outcomes, buffToggle, unknownToggle, playbackToggle](bool) {
        outcomes->setExpanded(buffToggle->isChecked() || unknownToggle->isChecked() || playbackToggle->isChecked());
      };
      connect(buffToggle, &QToolButton::toggled, outcomes, compactOutcomes);
      connect(playbackToggle, &QToolButton::toggled, outcomes, compactOutcomes);
      connect(unknownToggle, &QToolButton::toggled, outcomes, compactOutcomes);
    }
    plain.append(QStringLiteral("Buff：") + buffText.join(QLatin1Char('\n')));
    // The summary contains observed meter changes and operation counts, not
    // provenance. Keep these useful facts when the technical detail is hidden.
    if (!change.summary.isEmpty()) plain.append(change.summary);
    if (transformed) plain.append(QStringLiteral("变身后：") + currentName + originText);
    if (skillsReplaced) plain.append(QStringLiteral("技能已换\n") + currentSkills);
    stack_->addWidget(card); unitCards_.insert(f.key, card);
  }
  plainText_ = plain.join(QLatin1Char('\n'));
  setAccessibleName(plainText_);
  fitContent();
  update();
  if (!displayed.isEmpty()) focusUnit(priority(displayed.first()) > 1 || !unitCards_.contains(selectedKey_) ? displayed.first().key : selectedKey_);
}

QString ReplayCell::toPlainText() const { return plainText_; }

QWidget* ReplayCell::relationAnchor(const QString& key) const { return unitAnchors_.value(key); }

QList<ReplayMeterChange> ReplayCell::floatingMeterChanges() const { return meterChanges_; }

void ReplayCell::resetBuffExpansion() {
  for (auto* toggle : findChildren<QToolButton*>(QStringLiteral("unitBuffToggle"))) toggle->setChecked(false);
  for (auto* toggle : findChildren<QToolButton*>(QStringLiteral("unitBuffPlaybackToggle"))) toggle->setChecked(false);
  for (auto* toggle : findChildren<QToolButton*>(QStringLiteral("unitBuffUnknownToggle"))) toggle->setChecked(false);
  expandedBuffKeys_.clear(); expandedBuffUnknownKeys_.clear(); expandedBuffPlaybackKeys_.clear();
}

void ReplayCell::fitContent() {
  if (!stack_) return;
  const bool multiple = unitCards_.size() > 1;
  // Mini rows replace the single-unit name, but also have to share this fixed
  // height with three two-line meters. Multi-unit cards keep the compact
  // density even at 204 px; extra room belongs to their Buff document.
  const bool compact = height() <= 184 || multiple;
  const bool crowded = compact && unitCards_.size() == 4;
  setProperty("compactCell", compact);
  layout()->setSpacing(compact ? 0 : 2);
  layout()->setContentsMargins(compact ? 4 : 7, compact ? 2 : 4, compact ? 4 : 7, compact ? 2 : 5);
  heading_->setFixedHeight(compact ? 12 : 16);
  occupants_->setFixedHeight(compact ? 12 : 14);
  unitSwitcher_->setFixedSize(48, compact ? 12 : 16);
  if (multiple) {
    const int count = unitCards_.size(), columns = count > 4 ? 2 : 1;
    const int rows = (count + columns - 1) / columns;
    const int rowHeight = compact ? (rows <= 2 ? 24 : rows == 3 ? 16 : 14) : rows <= 2 ? 28 : rows == 3 ? 22 : 18;
    unitRows_->setFixedHeight(rows * rowHeight);
    for (auto* anchor : unitAnchors_) anchor->setFixedHeight(rowHeight);
  }
  for (auto* card : unitCards_) {
    card->layout()->setSpacing(compact ? 0 : 1);
    card->layout()->setContentsMargins(2, 0, 2, compact ? 0 : 1);
    for (const auto& name : {QStringLiteral("unitStatusLine"), QStringLiteral("unitAttributes"), QStringLiteral("unitRoles")})
      if (auto* child = card->findChild<QWidget*>(name)) child->setFixedHeight(compact ? 12 : 14);
    if (auto* badges = card->findChild<QLabel*>(QStringLiteral("unitResultBadges"))) badges->setFixedHeight(compact ? 14 : 16);
    if (auto* hints = card->findChild<QLabel*>(QStringLiteral("unitClientHints"))) hints->setFixedHeight(compact ? 14 : 16);
    if (auto* line = card->findChild<QWidget*>(QStringLiteral("unitJudgmentLine"))) line->setFixedHeight(compact ? 14 : 16);
    if (auto* panel = card->findChild<QWidget*>(QStringLiteral("unitBuffPanel"))) panel->setMinimumHeight(compact ? 26 : 28);
    for (const auto& name : {QStringLiteral("unitBuffHeading"), QStringLiteral("unitBuffSummary"), QStringLiteral("unitBuffToggle")})
      if (auto* child = card->findChild<QWidget*>(name)) child->setFixedHeight(compact ? 12 : 14);
    for (auto* bar : card->findChildren<QProgressBar*>()) static_cast<ValueBar*>(bar)->setCompact(compact, crowded);
  }
}

void ReplayCell::resizeEvent(QResizeEvent* event) {
  QFrame::resizeEvent(event);
  fitContent();
}

void ReplayCell::setGroupedNumbers(bool enabled) {
  groupedNumbers_ = enabled;
  for (auto* bar : findChildren<QProgressBar*>())
    if (auto* meter = dynamic_cast<ValueBar*>(bar)) meter->setGroupedNumbers(enabled);
}

void ReplayCell::setPlaybackDriven(bool enabled) {
  if (enabled && playbackDriven_) return;
  playbackDriven_ = enabled; playbackProgress_ = 1;
  setProperty("playbackDriven", enabled); setProperty("playbackProgress", 1.0);
  for (auto* bar : findChildren<QProgressBar*>()) static_cast<ValueBar*>(bar)->setPlaybackProgress(1);
  for (auto* overlay : findChildren<QWidget*>(QStringLiteral("unitOutcomeOverlay"))) static_cast<OutcomeOverlay*>(overlay)->setPlaybackDriven(enabled);
  static_cast<CellEmphasis*>(graphicsEffect())->setPulse(0);
  for (auto* card : unitCards_) if (card->graphicsEffect()) static_cast<CellEmphasis*>(card->graphicsEffect())->setPulse(0);
  if (unitCards_.size() > 1) for (auto* anchor : unitAnchors_) { auto* mini = static_cast<UnitMiniRow*>(anchor); mini->pulse = 0; mini->update(); }
}

void ReplayCell::setPlaybackProgress(qreal progress) {
  if (!playbackDriven_) return;
  playbackProgress_ = std::isfinite(progress) ? std::clamp(progress, qreal(0), qreal(1)) : 1;
  setProperty("playbackProgress", playbackProgress_);
  for (auto* bar : findChildren<QProgressBar*>()) static_cast<ValueBar*>(bar)->setPlaybackProgress(playbackProgress_);
  for (auto* overlay : findChildren<QWidget*>(QStringLiteral("unitOutcomeOverlay"))) static_cast<OutcomeOverlay*>(overlay)->setPlaybackProgress(playbackProgress_);
  const auto pulse = std::sin(playbackProgress_ * 3.14159265358979323846);
  static_cast<CellEmphasis*>(graphicsEffect())->setPulse(playbackProgress_ >= 1 ? 0 : pulse);
  for (auto* card : unitCards_) if (card->graphicsEffect())
    static_cast<CellEmphasis*>(card->graphicsEffect())->setPulse(playbackProgress_ >= 1 ? 0 : pulse);
  if (unitCards_.size() > 1) for (auto* anchor : unitAnchors_) {
    auto* mini = static_cast<UnitMiniRow*>(anchor);
    mini->pulse = (mini->property("isImpactTarget").toBool() || mini->property("outcomeAnimated").toBool()) && playbackProgress_ < 1 ? pulse : 0;
    mini->update();
  }
}

void ReplayCell::focusUnit(const QString& key) {
  if (auto* card = unitCards_.value(key)) {
    selectedKey_ = key; stack_->setCurrentWidget(card); setProperty("visibleFighterKey", key);
    occupants_->setText(card->property("otherUnitsSummary").toString());
    occupants_->setAccessibleName(occupants_->text());
    occupants_->setToolTip(card->property("otherUnitsDetail").toString());
    unitSwitcher_->setToolTip(QStringLiteral("切换查看同格单位\n") + card->property("otherUnitsDetail").toString());
    for (auto* action : unitSwitcher_->menu()->actions()) action->setChecked(action->data().toString() == key);
    bool actor = false, dead = true;
    for (auto* occupant : unitCards_) {
      actor = actor || occupant->property("isActor").toBool();
      dead = dead && occupant->property("isDead").toBool();
    }
    // One dead occupant must not grey out another living occupant in this cell.
    // Their cards and mini rows each retain their own actual state.
    for (auto* occupant : unitCards_) if (occupant->graphicsEffect())
      static_cast<CellEmphasis*>(occupant->graphicsEffect())->setState(false,
        occupant->property("isDead").toBool() && !dead, occupant->property("isImpactTarget").toBool());
    const bool target = unitCards_.size() == 1 && card->property("isImpactTarget").toBool();
    if (unitCards_.size() > 1) for (auto it = unitAnchors_.cbegin(); it != unitAnchors_.cend(); ++it) {
      auto* mini = static_cast<UnitMiniRow*>(it.value()); mini->setChecked(it.key() == key); mini->update();
    }
    setProperty("selectedUnitDead", card->property("isDead"));
    setProperty("actorEmphasis", actor); setProperty("deadUnit", dead); setProperty("visualScale", actor ? 1.16 : 1.0);
    setProperty("targetEmphasis", target);
    static_cast<CellEmphasis*>(graphicsEffect())->setState(actor, dead, target);
    if (actor) raise(); update();
  }
}

void ReplayCell::paintEvent(QPaintEvent* event) {
  QFrame::paintEvent(event);
  const bool actor = property("actorEmphasis").toBool();
  if (!hasSummoned_ && !hasEmpowered_ && !actor) return;
  QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing); painter.setBrush(Qt::NoBrush);
  if (actor) {
    painter.setPen(QPen(QColor(QStringLiteral("#1764d1")), 4));
    const int inset = hasSummoned_ || hasEmpowered_ ? 7 : 1;
    painter.drawRoundedRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset), 4, 4);
  }
  if (hasSummoned_) {
    QPen pen(QColor(QStringLiteral("#36a768")), 2); pen.setStyle(summonPending_ ? Qt::DashLine : Qt::SolidLine);
    painter.setPen(pen); painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 5, 5);
  }
  if (hasEmpowered_) {
    const qreal inset = hasSummoned_ ? 4 : 1;
    painter.setPen(QPen(QColor(QStringLiteral("#e57faa")), 2));
    painter.drawRoundedRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset), 4, 4);
  }
}
