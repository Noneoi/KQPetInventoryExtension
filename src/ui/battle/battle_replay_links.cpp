#include "ui/battle/battle_replay_links.h"
#include <QEvent>
#include <QFontMetricsF>
#include <QLineF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QVariantAnimation>
#include <QVariantMap>
#include <algorithm>
#include <cmath>

namespace {
QString relationColorName(int sourceTeam, int targetTeam) {
  if (sourceTeam < 0 || targetTeam < 0) return QStringLiteral("gray");
  return sourceTeam == targetTeam ? QStringLiteral("green") : QStringLiteral("red");
}
QColor relationColor(int sourceTeam, int targetTeam) {
  if (sourceTeam < 0 || targetTeam < 0) return QColor(QStringLiteral("#78838f"));
  return QColor(sourceTeam == targetTeam ? QStringLiteral("#278957") : QStringLiteral("#d44848"));
}

QRectF visualRect(QWidget* overlay, QWidget* cell) {
  QRectF rect(overlay->mapFromGlobal(cell->mapToGlobal(QPoint(0, 0))), cell->size());
  // A miniature occupant anchor is inside the cell's graphics effect. Include
  // that ancestor transform so a scaled actor still has an exact endpoint.
  for (auto* ancestor = cell; ancestor && ancestor != overlay->parentWidget(); ancestor = ancestor->parentWidget()) {
    const auto scale = ancestor->property("visualScale");
    if (scale.isValid() && scale.toReal() > 1) {
      const QRectF bounds(overlay->mapFromGlobal(ancestor->mapToGlobal(QPoint(0, 0))), ancestor->size());
      rect = QRectF(bounds.center() + (rect.topLeft() - bounds.center()) * scale.toReal(), rect.size() * scale.toReal());
    }
  }
  return rect;
}
QPointF edgeTowards(const QRectF& rect, const QPointF& destination) {
  const QPointF direction = destination - rect.center();
  const qreal x = std::abs(direction.x()), y = std::abs(direction.y());
  if (x < 0.01 && y < 0.01) return rect.center();
  const qreal horizontal = x > 0.01 ? rect.width() / (2 * x) : 1e9;
  const qreal vertical = y > 0.01 ? rect.height() / (2 * y) : 1e9;
  return rect.center() + direction * std::min(horizontal, vertical);
}

QPainterPath directRelation(const QRectF& from, const QRectF& to, bool self, bool sharedCell) {
  QPainterPath path;
  if (self) {
    // A self-effect gets one small loop around the corner of its own card.
    // Both endpoints remain on that card, so it cannot resemble a neighbour's
    // incoming effect.
    const qreal corner = std::min(qreal(25), from.height() - 3);
    const QPointF start(from.right() - corner, from.top() + 1);
    const QPointF end(from.right() - 1, from.top() + corner);
    path.moveTo(start);
    path.cubicTo(start + QPointF(35, -19), end + QPointF(19, -35), end);
    return path;
  }

  if (sharedCell) {
    // Distinct occupants in one formation cell are never a self-effect. Their
    // short straight connection uses the reserved gutter of their own rows.
    path.moveTo(QPointF(from.left() + 5, from.center().y()));
    path.lineTo(QPointF(to.left() + 5, to.center().y()));
    return path;
  }

  const QPointF difference = to.center() - from.center();
  const qreal distance = std::hypot(difference.x(), difference.y());
  if (distance < 0.01) return path;
  const QPointF direction = difference / distance;
  const QPointF sourceEdge = edgeTowards(from, to.center());
  const QPointF targetEdge = edgeTowards(to, from.center());
  QPointF start = sourceEdge + direction * 1.5;
  QPointF end = targetEdge - direction * 1.5;
  // Adjacent cards have very little gutter. Use a few pixels of the border to
  // keep the tail and arrowhead distinct, never either card's centre.
  if (QPointF::dotProduct(end - start, direction) < 18) {
    start = sourceEdge - direction * 5;
    end = targetEdge + direction * 2;
    // Actor enlargement can make neighbouring borders touch. Keep even that
    // tiny arrow pointing toward the target, never back toward the actor.
    if (QPointF::dotProduct(end - start, direction) < 9) start = end - direction * 9;
  }
  const qreal gap = QLineF(start, end).length();
  if (gap >= 46) {
    // A single shallow arc gives the ribbon its shape without routing it
    // around other cards. Aim both card-edge anchors at the control point so
    // the wide tail and tip leave/enter the correct card cleanly.
    QPointF normal(-direction.y(), direction.x());
    if (normal.y() > 0 || (std::abs(normal.y()) < .01 && normal.x() > 0)) normal = -normal;
    const QPointF control = (from.center() + to.center()) / 2 + normal * std::clamp(gap * .30, qreal(14), qreal(70));
    start = edgeTowards(from, control);
    end = edgeTowards(to, control);
    const QLineF outgoing(start, control), incoming(end, control);
    if (outgoing.length() > .01) start += (control - start) / outgoing.length() * 1.5;
    if (incoming.length() > .01) end += (control - end) / incoming.length() * 1.5;
    path.moveTo(start);
    path.quadTo(control, end);
    return path;
  }
  path.moveTo(start);
  path.lineTo(end);
  return path;
}

QPainterPath ribbonArrow(const QPainterPath& center, bool sharedCell, bool self, bool captioned = false) {
  const qreal length = center.length();
  if (length < .5) return {};
  // Width follows the available gap. Occupants sharing one card have only a
  // narrow gutter; their arrows must not spill into the neighbouring name.
  const qreal shaftHalf = sharedCell ? 1.8 : self ? 3.2 : std::clamp(length * .065, qreal(2.3), qreal(7));
  const qreal headLength = std::max(captioned ? (sharedCell ? qreal(9) : qreal(15)) : qreal(0),
    std::min(length * .46, sharedCell ? qreal(9) : self ? qreal(17) : qreal(29)));
  const qreal headHalf = sharedCell ? std::min(qreal(4), length * .23) :
    self ? qreal(8.5) : std::clamp(headLength * .66, qreal(4), qreal(18));
  const qreal shaftEnd = center.percentAtLength(std::max(qreal(0), length - headLength));
  const int samples = std::clamp(int(std::ceil(length / 5)), 8, 70);
  QPolygonF left, right;
  for (int i = 0; i <= samples; ++i) {
    const qreal t = shaftEnd * i / samples;
    const QPointF point = center.pointAtPercent(t);
    const QPointF tangent = center.pointAtPercent(std::min(qreal(1), t + .005)) -
      center.pointAtPercent(std::max(qreal(0), t - .005));
    const qreal tangentLength = std::hypot(tangent.x(), tangent.y());
    if (tangentLength < .001) continue;
    const QPointF normal(-tangent.y() / tangentLength, tangent.x() / tangentLength);
    // A gently widening tail makes the direction readable even when the
    // moving highlight is paused or the effect has already completed.
    const qreal halfWidth = shaftHalf * (.78 + .22 * i / samples);
    left.append(point + normal * halfWidth);
    right.append(point - normal * halfWidth);
  }
  if (left.isEmpty()) return {};
  const QPointF tip = center.pointAtPercent(1);
  QPointF shoulder = center.pointAtPercent(shaftEnd);
  if (headLength > length && QLineF(shoulder, tip).length() > .001)
    shoulder = tip - (tip - shoulder) / QLineF(shoulder, tip).length() * headLength;
  const QPointF headDirection = tip - shoulder;
  const qreal headDistance = std::hypot(headDirection.x(), headDirection.y());
  if (headDistance < .001) return {};
  const QPointF headNormal(-headDirection.y() / headDistance, headDirection.x() / headDistance);
  QPainterPath ribbon;
  ribbon.moveTo(left.first());
  for (int i = 1; i < left.size(); ++i) ribbon.lineTo(left[i]);
  ribbon.lineTo(shoulder + headNormal * headHalf);
  ribbon.lineTo(tip);
  ribbon.lineTo(shoulder - headNormal * headHalf);
  for (int i = right.size() - 1; i >= 0; --i) ribbon.lineTo(right[i]);
  ribbon.closeSubpath();
  return ribbon;
}

struct LabelObstacle {
  QRectF rect;
  qreal weight = 0;
};
struct RelationCaption {
  QString text, displayText;
  QColor color;
  QRectF rect;
  QPainterPath center, ribbon;
  QPointF tip;
  QRectF headBounds;
  QString sourceKey, targetKey;
};
QList<QPainterPath> captionPaths(const QPainterPath& original, const QRectF& from, const QRectF& to,
                                bool compact, qreal captionWidth) {
  QList<QPainterPath> result{original};
  if (compact || original.length() >= captionWidth + 65) return result;
  // A narrow gutter cannot contain both readable text and an arrowhead. Keep
  // the arrow on the two facing card borders, but consider the unused parts of
  // those borders before extending its labelled tail into the source card.
  const auto between = [&](QPointF source, const QPointF& target, const QPointF& direction) {
    // Enlarging the acting card can make its border pass the neighbour's
    // border by a few pixels. The target still decides the arrow direction.
    if (QPointF::dotProduct(target - source, direction) < 9) source = target - direction * 9;
    QPainterPath candidate; candidate.moveTo(source); candidate.lineTo(target); result.append(candidate);
  };
  if (from.right() <= to.left() + 8 || to.right() <= from.left() + 8) {
    const qreal top = std::max(from.top(), to.top()) + 16;
    const qreal bottom = std::min(from.bottom(), to.bottom()) - 16;
    if (bottom > top) for (const qreal fraction : {.85, .15, .65, .35}) {
      const qreal y = top + (bottom - top) * fraction;
      const bool forward = to.center().x() > from.center().x();
      between(QPointF(forward ? from.right() : from.left(), y), QPointF(forward ? to.left() : to.right(), y), QPointF(forward ? 1 : -1, 0));
    }
  }
  if (from.bottom() <= to.top() + 8 || to.bottom() <= from.top() + 8) {
    const qreal left = std::max(from.left(), to.left()) + 16;
    const qreal right = std::min(from.right(), to.right()) - 16;
    if (right > left) for (const qreal fraction : {.85, .15, .65, .35}) {
      const qreal x = left + (right - left) * fraction;
      const bool forward = to.center().y() > from.center().y();
      between(QPointF(x, forward ? from.bottom() : from.top()), QPointF(x, forward ? to.top() : to.bottom()), QPointF(0, forward ? 1 : -1));
    }
  }
  return result;
}
RelationCaption relationCaption(const QStringList& labels, const QColor& color, const QRectF& from,
                               const QRectF& target, const QPainterPath& arrow, bool sharedCell, bool self,
                               const QRectF& bounds,
                               const QFont& font, const QList<LabelObstacle>& obstacles,
                               const QList<RelationCaption>& placed) {
  RelationCaption result;
  QStringList visibleLabels;
  for (const auto& label : labels) if (!label.trimmed().isEmpty()) visibleLabels.append(label.trimmed());
  result.text = visibleLabels.join(QStringLiteral(" · ")); result.color = color;
  result.displayText = visibleLabels.join(QLatin1Char('\n'));
  if (result.text.isEmpty() || arrow.isEmpty() || arrow.length() < .5 || bounds.width() < 40 || bounds.height() < 24) return result;
  const QFontMetricsF metrics(font);
  const qreal maxWidth = std::min({qreal(196), bounds.width() - 10, std::max(qreal(88), arrow.length() * .58)});
  qreal preferredWidth = 0;
  for (const auto& label : visibleLabels) preferredWidth = std::max(preferredWidth, metrics.horizontalAdvance(label) + 20);
  const qreal width = std::min(maxWidth, std::max(qreal(48), preferredWidth));
  const qreal height = metrics.boundingRect(QRectF(0, 0, width - 20, 1000), Qt::TextWordWrap, result.displayText).height() + 10;
  struct Candidate { QRectF rect; QPainterPath center; QPointF tip; QRectF headBounds; qreal order; bool extendTail; };
  QList<Candidate> candidates;
  int pathIndex = 0;
  for (const auto& path : captionPaths(arrow, from, target, sharedCell || self, width)) {
    const qreal length = path.length();
    const qreal headLength = std::max(sharedCell ? qreal(9) : qreal(15),
      std::min(length * .46, sharedCell ? qreal(9) : self ? qreal(17) : qreal(29)));
    const qreal shaftEnd = path.percentAtLength(std::max(qreal(0), length - headLength));
    const QPointF tip = path.pointAtPercent(1);
    QPointF shoulder = path.pointAtPercent(shaftEnd);
    if (headLength > length && QLineF(shoulder, tip).length() > .001)
      shoulder = tip - (tip - shoulder) / QLineF(shoulder, tip).length() * headLength;
    const QLineF head(shoulder, tip);
    if (head.length() < .5) continue;
    const QPointF direction = (tip - shoulder) / head.length();
    const qreal headHalf = sharedCell ? std::min(qreal(4), length * .23) :
      self ? qreal(8.5) : std::clamp(headLength * .66, qreal(4), qreal(18));
    const QPointF normal(-direction.y(), direction.x());
    QPainterPath protectedHead;
    protectedHead.addPolygon(QPolygonF{tip, shoulder + normal * headHalf, shoulder - normal * headHalf});
    protectedHead.closeSubpath();
    QRectF previousBox;
    const int positions = std::clamp(int(std::ceil(length / 3)), 28, 240);
    for (int position = 0; position <= positions; ++position) {
      const qreal fraction = .08 + .84 * position / positions;
      QPointF center = path.pointAtPercent(path.percentAtLength(length * fraction));
      QRectF box(center - QPointF(width / 2, height / 2), QSizeF(width, height));
      const bool extendTail = length < width + 55;
      if (extendTail) {
        // Expand only toward the source. The triangle on the recipient's
        // border stays uncovered, so even adjacent cells retain a clear end.
        const qreal radius = std::abs(direction.x()) * width / 2 + std::abs(direction.y()) * height / 2;
        const qreal shortfall = radius + head.length() + 7 - QPointF::dotProduct(tip - center, direction);
        if (shortfall > 0) { center -= direction * shortfall; box.moveCenter(center); }
      }
      if (!bounds.contains(box) || protectedHead.intersects(box.adjusted(-4, -4, 4, 4))) continue;
      if (!previousBox.isEmpty() && QLineF(previousBox.center(), box.center()).length() < .5) continue;
      previousBox = box;
      candidates.append({box, path, tip, protectedHead.boundingRect(), pathIndex * 4 + std::abs(fraction - .58) * 10, extendTail});
    }
    ++pathIndex;
  }
  qreal best = 1e30;
  bool extendTail = false;
  for (const auto& candidate : candidates) {
    qreal score = candidate.order;
    const qreal area = std::max(qreal(1), candidate.rect.width() * candidate.rect.height());
    for (const auto& obstacle : obstacles) {
      const QRectF overlap = candidate.rect.adjusted(-3, -3, 3, 3).intersected(obstacle.rect);
      if (!overlap.isEmpty()) score += obstacle.weight * (1 + overlap.width() * overlap.height() / area);
    }
    for (const auto& other : placed) {
      const QRectF overlap = candidate.rect.adjusted(-4, -4, 4, 4).intersected(other.rect);
      if (!overlap.isEmpty()) score += 100000 * (1 + overlap.width() * overlap.height() / area);
    }
    if (score < best) {
      best = score; result.rect = candidate.rect; result.center = candidate.center; extendTail = candidate.extendTail;
      result.tip = candidate.tip; result.headBounds = candidate.headBounds;
    }
  }
  if (!result.rect.isEmpty()) {
    result.ribbon = ribbonArrow(result.center, sharedCell, self, true);
    QPainterPath body; body.addRoundedRect(result.rect, 5, 5);
    if (extendTail) {
      // This is part of the filled arrow body, not a caption leader line.
      // It also handles a label whose width exceeds the original gutter.
      QPainterPath bridge; bridge.moveTo(result.rect.center()); bridge.lineTo(result.center.pointAtPercent(0));
      QPainterPathStroker stroke; stroke.setWidth(sharedCell ? 3.6 : 8); stroke.setCapStyle(Qt::RoundCap);
      result.ribbon = result.ribbon.united(stroke.createStroke(bridge));
    }
    result.ribbon = result.ribbon.united(body);
  }
  return result;
}
}

