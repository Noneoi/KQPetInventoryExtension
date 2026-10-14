#pragma once
#include "battle_replay_life_timeline_data.h"
#include <QSlider>

class QHBoxLayout;
class QScrollArea;
class QScrollBar;
class QToolButton;

// A round overview and a scrollable unit view share the same playback value.
// Dense groups retain every event; clicks request the beginning of an action.
class ReplayLifeTimeline final : public QSlider {
  Q_OBJECT
public:
  using Marker = ReplayTimeline::LifeMarker;
  explicit ReplayLifeTimeline(QWidget* parent = nullptr);
  void setReplay(const BattleReplay::Replay& replay, const QJsonObject& catalog);
  void setPerspective(int ownTeam);
  void setUnitView(bool enabled);
  bool unitView() const { return unitView_; }
  void refreshPosition();
  int turnAt(int step) const { return model_.globalTurns.value(step, 0); }
  int roundTurnAt(int step) const { return model_.localTurns.value(step, 0); }
  int roundNumberAt(int step) const;
  int activeRoundIndex() const;
  int turnCount() const { return model_.actionSteps.size(); }
  const QList<Marker>& markers() const { return model_.markers; }
  QToolButton* roundButton(int groupIndex) const;
  QStringList unitKeys() const;
  QRect unitRowRect(const QString& key) const;
  QRect markerRect(int marker) const;
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;
signals:
  void actionStartRequested(int step);
  void inspectionStarted();
  void layoutModeChanged();
protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void keyPressEvent(QKeyEvent*) override;
  bool event(QEvent*) override;
private:
  struct Group { QRect rect; QList<int> indices; int lane = 0; };
  QList<Group> groups() const;
  QString description(const Marker& marker) const;
  QString groupCaption(const Group& group) const;
  QString groupDescription(const Group& group) const;
  QString roundCaption(int groupIndex) const;
  QString unitCaption(int unitIndex) const;
  QRectF track() const;
  int headerHeight() const;
  int visibleUnitRows() const;
  int totalHeight() const;
  qreal xForStep(int step) const;
  int stepForX(qreal x) const;
  void activate(const Group& group);
  void rebuildUnitOrder();
  void updateGeometryAndScroll();
  void closeMenus();
  ReplayTimeline::LifeModel model_;
  QList<int> unitOrder_;
  QMap<QString, int> unitPositions_;
  QScrollArea* roundStrip_;
  QWidget* roundContent_;
  QHBoxLayout* roundLayout_;
  QList<QToolButton*> roundButtons_;
  QScrollBar* unitScroll_;
  int ownTeam_ = -1, lastRound_ = -1;
  bool dragging_ = false, unitView_ = false;
};
