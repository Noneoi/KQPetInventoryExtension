#include "ui/battle/battle_replay_meter_overlay.h"
#include "ui/battle/battle_replay_numbers.h"
#include <QEvent>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>
#include <QVariant>
#include <algorithm>
#include <limits>

namespace { constexpr qreal textAngle = -15; }

ReplayMeterOverlay::ReplayMeterOverlay(QWidget* board) : QWidget(board) {
  setObjectName(QStringLiteral("replayMeterOverlay"));
  setAttribute(Qt::WA_TransparentForMouseEvents); setAttribute(Qt::WA_TranslucentBackground);
  setFocusPolicy(Qt::NoFocus); setGeometry(board->rect()); board->installEventFilter(this);
  if (auto* arrows = board->findChild<QWidget*>(QStringLiteral("replayRelationLinks"), Qt::FindDirectChildrenOnly))
    arrows->installEventFilter(this);
  setProperty("textAngle", textAngle); setProperty("lifetime", QStringLiteral("meter-segment"));
}

void ReplayMeterOverlay::setGroupedNumbers(bool enabled) {
  groupedNumbers_ = enabled;
  update();
}

void ReplayMeterOverlay::setChanges(const QList<ReplayMeterChange>& changes) {
  lines_.clear(); QVariantList records; QStringList accessible;
  for (const auto& change : changes) {
    if (!change.anchor || change.events.isEmpty()) continue;
    Line line; line.anchor = change.anchor; line.key = change.fighterKey; line.metric = change.metric;
    line.color = change.metric == QStringLiteral("生命") ? QColor("#c7465b") :
      change.metric == QStringLiteral("护盾") ? QColor("#246fc2") : QColor("#ba751d");
    for (const auto& event : change.events) {
      if (event.valueText.isEmpty()) continue;
      records.append(QVariantMap{{QStringLiteral("fighterKey"), change.fighterKey}, {QStringLiteral("metric"), change.metric},
        {QStringLiteral("text"), event.valueText}, {QStringLiteral("critical"), event.critical}, {QStringLiteral("effectIndex"), event.index}});
      accessible.append(change.anchor->property("displayName").toString() + QLatin1Char(' ') + event.label + (event.critical ? QStringLiteral(" 暴击") : QString()));
      auto amountText = event.valueText;
      for (const auto& prefix : {QStringLiteral("校准 "), QStringLiteral("上限 ")}) amountText.remove(prefix);
      bool amountKnown = false; const auto amount = amountText.toLongLong(&amountKnown);
      // Zero confirmations remain inspectable on their bar. They are not an
      // increase/decrease and must not fill the entire board with floating 0s.
      if (amountKnown && amount == 0) continue;
      // A later hit must replace, rather than sum with, the previous hit.
      // If the official boundary cannot be refined, keep its entries separate.
      line.tokens.append({event.valueText, event.critical, 1, std::nullopt});
    }
    if (!line.tokens.isEmpty()) lines_.append(line);
  }
  setProperty("recordedChanges", records); setProperty("recordCount", records.size());
  setAccessibleName(QStringLiteral("当前数值变化；每条保留到本指标下一次变化前，完整过程可点击对应条子查看。\n") + accessible.join(QLatin1Char('\n')));
  setVisible(!lines_.isEmpty()); setGeometry(parentWidget()->rect()); raise(); update();
}

bool ReplayMeterOverlay::eventFilter(QObject* watched, QEvent* event) {
  // The arrows raise themselves after a board layout change. Keep numeric
  // callouts readable above them while leaving the skill banner above both.
  if (watched != parentWidget() && event->type() == QEvent::ZOrderChange && isVisible()) {
    raise();
    if (auto* banner = parentWidget()->findChild<QWidget*>(QStringLiteral("replayCastingPanel"), Qt::FindDirectChildrenOnly))
      if (banner->isVisible()) banner->raise();
  }
  if (watched == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest)) {
    setGeometry(parentWidget()->rect()); update();
  }
  return QWidget::eventFilter(watched, event);
}

