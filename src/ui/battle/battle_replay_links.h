#pragma once
#include <QPointer>
#include <QStringList>
#include <QWidget>
class QVariantAnimation;

struct ReplayRelation {
  QWidget* source = nullptr;
  QWidget* target = nullptr;
  int sourceTeam = -1, targetTeam = -1;
  QString sourceKey, targetKey;
  QStringList labels;
  QString detail;
};

// Evidenced action targets or a selected effect. Animation decorates the
// relation only; it never interpolates battle state or invents a Buff source.
class ReplayLinks final : public QWidget {
public:
  explicit ReplayLinks(QWidget* parent);
  void setRelation(QWidget* source, QWidget* target);
  void setRelations(const QList<ReplayRelation>& relations, const QString& cueKey, bool animate);
  // An evidenced client branch decorates the same relation, without adding
  // movements or damage records to the timeline. Unknown modes stay generic.
  void setMotion(const QString& motion);
  void setPlaybackDriven(bool enabled);
  void setPlaybackProgress(qreal progress);
protected:
  bool eventFilter(QObject* object, QEvent* event) override;
  void paintEvent(QPaintEvent*) override;
private:
  struct Relation {
    QPointer<QWidget> source, target;
    int sourceTeam = -1, targetTeam = -1;
    QString sourceKey, targetKey;
    QStringList labels;
    QString detail;
  };
  QList<Relation> relations_;
  QVariantAnimation* animation_;
  QString cueKey_;
  QString motion_;
  qreal phase_ = 1;
  bool playbackDriven_ = false, animate_ = false;
};
