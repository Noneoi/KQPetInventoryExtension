#pragma once
#include "ui/battle/battle_replay_cell.h"
#include <QColor>

// Board-level decoration: numbers may extend beyond their card without being
// clipped by the card's layout/scroll area or capturing its mouse clicks.
class ReplayMeterOverlay final : public QWidget {
public:
  explicit ReplayMeterOverlay(QWidget* board);
  void setChanges(const QList<ReplayMeterChange>& changes);
  void setGroupedNumbers(bool enabled);
protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void paintEvent(QPaintEvent*) override;
private:
  struct Token { QString text; bool critical = false; int count = 1; std::optional<qint64> value; };
  struct Line { QPointer<QWidget> anchor; QString key, metric; QColor color; QList<Token> tokens; };
  QList<Line> lines_;
  bool groupedNumbers_ = false;
};
