#pragma once
#include "contracts/battle_replay_catalog.h"
#include "domain/battle_replay.h"
#include "domain/battle_replay_empowerment.h"
#include <QWidget>
#include <QJsonObject>
#include <array>

class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;
class QTableView;
class QTextBrowser;
class QPlainTextEdit;
class QLineEdit;
class QSortFilterProxyModel;
class ReplayStepModel;
class ReplayFieldModel;
class ReplayCell;
class ReplayStepFilter;
class QCheckBox;
class QListWidget;
class QTabWidget;
class ReplayLinks;
class ReplayMeterOverlay;
class ReplaySpiritBar;
class ReplayLifeTimeline;
class QFrame;
class QVariantAnimation;
class QProgressBar;
class QScrollArea;
class QToolButton;

class BattleReplayPage final : public QWidget {
  Q_OBJECT
public:
  explicit BattleReplayPage(QWidget* parent = nullptr, BattleReplayCatalogHandle initialCatalog = {});
  bool loadFile(const QString& path);
  void loadDirectory(const QString& path);
  void enqueueReplay(const QString& sourceLabel, const QByteArray& original, const QString& sha256);
  void setAccountIdentity(const QString& playerName, bool reliable, const QString& evidence);
  void setReplaySourceStatus(const QString& status);
  void setCatalogState(BattleReplayCatalogHandle snapshot, const QString& message, bool busy);
  bool useUpdatedCatalog();
  const BattleReplay::Replay& replay() const { return replay_; }
  int currentStep() const { return current_; }
  int currentMoment() const { return moment_; }
  int currentPlaybackPhase() const { return playbackPhase_; }
  int playbackPhaseCount() const { return static_cast<int>(playbackPhases_.size()); }
  QString playbackPhaseKind() const;
  const QMap<QString, BattleReplay::Fighter>& displayedState() const;
signals:
  void catalogReloadRequested();
public slots:
  void seek(int step);
  void seekMoment(int moment);
  void seekPlaybackPhase(int phase);
  void selectEffect(int index);
  void focusSearch();
  void resetSessionContext();
protected:
  void hideEvent(QHideEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
private:
  void render();
  void showUnit(const QString& key, bool raw = false);
  void showBuff(const QString& key, const QString& id, bool raw = false);
  void showSkill(const QString& id, bool raw = false);
  void showPetMechanics(const QString& raceId);
  void showReferenceLibrary(const QString& category = QStringLiteral("summon"), int page = 0);
  void showReferenceRecord(const QString& category, const QString& id, bool raw = false);
  bool loadReplayBytes(const QByteArray& bytes, const QString& label, const QString& itemKey = {});
  void installReplay(BattleReplay::Replay result, BattleReplayCatalogHandle catalog,
                     const QString& label, const QString& itemKey, bool preserveSettings = false);
  void showIncomingReplays();
  void openIncomingReplay(const QString& sha256);
  void updateIncomingButton();
  void showCurrentDiagnostics();
  void showTeamDetails(int side, bool raw = false);
  QString tableReferencesHtml(const QString& id, bool skill, bool raw = true) const;
  QString officialEntriesHtml(const QString& id, bool skill, bool raw = true) const;
  QString catalogStateHtml() const;
  void updateCatalogStatus();
  void clearReferenceCatalogCache();
  void renderEffects();
  void renderOverview();
  void openBoardDetail(bool reference = false);
  QString unitCaption(const QString& key) const;
  QString effectSummary(const BattleReplay::Effect& effect) const;
  void updateNotes();
  void pause();
  void clearPlaybackPhase();
  void rebuildPlaybackPhases();
  void startContinuousPlayback();
  void enterPlaybackPhase(int phase, bool play);
  void finishPlaybackPhase();
  void updatePlaybackProgress(qreal progress);
  int playbackDuration() const;
  void fitBoardHeight();
  void positionCastingPanel();
  void updateTimelineFilter();
  void jumpEvent(int direction);
  void seekActionStart(int step);
  int nextVisibleStep(int direction) const;
  int nextPlaybackStep() const;
  int nextEffectIndex(int direction) const;
  bool advanceEffect(int direction);
  BattleReplay::Replay replay_;
  QJsonObject catalog_;
  // New imports use the latest published snapshot; the open replay keeps its
  // original snapshot for parsing, navigation, labels and reference lookups.
  BattleReplayCatalogHandle latestCatalog_, replayCatalog_;
  QString catalogMessage_;
  bool catalogBusy_ = false;
  QSet<QString> empowered_;
  QMap<QString, QString> empowermentNotes_;
  QMap<QString, BattleReplay::EmpowermentEvidence> empowermentEvidence_, selfTransformEvidence_;
  QString file_, selectedUnit_, reportDirectory_;
  QString accountName_, accountEvidence_, sourceStatus_;
  bool accountReliable_ = false;
  struct IncomingReplay { QString label, sha256; QByteArray bytes; };
  QList<IncomingReplay> incomingReplays_;
  QMap<QString, QByteArray> memoryReplays_;
  int droppedIncoming_ = 0;
  int current_ = 0;
  int moment_ = -1;
  int effect_ = -1;
  QComboBox *files_, *side_, *rounds_, *timelineView_;
  QToolButton *incomingButton_, *configurationButton_, *diagnosticsButton_, *sourceButton_;
  QToolButton *catalogReloadButton_, *catalogApplyButton_, *referencesButton_;
  QComboBox* moments_;
  QComboBox* playbackMode_;
  QComboBox* numberFormat_;
  QPushButton *previousMoment_, *nextMoment_;
  QLabel *status_, *stepLabel_, *catalogStatus_;
  QLabel* smallTurnLabel_;
  std::array<QLabel*, 2> teamLabels_, teamEffects_, queues_;
  std::array<QLabel*, 2> dragonBars_;
  std::array<QWidget*, 2> teamSources_{};
  std::array<QTextBrowser*, 2> teamBuffs_;
  std::array<QComboBox*, 2> players_;
  std::array<ReplaySpiritBar*, 2> spirits_;
  bool followPlayers_ = true;
  std::array<ReplayCell*, 18> cells_{};
  QPushButton *play_, *previous_, *next_, *restart_;
  ReplayLifeTimeline* slider_;
  QSpinBox* jump_;
  QTimer* timer_;
  struct PlaybackPhase {
    QString kind, title, actor;
    QList<int> effects;
    int firstMoment = -1, lastMoment = -1, beforeMoment = -1;
    int durationMs = 600;
    bool targets = false, outcomes = false;
    // A reading slice uses recorded balances, never interpolated game state.
    std::optional<QMap<QString, BattleReplay::Fighter>> numericBefore, numericState;
    int numericSegment = 0, numericSegmentCount = 0;
  };
  QList<PlaybackPhase> playbackPhases_;
  QMap<int, int> meterEventGroups_;
  int playbackPhase_ = -1;
  bool playing_ = false;
  bool playbackCompleted_ = false;
  qreal playbackProgress_ = 1;
  QVariantAnimation* phaseAnimation_;
  QComboBox* speed_;
  QProgressBar* phaseProgressBar_;
  QLabel* phaseTrail_;
  QTableView* timeline_;
  QComboBox *unitFilter_, *eventType_;
  QCheckBox* important_;
  QCheckBox* evidence_;
  QCheckBox* animations_;
  QWidget *arena_ = nullptr, *detailPane_, *microControls_;
  QWidget* castingAnchor_ = nullptr;
  QFrame* castingPanel_ = nullptr;
  QScrollArea* arenaScroll_ = nullptr;
  QFrame* boardInspector_;
  QTextBrowser* boardDetails_;
  QWidget* referenceControls_;
  QComboBox* referenceCategory_;
  QLineEdit* referenceSearch_;
  QJsonObject referenceRows_;
  QString referenceRowsCategory_;
  QStringList referenceSortedIds_, referenceMatches_;
  QMap<QString, QString> referenceSearchIndex_;
  QString referenceQuery_;
  QLabel* filterCount_;
  QLabel *relation_, *outcome_;
  QLabel *skillTitle_, *skillSide_, *actionPhase_;
  QListWidget* effectList_;
  QCheckBox* allEffects_;
  QTabWidget* tabs_;
  ReplayLinks* links_;
  ReplayMeterOverlay* meterOverlay_;
  ReplayStepFilter* stepsProxy_;
  QTextBrowser *overview_, *details_, *unit_, *notes_;
  QPlainTextEdit *rawStep_, *rawAll_;
  QLineEdit* search_;
  ReplayStepModel* stepsModel_;
  ReplayFieldModel* fieldsModel_;
  QSortFilterProxyModel* fieldsProxy_;
};