ReplayLinks::ReplayLinks(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("replayRelationLinks"));
  setProperty("relationShape", QStringLiteral("filled-ribbon"));
  setProperty("captionPlacement", QStringLiteral("inside-arrow"));
  setProperty("playbackDriven", false); setProperty("playbackProgress", 1.0);
  setAttribute(Qt::WA_TransparentForMouseEvents);
  setAttribute(Qt::WA_NoSystemBackground);
  parent->installEventFilter(this); setGeometry(parent->rect());
  animation_ = new QVariantAnimation(this); animation_->setObjectName(QStringLiteral("replayFlowAnimation"));
  animation_->setStartValue(0.0); animation_->setEndValue(1.0); animation_->setDuration(1000);
  connect(animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
    phase_ = value.toReal(); setProperty("playbackProgress", phase_); update();
  });
}
void ReplayLinks::setRelation(QWidget* source, QWidget* target) {
  setRelations({{source, target}}, {}, false);
}
void ReplayLinks::setMotion(const QString& motion) {
  motion_ = motion == QStringLiteral("close") || motion == QStringLiteral("remote") || motion == QStringLiteral("summon") ? motion : QString();
  setProperty("relationMotion", motion_);
  setAccessibleName(motion_ == QStringLiteral("close") ? QStringLiteral("近程作用方向提示") :
    motion_ == QStringLiteral("remote") ? QStringLiteral("远程作用方向提示") :
    motion_ == QStringLiteral("summon") ? QStringLiteral("召唤关系提示") : QStringLiteral("已记录作用关系"));
  update();
}
void ReplayLinks::setRelations(const QList<ReplayRelation>& relations, const QString& cueKey, bool animate) {
  animate_ = animate;
  relations_.clear(); bool hasSource = false, hasTarget = false; int connections = 0;
  QVariantList presentations; QStringList accessible;
  for (const auto& relation : relations) {
    if (!relation.target) continue;
    relations_.append({relation.source, relation.target, relation.sourceTeam, relation.targetTeam,
      relation.sourceKey, relation.targetKey, relation.labels, relation.detail});
    hasSource = hasSource || relation.source; hasTarget = true;
    if (relation.source) ++connections;
    presentations.append(QVariantMap{
      {QStringLiteral("sourceKey"), relation.sourceKey}, {QStringLiteral("targetKey"), relation.targetKey},
      {QStringLiteral("sourceObjectName"), relation.source ? relation.source->objectName() : QString()},
      {QStringLiteral("targetObjectName"), relation.target->objectName()},
      {QStringLiteral("sourceTeam"), relation.sourceTeam}, {QStringLiteral("targetTeam"), relation.targetTeam},
      {QStringLiteral("color"), relationColorName(relation.sourceTeam, relation.targetTeam)},
      {QStringLiteral("colorHex"), relationColor(relation.sourceTeam, relation.targetTeam).name()},
      {QStringLiteral("labels"), relation.labels}, {QStringLiteral("detail"), relation.detail}
    });
    // The relation property retains the original evidence. Screen readers,
    // like the painted arrow, receive only the human-facing effect labels.
    if (!relation.labels.isEmpty()) accessible.append(relation.labels.join(QStringLiteral(" · ")));
  }
  setProperty("hasKnownSource", hasSource); setProperty("hasVisibleTarget", hasTarget);
  setProperty("connectionCount", connections);
  setProperty("relationPresentations", presentations);
  setAccessibleDescription(accessible.join(QLatin1Char('\n')));
  if (playbackDriven_) {
    animation_->stop();
    if (!animate_) phase_ = 1;
    cueKey_ = cueKey;
  } else if (cueKey_ != cueKey || !animate) {
    animation_->stop(); phase_ = 1;
    if (animate && !relations_.isEmpty()) animation_->start();
    cueKey_ = cueKey;
  }
  setProperty("playbackProgress", phase_);
  setGeometry(parentWidget()->rect()); raise(); update();
}
void ReplayLinks::setPlaybackDriven(bool enabled) {
  if (enabled && playbackDriven_) return;
  playbackDriven_ = enabled; animation_->stop(); phase_ = 1;
  if (!enabled) cueKey_.clear();
  setProperty("playbackDriven", enabled); setProperty("playbackProgress", 1.0); update();
}
void ReplayLinks::setPlaybackProgress(qreal progress) {
  if (!playbackDriven_) return;
  animation_->stop();
  phase_ = animate_ && std::isfinite(progress) ? std::clamp(progress, qreal(0), qreal(1)) : 1;
  setProperty("playbackProgress", phase_); update();
}
bool ReplayLinks::eventFilter(QObject* object, QEvent* event) {
  if (object == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest)) {
    setGeometry(parentWidget()->rect()); raise(); update();
  }
  return QWidget::eventFilter(object, event);
}
void ReplayLinks::paintEvent(QPaintEvent*) {
  QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
  QFont captionFont = font(); captionFont.setPixelSize(14); captionFont.setBold(true);
  QList<RelationCaption> captions;
  QVariantList captionLayouts;
  QList<LabelObstacle> obstacles;
  for (auto* widget : parentWidget()->findChildren<QWidget*>()) {
    if (!widget->isVisible() || widget == this) continue;
    const QString name = widget->objectName();
    qreal weight = 0;
    if (name == QStringLiteral("hpBar") || name == QStringLiteral("shieldValue") || name == QStringLiteral("powerBar")) weight = 60000;
    else if (name == QStringLiteral("unitName") || name == QStringLiteral("unitMiniRow") || name == QStringLiteral("unitOutcomeOverlay")) weight = 35000;
    else if (name == QStringLiteral("replayCastingPanel")) weight = 70000;
    else if (name.startsWith(QStringLiteral("replayTeamSource")) || name.startsWith(QStringLiteral("replaySpirit"))) weight = 35000;
    else if (name.startsWith(QStringLiteral("replayCell"))) weight = 300;
    if (weight > 0) obstacles.append({visualRect(this, widget), weight});
  }
  const qreal pulse = playbackDriven_ ? (phase_ >= 1 ? 0 : std::sin(phase_ * 3.14159265358979323846)) : 1 - phase_;
  auto drawingOrder = relations_;
  const auto captionPriority = [&](const Relation& relation) {
    if (relation.labels.isEmpty() || !relation.source || !relation.target) return QPair<int, int>{4, 0};
    if (relation.source == relation.target) return QPair<int, int>{0, 0};
    const auto from = visualRect(this, relation.source), to = visualRect(this, relation.target);
    const auto gap = directRelation(from, to, false, false).length();
    return QPair<int, int>{gap < 120 ? 1 : 2, -relation.labels.join(QStringLiteral(" · ")).size()};
  };
  // A self-loop or adjacent-card arrow has fewer legal text positions. Give
  // it room first; flexible long ribbons can carry their text farther along.
  std::stable_sort(drawingOrder.begin(), drawingOrder.end(), [&](const Relation& a, const Relation& b) {
    return captionPriority(a) < captionPriority(b);
  });
  for (const auto& relation : drawingOrder) {
    const auto source = relation.source, target = relation.target;
    if (!target || !target->isVisible()) continue;
    const QRectF to = visualRect(this, target);
    const QColor color = relationColor(relation.sourceTeam, relation.targetTeam);
    QColor halo = color; halo.setAlpha(int(145 + 100 * pulse));
    const bool summon = motion_ == QStringLiteral("summon");
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(halo, 2 + pulse * 2));
    painter.drawRoundedRect(to.adjusted(2, 2, -2, -2), 5, 5);
    if (summon && phase_ < 1) {
      const qreal growth = 3 * pulse;
      halo.setAlpha(int(120 * pulse)); painter.setPen(QPen(halo, 1.2));
      painter.drawRoundedRect(to.adjusted(-growth, -growth, growth, growth), 5, 5);
    }
    bool sharedCell = false, self = false;
    QPainterPath path;
    QRectF from;
    if (source && source->isVisible()) {
      from = visualRect(this, source);
      sharedCell = source != target && source->property("relationMiniAnchor").toBool() &&
        target->property("relationMiniAnchor").toBool() && source->parentWidget() == target->parentWidget();
      self = source == target;
      path = directRelation(from, to, self, sharedCell);
    }
    QPainterPath ribbon;
    if (!relation.labels.isEmpty() && !path.isEmpty()) {
      auto caption = relationCaption(relation.labels, color, from, to, path, sharedCell, self,
        QRectF(rect()).adjusted(5, 5, -5, -5), captionFont, obstacles, captions);
      if (!caption.rect.isEmpty()) {
        caption.sourceKey = relation.sourceKey; caption.targetKey = relation.targetKey;
        ribbon = caption.ribbon; path = caption.center;
        captions.append(caption);
        const QRectF textBounds = caption.rect.adjusted(8, 4, -8, -4);
        captionLayouts.append(QVariantMap{{QStringLiteral("sourceKey"), relation.sourceKey},
          {QStringLiteral("targetKey"), relation.targetKey}, {QStringLiteral("text"), caption.text},
          {QStringLiteral("placement"), QStringLiteral("inside-arrow")},
          {QStringLiteral("color"), relationColorName(relation.sourceTeam, relation.targetTeam)},
          {QStringLiteral("captionRect"), caption.rect}, {QStringLiteral("textRect"), textBounds},
          {QStringLiteral("arrowBounds"), ribbon.boundingRect()}, {QStringLiteral("targetTip"), caption.tip},
          {QStringLiteral("headBounds"), caption.headBounds},
          {QStringLiteral("textInsideArrow"), ribbon.contains(textBounds)}});
      }
    }
    if (path.isEmpty()) continue;
    if (ribbon.isEmpty()) ribbon = ribbonArrow(path, sharedCell, self);
    if (ribbon.isEmpty()) continue;
    // The shaft and large triangular tip are one filled silhouette, matching
    // the curved-arrow reference while retaining an unmistakable endpoint.
    QLinearGradient fill(path.pointAtPercent(0), path.pointAtPercent(1));
    QColor tailColor = color.darker(118), tipColor = color.lighter(122);
    tailColor.setAlpha(240); tipColor.setAlpha(250);
    fill.setColorAt(0, tailColor); fill.setColorAt(1, tipColor);
    painter.setPen(QPen(QColor(255, 255, 255, 250), sharedCell ? 1.2 : 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(fill);
    painter.drawPath(ribbon);
    if (phase_ < 1) {
      // All decoration is driven by the page's shared progress. The trail is
      // a reading cue, not a claim about original animation frames or hits.
      const auto position = path.pointAtPercent(phase_);
      if (motion_ == QStringLiteral("remote")) {
        QColor trail = color.lighter(165); trail.setAlpha(160);
        painter.setPen(QPen(trail, 4, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(path.pointAtPercent(std::max(qreal(0), phase_ - .08)), position);
      }
      painter.setBrush(color.lighter(180)); painter.setPen(QPen(Qt::white, 1.5));
      if (motion_ == QStringLiteral("close")) {
        const qreal size = 3.5;
        painter.drawPolygon(QPolygonF{position + QPointF(0, -size), position + QPointF(size, 0),
          position + QPointF(0, size), position + QPointF(-size, 0)});
      } else painter.drawEllipse(position, 3.5, 3.5);
    }
  }
  // Text is painted inside the widened, continuous arrow body. There is no
  // detached badge or leader to mistake for a second source/target relation.
  // Restore its fill after other ribbons so crossing arrows cannot erase it.
  painter.setFont(captionFont);
  for (const auto& caption : captions) {
    QLinearGradient fill(caption.center.pointAtPercent(0), caption.center.pointAtPercent(1));
    QColor tailColor = caption.color.darker(118), tipColor = caption.color.lighter(122);
    tailColor.setAlpha(240); tipColor.setAlpha(250); fill.setColorAt(0, tailColor); fill.setColorAt(1, tipColor);
    painter.setBrush(fill); painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(caption.rect.adjusted(.8, .8, -.8, -.8), 4.2, 4.2);
    painter.setPen(Qt::white);
    painter.drawText(caption.rect.adjusted(8, 4, -8, -4), Qt::AlignCenter | Qt::TextWordWrap, caption.displayText);
  }
  setProperty("relationCaptionLayouts", captionLayouts);
}