void ReplayMeterOverlay::paintEvent(QPaintEvent*) {
  QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing); painter.setRenderHint(QPainter::TextAntialiasing);
  QVariantList painted; QList<QRectF> placed;
  const QTransform rotation = QTransform().rotate(textAngle);
  for (const auto& line : lines_) {
    if (!line.anchor || !line.anchor->isVisibleTo(parentWidget())) continue;
    auto tokens = line.tokens;
    for (auto& token : tokens) token.text = ReplayNumbers::display(token.text, groupedNumbers_);
    const QPoint barPoint = line.anchor->mapTo(parentWidget(), QPoint());
    const QRectF bar(barPoint, line.anchor->size());
    QFont font(QStringLiteral("Segoe UI")); font.setPixelSize(25); font.setWeight(QFont::Bold);
    QFont badgeFont(QStringLiteral("Microsoft YaHei UI")); badgeFont.setPixelSize(10); badgeFont.setWeight(QFont::Bold);
    const auto textWidth = [&](const QFont& current) {
      qreal width = 0;
      for (const auto& token : tokens)
        width += QFontMetricsF(current).horizontalAdvance(token.text) + (token.critical ? 33 : 0) + (token.count > 1 ? 27 : 0) + 10;
      return std::max(qreal(0), width - 10);
    };
    while (font.pixelSize() > 21 && textWidth(font) > 250) font.setPixelSize(font.pixelSize() - 1);
    const QFontMetricsF fm(font); const qreal textWidthValue = textWidth(font);
    const QRectF local(-3, -fm.ascent() - 3, textWidthValue + 6, fm.height() + 6);
    const auto rotated = rotation.mapRect(local);
    const int row = line.metric == QStringLiteral("生命") ? 0 : line.metric == QStringLiteral("护盾") ? 1 : 2;
    QPointF origin(bar.left() + bar.width() * .54 + row * 14, bar.center().y() + 6);
    QRectF bounds = rotated.translated(origin);
    if (bounds.right() > width() - 7) origin.rx() -= bounds.right() - width() + 7;
    bounds = rotated.translated(origin);
    if (bounds.top() < 4) origin.ry() += 4 - bounds.top();
    bounds = rotated.translated(origin);
    // Avoid another unit's numbers where possible. The small color-matched
    // stem always stays attached to this exact bar after shifting.
    for (int attempt = 0; attempt < 3; ++attempt) {
      bool intersects = false;
      for (const auto& other : placed) if (other.intersects(bounds.adjusted(8, 10, -8, -10))) { intersects = true; break; }
      if (!intersects) break;
      const QPointF shift(0, 8); origin += shift; bounds.translate(shift);
    }
    if (bounds.left() < 4) origin.rx() += 4 - bounds.left();
    if (bounds.bottom() > height() - 4) origin.ry() -= bounds.bottom() - height() + 4;
    bounds = rotated.translated(origin); placed.append(bounds);
    const QPointF anchor(bar.right() - 5, bar.center().y());
    QColor stem = line.color; stem.setAlpha(130); painter.setPen(QPen(stem, 1.5));
    painter.drawLine(anchor, origin + QPointF(-3, -5));
    painter.save(); painter.translate(origin); painter.rotate(textAngle); painter.setFont(font);
    qreal x = 0; QVariantList paintedTokens;
    for (const auto& token : tokens) {
      paintedTokens.append(QVariantMap{{QStringLiteral("text"), token.text}, {QStringLiteral("critical"), token.critical}, {QStringLiteral("count"), token.count}});
      QPainterPath glyphs; glyphs.addText(QPointF(x, 0), font, token.text);
      painter.setPen(QPen(QColor(255, 255, 255, 245), 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      painter.setBrush(line.color); painter.drawPath(glyphs);
      painter.setPen(Qt::NoPen); painter.drawPath(glyphs);
      x += fm.horizontalAdvance(token.text) + 3;
      if (token.critical) {
        const QRectF badge(x, -14, 30, 16);
        painter.setPen(QPen(QColor("#b94b34"), 1)); painter.setBrush(QColor("#fff0d5")); painter.drawRoundedRect(badge, 3, 3);
        painter.setFont(badgeFont); painter.setPen(QColor("#b63e29")); painter.drawText(badge, Qt::AlignCenter, QStringLiteral("暴击")); x += 30;
      }
      if (token.count > 1) {
        painter.setFont(badgeFont); painter.setPen(line.color);
        painter.drawText(QRectF(x, -14, 27, 15), Qt::AlignCenter, QStringLiteral("合计")); x += 27;
      }
      x += 7;
    }
    painter.restore();
    painted.append(QVariantMap{{QStringLiteral("fighterKey"), line.key}, {QStringLiteral("metric"), line.metric},
      {QStringLiteral("bounds"), bounds}, {QStringLiteral("anchor"), bar}, {QStringLiteral("fontPixels"), font.pixelSize()},
      {QStringLiteral("color"), line.color.name()}, {QStringLiteral("angle"), textAngle}, {QStringLiteral("tokens"), paintedTokens}});
  }
  setProperty("paintedLines", painted);
}
