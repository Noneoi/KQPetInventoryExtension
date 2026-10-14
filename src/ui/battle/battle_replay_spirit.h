#pragma once

#include <QWidget>
#include <QList>
#include <QStringList>
#include <optional>

class QVariantAnimation;
struct ReplaySpiritTransition {
  std::optional<qint64> before, after, delta;
  QString kind, status, evidence;
};

class ReplaySpiritBar final : public QWidget {
  Q_OBJECT
public:
  explicit ReplaySpiritBar(QWidget* parent = nullptr);
  void setProgress(std::optional<qint64> current, std::optional<qint64> maximum,
                   const QString& caption, const QString& tooltip = {});
  // Supply recorded values after setProgress. A missing delta stays missing;
  // neither a zero balance nor its difference is treated as a skill cast.
  void setTransition(std::optional<qint64> before, std::optional<qint64> delta,
                     const QString& status = {}, bool animate = false);
  // Presentation branch supplied only after checking the official progress
  // record. A negative delta alone does not identify a drain or a skill cast.
  void setTransitionKind(const QString& kind, const QString& evidence = {});
  void setRecordedTransitions(const QList<ReplaySpiritTransition>& records);
  void setPlaybackDriven(bool enabled);
  void setPlaybackProgress(qreal progress);
  QString toPlainText() const;
  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  QString progressText() const;
  QString transitionText() const;
  void refreshDescription();
  void applyProgress(qreal progress);
  qreal paintedValue() const;
  QStringList visibleDeltas() const;
  QStringList visibleKinds() const;
  QString activeTransitionKind() const;
  std::optional<qint64> current_, maximum_;
  std::optional<qint64> before_, delta_;
  QString caption_, status_, explanation_, transitionKind_, transitionEvidence_;
  bool playbackDriven_ = false, animate_ = false, animateCue_ = false;
  qreal playbackProgress_ = 1;
  QVariantAnimation* animation_ = nullptr;
  QList<ReplaySpiritTransition> records_;
};
