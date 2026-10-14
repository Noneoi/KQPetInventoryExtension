#include "ui/battle/battle_replay_spirit.h"

#include <QEasingCurve>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <QVariantAnimation>
#include <algorithm>
#include <cmath>

namespace {
QString digits(std::optional<qint64> value) {
  return value ? QString::number(*value) : QStringLiteral("?");
}
}

ReplaySpiritBar::ReplaySpiritBar(QWidget* parent) : QWidget(parent) {
  setFixedHeight(46);
  setMinimumWidth(120);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setProperty("playbackDriven", false);
  setProperty("playbackProgress", 1.0);
  animation_ = new QVariantAnimation(this);
  animation_->setObjectName(QStringLiteral("replaySpiritAnimation"));
  animation_->setDuration(350);
  animation_->setStartValue(0.0); animation_->setEndValue(1.0);
  connect(animation_, &QVariantAnimation::valueChanged, this,
    [this](const QVariant& value) { applyProgress(value.toReal()); });
  setProgress({}, {}, {});
}

void ReplaySpiritBar::setProgress(std::optional<qint64> current,
                                  std::optional<qint64> maximum,
                                  const QString& caption, const QString& tooltip) {
  animation_->stop();
  current_ = current;
  maximum_ = maximum;
  caption_ = caption;
  explanation_ = tooltip;
  before_.reset(); delta_.reset(); status_.clear(); animate_ = false; animateCue_ = false;
  transitionKind_.clear(); transitionEvidence_.clear(); records_.clear();
  applyProgress(1);
  refreshDescription();
}

void ReplaySpiritBar::setTransition(std::optional<qint64> before,
                                    std::optional<qint64> delta,
                                    const QString& status, bool animate) {
  animation_->stop();
  before_ = before; delta_ = delta; status_ = status;
  records_.clear();
  animateCue_ = animate;
  animate_ = animate && before_ && current_ && maximum_ && *maximum_ > 0 && *before_ != *current_;
  applyProgress(animate_ ? 0 : 1);
  refreshDescription();
  if (animate_ && !playbackDriven_) animation_->start();
}

void ReplaySpiritBar::setTransitionKind(const QString& kind, const QString& evidence) {
  transitionKind_ = QStringList{QStringLiteral("charge"), QStringLiteral("drain"), QStringLiteral("release"), QStringLiteral("neutral")}.contains(kind) ? kind : QString();
  transitionEvidence_ = evidence;
  refreshDescription();
  if (animateCue_ && !transitionKind_.isEmpty() && !playbackDriven_ && animation_->state() != QAbstractAnimation::Running)
    animation_->start();
}

void ReplaySpiritBar::setRecordedTransitions(const QList<ReplaySpiritTransition>& records) {
  records_ = records;
  if (records_.size() > 1) {
    before_ = records_.first().before; delta_.reset();
    QStringList statuses;
    for (const auto& record : records_) {
      const auto label = record.kind == QStringLiteral("charge") ? QStringLiteral("充能") : record.kind == QStringLiteral("drain") ? QStringLiteral("扣除") :
        record.kind == QStringLiteral("release") ? QStringLiteral("技能生效") : record.status;
      if (!label.isEmpty() && !statuses.contains(label)) statuses.append(label);
    }
    if (transitionKind_ == QStringLiteral("release") && !statuses.contains(QStringLiteral("技能生效"))) statuses.append(QStringLiteral("技能施放"));
    status_ = statuses.join(QStringLiteral(" · "));
    animate_ = animateCue_ && before_ && current_ && maximum_ && *maximum_ > 0 && *before_ != *current_;
    transitionEvidence_ += QStringLiteral("\n条内各差值是逐条原始记录，没有合并成净变化。条宽从阶段起点过渡至最终余额仅为阅读动画，不表示官方动画帧序。");
  }
  applyProgress(playbackProgress_); refreshDescription();
}

