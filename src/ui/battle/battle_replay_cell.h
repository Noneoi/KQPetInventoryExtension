#pragma once
#include "domain/battle_replay.h"
#include <QFrame>
#include <QPointer>
#include <QSet>

class QLabel;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;

struct ReplayUnitEvent {
  int index = -1;
  QString label, detail, color, buffId;
  bool removing = false, selected = false;
  QString metric, valueText;
  QString buffSource, buffSourceName, buffSourcePath;
  int buffSourceTeam = -1;
  bool buffTriggered = false;
  // Verified client command attached to this event, not a Buff-ID category.
  bool restoresDisplayLayers = false;
  QString harmLabel;
  std::optional<qint64> observedBefore, observedAfter;
  // True only for a damage record carrying an explicit critical judgement.
  bool critical = false;
  // Historical notices retain their action as well as the local effect index.
  int recordStep = -1;
};
struct ReplayVisualCue {
  QString label, kind, detail;
  bool derived = false;
  QString evidenceType = QStringLiteral("recorded");
  QString applier, applierName;
  int applierTeam = -1;
  int eventIndex = -1;
};
struct ReplayUnitOverview {
  bool target = false;
  QString summary, detail, badge;
  QList<ReplayUnitEvent> events;
  // Separate from current-phase events: these must never replay meter deltas,
  // animate a previous target, or be mistaken for current Buff membership.
  std::optional<QList<ReplayUnitEvent>> roundBuffEvents;
  int buffRound = -1;
  bool animate = false;
  int transitionMs = 350;
  std::optional<qint64> hpBefore, maxHpBefore, powerBefore, maxPowerBefore;
  // An action's observed prefix outlives an individual playback phase. These
  // records drive the pending bands and the board-level numeric callouts.
  bool hasActionMeterContext = false, actionMetersPending = false;
  QList<ReplayUnitEvent> actionMeterEvents;
  // History remains inspectable; only the latest observed segment of each
  // meter is allowed to float or paint a change band. Unset means one group
  // supplied by an independent cell preview, not an inferred event order.
  std::optional<QList<ReplayUnitEvent>> activeMeterEvents;
  std::optional<qint64> actionHpBefore, actionMaxHpBefore, actionShieldBefore;
  std::optional<qint64> actionPowerBefore, actionMaxPowerBefore;
  // Derived official-client cues are separate from explicit target flags.
  QString clientBadge, clientDetail;
  // Official display precedence can differ from the set of raw flags.
  // An explicit empty value suppresses visual judgements without hiding them.
  std::optional<QString> visualBadge;
  QList<ReplayVisualCue> visuals;
};

struct ReplayMeterChange {
  QPointer<QWidget> anchor;
  QString fighterKey, metric;
  QList<ReplayUnitEvent> events;
};

class ReplayCell final : public QFrame {
  Q_OBJECT
public:
  explicit ReplayCell(int side, QWidget* parent = nullptr);
  void display(int slot, const QList<BattleReplay::Fighter>& fighters, const QJsonObject& catalog,
               const QString& actor, const QSet<QString>& changed,
               const QString& source = {}, const QString& target = {},
               const QString& targetText = {}, const QSet<QString>& empowered = {},
               const QMap<QString, ReplayUnitOverview>& overview = {}, int ownTeam = 0);
  QString toPlainText() const;
  void focusUnit(const QString& key);
  QWidget* relationAnchor(const QString& key) const;
  QList<ReplayMeterChange> floatingMeterChanges() const;
  void resetBuffExpansion();
  // Continuous playback supplies one shared phase clock. Pausing that clock
  // also pauses these decorations; logged balances are never interpolated.
  void setPlaybackDriven(bool enabled);
  void setPlaybackProgress(qreal progress);
  void setGroupedNumbers(bool enabled);
signals:
  void unitActivated(const QString& key);
  void buffActivated(const QString& key, const QString& id);
  void effectActivated(int index);
  void recordedEffectActivated(int step, int index);
  void buffExpansionChanged(bool expanded);
  void diagnosticActivated(const QString& key);
  void unitSelectionChanged(const QString& key);
protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
private:
  void fitContent();
  QWidget* heading_;
  QLabel* occupants_;
  QToolButton* unitSwitcher_;
  QWidget* unitRows_ = nullptr;
  QStackedWidget* stack_ = nullptr;
  QMap<QString, QWidget*> unitNames_;
  QMap<QString, QWidget*> unitCards_;
  QMap<QString, QWidget*> unitAnchors_;
  QList<ReplayMeterChange> meterChanges_;
  QString selectedKey_;
  QSet<QString> expandedBuffKeys_;
  QSet<QString> expandedBuffUnknownKeys_;
  QSet<QString> expandedBuffPlaybackKeys_;
  QString plainText_;
  bool hasSummoned_ = false, summonPending_ = false, hasEmpowered_ = false;
  bool playbackDriven_ = false;
  bool groupedNumbers_ = false;
  qreal playbackProgress_ = 1;
};