QStringList ReplaySpiritBar::visibleDeltas() const {
  QStringList values;
  for (const auto& record : records_) {
    if (record.delta) values.append((*record.delta >= 0 ? QStringLiteral("+") : QString()) + QString::number(*record.delta));
    else values.append(QStringLiteral("%1→%2").arg(digits(record.before), digits(record.after)));
  }
  if (records_.isEmpty() && delta_) values.append((*delta_ >= 0 ? QStringLiteral("+") : QString()) + QString::number(*delta_));
  return values;
}
QStringList ReplaySpiritBar::visibleKinds() const {
  QStringList kinds;
  for (const auto& record : records_) if (!record.kind.isEmpty() && !kinds.contains(record.kind)) kinds.append(record.kind);
  if (!transitionKind_.isEmpty() && !kinds.contains(transitionKind_)) kinds.append(transitionKind_);
  return kinds;
}
QString ReplaySpiritBar::activeTransitionKind() const {
  if (records_.size() < 2 || !animateCue_) return transitionKind_;
  const int index = std::min(static_cast<int>(records_.size()) - 1, static_cast<int>(playbackProgress_ * records_.size()));
  return records_[index].kind;
}

void ReplaySpiritBar::setPlaybackDriven(bool enabled) {
  if (enabled && playbackDriven_) return;
  playbackDriven_ = enabled; animation_->stop();
  setProperty("playbackDriven", enabled);
  applyProgress(1);
}

void ReplaySpiritBar::setPlaybackProgress(qreal progress) {
  if (!playbackDriven_) return;
  animation_->stop(); applyProgress(progress);
}

void ReplaySpiritBar::applyProgress(qreal progress) {
  playbackProgress_ = std::isfinite(progress) ? std::clamp(progress, qreal(0), qreal(1)) : 1;
  setProperty("playbackProgress", playbackProgress_);
  setProperty("paintedValue", paintedValue());
  setProperty("activeTransitionKind", activeTransitionKind());
  update();
}

qreal ReplaySpiritBar::paintedValue() const {
  if (!current_ || !maximum_ || *maximum_ <= 0) return 0;
  const qreal end = std::clamp(qreal(*current_), qreal(0), qreal(*maximum_));
  if (!animate_ || !before_) return end;
  const qreal start = std::clamp(qreal(*before_), qreal(0), qreal(*maximum_));
  const auto progress = QEasingCurve(QEasingCurve::OutCubic).valueForProgress(playbackProgress_);
  return start + (end - start) * progress;
}

void ReplaySpiritBar::refreshDescription() {
  const auto text = toPlainText();
  setAccessibleName(text);
  const auto evidence = explanation_ + (transitionEvidence_.isEmpty() ? QString() : QLatin1Char('\n') + transitionEvidence_);
  // Provenance belongs to the explicitly opened raw-data view. Keep the
  // battlefield tooltip and screen-reader description about the actual play.
  setProperty("progressEvidence", explanation_);
  setProperty("rawEvidence", evidence);
  QStringList description;
  if (!current_) description.append(QStringLiteral("当前通灵进度尚未记录。"));
  if (!maximum_) description.append(QStringLiteral("本场通灵上限尚未记录。"));
  if (records_.size() > 1) {
    description.append(QStringLiteral("本次变化按发生顺序列出："));
    for (const auto& record : records_) {
      QString change = QStringLiteral("%1 → %2").arg(digits(record.before), digits(record.after));
      if (record.delta) change += QStringLiteral("（%1%2）")
        .arg(*record.delta > 0 ? QStringLiteral("+") : QString()).arg(*record.delta);
      description.append(change);
    }
  }
  setAccessibleDescription(description.join(QLatin1Char('\n')));
  setToolTip(description.isEmpty() ? text : text + QLatin1Char('\n') + description.join(QLatin1Char('\n')));
  setProperty("transitionText", transitionText());
  setProperty("transitionStatus", status_);
  setProperty("transitionKind", transitionKind_);
  setProperty("transitionEvidence", transitionEvidence_);
  setProperty("visibleTransitionKinds", visibleKinds());
  setProperty("visibleTransitionDeltas", visibleDeltas());
  setProperty("cueAnimated", animateCue_ && !transitionKind_.isEmpty());
  setProperty("full", current_ && maximum_ && *maximum_ > 0 && *current_ >= *maximum_);
  update();
}

QString ReplaySpiritBar::progressText() const {
  if (!current_ && !maximum_) return QStringLiteral("通灵之魂 未记录");
  return QStringLiteral("通灵之魂 %1/%2").arg(digits(current_), digits(maximum_)) +
    (current_ && maximum_ && *maximum_ > 0 && *current_ >= *maximum_ ? QStringLiteral(" · 满槽") : QString());
}

QString ReplaySpiritBar::transitionText() const {
  if (records_.size() > 1) return visibleDeltas().join(QStringLiteral(" / "));
  QStringList parts;
  if (before_) parts.append(QStringLiteral("%1 → %2").arg(digits(before_), digits(current_)));
  if (delta_) parts.append((*delta_ >= 0 ? QStringLiteral("+") : QString()) + QString::number(*delta_));
  return parts.join(QStringLiteral("  "));
}

QString ReplaySpiritBar::toPlainText() const {
  QStringList text{progressText()};
  if (!transitionText().isEmpty()) text.append(transitionText());
  if (!status_.isEmpty()) text.append(status_);
  if (!caption_.isEmpty()) text.append(caption_);
  return text.join(QLatin1Char('\n'));
}

QSize ReplaySpiritBar::sizeHint() const { return {320, 46}; }

void ReplaySpiritBar::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::TextAntialiasing);

  auto titleFont = font();
  titleFont.setPixelSize(12);
  titleFont.setWeight(QFont::DemiBold);
  painter.setFont(titleFont);
  const QFontMetrics titleMetrics(titleFont);
  const QRect heading(1, 0, std::max(0, width() - 2), 21);
  const QString progress = records_.size() > 1 ? QStringLiteral("通灵 %1/%2").arg(digits(current_), digits(maximum_)) : progressText();
  const int progressWidth = std::min(heading.width(), titleMetrics.horizontalAdvance(progress) + 2);
  painter.setPen(QColor(QStringLiteral("#51497d")));
  painter.drawText(QRect(heading.x(), heading.y(), progressWidth, heading.height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   titleMetrics.elidedText(progress, Qt::ElideRight, progressWidth));

  const int captionWidth = heading.width() - progressWidth - 12;
  const auto marker = records_.size() > 1 ? QString() : transitionKind_ == QStringLiteral("charge") ? QStringLiteral("↑ ") : transitionKind_ == QStringLiteral("drain") ? QStringLiteral("↓ ") : transitionKind_ == QStringLiteral("release") ? QStringLiteral("✦ ") : QString();
  const auto caption = records_.size() > 1 ? status_ : marker + (status_.isEmpty() ? caption_ : status_ + (caption_.isEmpty() ? QString() : QStringLiteral(" · ") + caption_));
  if (!caption.isEmpty() && captionWidth > 24) {
    auto captionFont = titleFont;
    captionFont.setPixelSize(11);
    if (records_.size() > 1) while (captionFont.pixelSize() > 9 && QFontMetrics(captionFont).horizontalAdvance(caption) > captionWidth)
      captionFont.setPixelSize(captionFont.pixelSize() - 1);
    captionFont.setWeight(transitionKind_.isEmpty() || transitionKind_ == QStringLiteral("neutral") ? QFont::Normal : QFont::Bold);
    painter.setFont(captionFont);
    painter.setPen(QColor(transitionKind_ == QStringLiteral("drain") ? QStringLiteral("#b45463") : transitionKind_ == QStringLiteral("release") ? QStringLiteral("#a77521") : status_.isEmpty() ? QStringLiteral("#748196") : QStringLiteral("#67519e")));
    painter.drawText(QRect(heading.x() + progressWidth + 12, heading.y(), captionWidth, heading.height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(captionFont).elidedText(caption, Qt::ElideRight, captionWidth));
  }

  const QRectF track(1, 24, std::max(0, width() - 2), 16);
  if (track.width() <= 0) return;
  const QColor empty(QStringLiteral("#edeaf6"));
  const QColor border(QStringLiteral("#d8d2e9"));
  const QColor filled(QStringLiteral("#9685d8"));
  const QColor full(QStringLiteral("#7462bb"));
  if (!current_ || !maximum_ || *maximum_ <= 0) {
    painter.setPen(border);
    painter.setBrush(empty);
    painter.drawRoundedRect(track, 3, 3);
    auto noteFont = font();
    noteFont.setPixelSize(10);
    painter.setFont(noteFont);
    painter.setPen(QColor(QStringLiteral("#817994")));
    painter.drawText(track, Qt::AlignCenter, transitionText().isEmpty() ? QStringLiteral("进度未确认") : transitionText());
    return;
  }

  // Keep exact logged numbers in the heading. Saturate only the painted fill.
  const qreal lit = paintedValue();
  const QColor fill = *current_ >= *maximum_ ? full : filled;
  if (*maximum_ <= static_cast<qint64>(track.width() / 2)) {
    const qreal count = static_cast<qreal>(*maximum_);
    const qreal pitch = track.width() / count;
    const qreal gap = std::min(qreal(3), pitch / 4);
    const qreal segmentWidth = (track.width() - gap * (count - 1)) / count;
    for (qint64 i = 0; i < *maximum_; ++i) {
      const QRectF segment(track.x() + i * (segmentWidth + gap), track.y(), segmentWidth, track.height());
      const auto inner = segment.adjusted(0.5, 0.5, -0.5, -0.5);
      painter.setPen(border); painter.setBrush(empty);
      painter.drawRoundedRect(inner, 2, 2);
      const qreal portion = std::clamp(lit - qreal(i), qreal(0), qreal(1));
      if (portion > 0) {
        painter.save(); QPainterPath clip; clip.addRoundedRect(inner, 2, 2); painter.setClipPath(clip);
        painter.fillRect(QRectF(inner.x(), inner.y(), inner.width() * portion, inner.height()), fill);
        painter.restore();
      }
    }
  } else {
    // At less than two pixels per point, individual cells cannot be resolved.
    // Avoid an unbounded paint loop for unexpected maxima; retain their exact
    // value in text and draw the true ratio without inventing a segment count.
    painter.setPen(border);
    painter.setBrush(empty);
    painter.drawRoundedRect(track, 3, 3);
    const qreal ratio = static_cast<qreal>(lit) / static_cast<qreal>(*maximum_);
    if (ratio > 0) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(fill);
      painter.drawRoundedRect(QRectF(track.x(), track.y(), track.width() * ratio, track.height()), 3, 3);
    }
  }
  const auto paintedKind = activeTransitionKind();
  if (!paintedKind.isEmpty() && paintedKind != QStringLiteral("neutral")) {
    const qreal pulse = animateCue_ ? std::sin(playbackProgress_ * 3.141592653589793) : 0;
    const QColor accent(paintedKind == QStringLiteral("drain") ? QStringLiteral("#b45463") : paintedKind == QStringLiteral("release") ? QStringLiteral("#b9822a") : QStringLiteral("#8461c4"));
    painter.save(); painter.setPen(QPen(accent, 1 + pulse)); painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(track.adjusted(0, -1, 0, 1), 3, 3);
    if (animateCue_ && playbackProgress_ > 0 && playbackProgress_ < 1) {
      painter.setPen(Qt::NoPen); painter.setBrush(accent);
      for (int i = 0; i < 5; ++i) {
        const qreal x = track.x() + track.width() * (i + 0.5) / 5;
        const qreal t = std::fmod(playbackProgress_ * 1.6 + i * 0.11, 1.0);
        if (paintedKind == QStringLiteral("charge")) painter.drawEllipse(QPointF(x, 44 - t * 18), 1.7 + pulse, 1.7 + pulse);
        else if (paintedKind == QStringLiteral("drain")) {
          QPolygonF shard; shard << QPointF(x - 2, 27 + t * 16) << QPointF(x + 2, 27 + t * 16) << QPointF(x, 31 + t * 16); painter.drawPolygon(shard);
        } else painter.drawEllipse(QPointF(track.center().x() + (x - track.center().x()) * t, track.center().y()), 2 + pulse, 2 + pulse);
      }
    }
    painter.restore();
  }
  const auto change = transitionText();
  if (!change.isEmpty()) {
    auto changeFont = font(); changeFont.setPixelSize(11); changeFont.setWeight(QFont::DemiBold);
    while (changeFont.pixelSize() > 8 && QFontMetrics(changeFont).horizontalAdvance(change) > track.width() - 8)
      changeFont.setPixelSize(changeFont.pixelSize() - 1);
    painter.setFont(changeFont);
    const int textWidth = std::min(int(track.width()) - 2, QFontMetrics(changeFont).horizontalAdvance(change) + 10);
    const QRectF pill(track.center().x() - textWidth / 2.0, track.y(), textWidth, track.height());
    painter.setPen(Qt::NoPen); painter.setBrush(QColor(255, 255, 255, 224)); painter.drawRoundedRect(pill, 3, 3);
    painter.setPen(QColor(QStringLiteral("#4d3d7c")));
    painter.drawText(pill, Qt::AlignCenter, QFontMetrics(changeFont).elidedText(change, Qt::ElideRight, textWidth - 4));
  }
}
