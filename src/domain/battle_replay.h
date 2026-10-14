#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QMap>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <optional>

namespace BattleReplay {

// A membership entry records the Buff's applier, independently of its holder
// and of any later command that removes it. Empty source remains unknown.
struct BuffMember {
  QString id, source, sourceName, path;
  int sourceTeam = -1;
};

// A player's progress effects have independent balances. The initial ppbf and
// later t=4 records share the same Buff ID; one effect must not erase another.
struct TeamProgress {
  QString buffId, path;
  std::optional<qint64> current, maximum, previous, delta;
  std::optional<bool> skillEffected;
  QJsonObject raw;
};

// Observations, not a battle simulator. Identity includes team AND player:
// fighter IDs and cells are not unique across a battle.
struct Fighter {
  QString key, name, player, id, path;
  int team = -1, x = -1, y = -1;
  bool representer = false;
  QString presence;
  QString raceId, faceId, formName, normalSkill, uniqueSkill, superUniqueSkill;
  // Official transFromRaceId is an original-body resource relationship, not
  // this fighter's own identity. Chasers inherit the first non-chaser in their
  // initial cell; later transformations preserve that first origin.
  QString originRaceId, originFighterKey, originRelation, originPath;
  QStringList attributes;
  QString formPath, attributesPath, skillsPath, presencePath;
  bool attributesKnown = false, summoned = false, clone = false, cloneCalled = false;
  bool entered = true, entryKnown = true, chaser = false, playerRemoved = false;
  // BattleUtil.handleOneBuff can restore a summoned avatar without changing
  // FighterInfo's called-out flag. Keep that persistent client fact separate
  // from a confirmed entry command and from all recorded HP transitions.
  bool displayLayersRestored = false;
  QString displayLayersPath;
  // Player movement moves the whole formation, never a fighter's x/y cell.
  int playerX = -1, playerY = -1;
  std::optional<qint64> hp, maxHp, power, maxPower, shield;
  QString hpPath, maxHpPath, powerPath, shieldPath;
  // Client membership is a multiset: add appends; remove erases one occurrence.
  QStringList buffs, initialBuffs;
  QList<BuffMember> buffMembers;
  bool buffsKnown = true;
  QString buffPath;
  QJsonObject raw, lastNumbers, lastUnit;
  QMap<QString, TeamProgress> teamProgress;
};

inline bool presentedOnField(const Fighter& fighter) {
  return !fighter.playerRemoved &&
    (fighter.displayLayersRestored || (fighter.entryKnown && fighter.entered));
}

struct Detail {
  QString phase, object, text, path;
  QJsonValue raw;
};

// A displayed relation must distinguish the command's source from an unknown
// Buff applier. Empty source means the log does not identify that applier.
struct Effect {
  QString kind, source, target, cause, causeId, causeName, metric, text, path;
  QJsonValue raw;
  std::optional<qint64> before, after, delta;
  // t=2 supplies its own old/new values for official bubbles. Keep those apart
  // from observed state before/after/delta so ledger consistency stays testable.
  std::optional<qint64> recordedBefore, recordedAfter, recordedDelta;
  bool changed = true;
  int moment = -1;
  // Buff provenance belongs to the matched membership, never to a remover.
  QString buffSource, buffSourceName, buffSourcePath;
  int buffSourceTeam = -1;
  // Buff playback policy from the Flash client. Batch numbers are local to the
  // object's display-change list, never timestamps shared between fighters.
  int buffWaitType = -1, buffPlaybackBatch = -1;
  bool afterWaitCallback = false;
  // none / visible-or-skip / force-show. The report does not record Avatar
  // alpha; HP, entry flags and hidden Buff metadata cannot supply that value.
  QString buffWaitCondition;
  // A presentation directive (e.g. waitType 3 show-up), never an HP/entry reset.
  QString visibilityCommand;
  // BattleUtil.handleOneBuff invokes summonAppear at this exact add. This
  // restores client display layers; it never establishes an HP recovery.
  bool restoresDisplayLayers = false;
  // Recorded display operation, independent of the resulting member-count
  // delta. A clear with no matching member is still an operation 0 event.
  int buffOperation = -1; // 0 clear, 1 appear; -1 not a display-Buff event.
};

struct Moment {
  QString title, note;
  QList<Detail> details;
  QMap<QString, Fighter> state;
  QList<Effect> effects;
  // Playback metadata, independent of translated presentation text. Empty phase
  // remains valid for older/manually constructed snapshots. Known values:
  // setup, before, attack, counter, after, settle, unresolved.
  QString phase, actor;
  // Stable command boundary within the phase: initial, checks, wait-buff,
  // wait-tail, values, power-buff, calibration, objects, group-end, result or final.
  // In particular, wait-buff and objects must not be folded into value changes.
  QString part;
  // Zero-based cursor into EACH object's sorted changeset list, not a shared
  // raw changeset ID. Sequence skills fill missing IDs before this cursor runs.
  // Only before/after phases have a group index; other phases use -1.
  int groupIndex = -1;
};

struct KeyEvent { QString kind, key; int moment = -1; };

// A report may refer to an instance that the initial player never registered.
// Such evidence belongs to this action, never to a fabricated formation cell.
struct UnlinkedReference {
  QString kind, key, player, id, path;
  int team = -1;
  QJsonValue raw;
  QJsonObject numbers;
  QMap<QString, bool> flags;
  QString reason;
};

struct Step {
  int round = -1;
  QString title, actor, path;
  QStringList affected;
  QList<Detail> details;
  QMap<QString, Fighter> state;
  QJsonValue raw;
  QList<Moment> moments;
  QString schedule;
  QStringList participants, keyChanges;
  QList<KeyEvent> events;
  QList<Effect> effects;
  QList<UnlinkedReference> unlinkedReferences;
  // Logged skill identity is never overwritten by a combo or a team command.
  // A counter uses the target's current normal skill at the counter boundary.
  QString loggedSkillId, playbackSkillId, counterSkillId, executionName;
  // The last action of this BattleUnit, unless ANY target records a counter.
  // Dedicated last-effect definitions do not change the logged skill ID.
  bool isLastRound = false;
  QString lastSkillStatus = QStringLiteral("not-applicable");
  // Only set when FighterInfo's official desc == "内测" rule remaps the ID.
  QString testingSkillId;
  QStringList playbackNotes;
  // normal, summon, team-simple, team-skill, disabled, unresolved.
  QString playbackKind;
  // seqa/seqb inherit only within their BattleUnit. A known empty queue differs
  // from a queue that this log has not supplied. Player rounds persist by team.
  QMap<int, QStringList> actionQueues;
  QSet<int> knownActionQueues;
  // Queue IDs belong to this BattleUnit's pan/pbn, not a UI-selected player.
  QMap<int, QString> actionQueuePlayers;
  QMap<int, int> playerRounds;
  std::optional<bool> fromNormalQueue, overRoundLimit;
  // BattleSyncManager advances up to three lines in one callback barrier.
  // Parallel actions are separate views of the SAME starting observation;
  // their list order never claims an animation completion order.
  int lineIndex = -1, batchId = -1;
  bool parallelBatch = false;
  QMap<QString, Fighter> batchStartState;
  QStringList parallelConflictKeys;
};

struct Field {
  QString path, label;
  QJsonValue value;
  bool verified = false;
};

struct Replay {
  QByteArray original;
  QString sha256, error;
  QJsonObject root;
  QList<Step> steps;
  QList<Field> fields;
  QMap<int, QString> teams;
  QStringList issues;
  int actions = 0;
  // Official tbt/brs chooses one myBattle, not a sequence of fights. The full
  // outer document remains in root/original/fields; this index is zero-based.
  QString containerKind, battlePath;
  int selectedBattleIndex = -1, containerBattleCount = 0;
};

QString valueText(const QJsonValue& value);
QString petName(const QJsonObject& catalog, const QString& raceId);
QString fighterName(const Fighter& fighter);
QString referenceName(const QJsonObject& catalog, const QString& section, const QString& id);
QString fighterKey(int team, const QString& player, const QString& id);
// SkillResult maps null/missing tci to RepresentRoundResult's int.MAX_VALUE.
// Both spellings identify the player's representer; other IDs stay distinct.
QString targetFighterId(const QJsonValue& id);
// Directly constructed/older states may only carry buffs. Never attach stale
// provenance when the two ordered membership lists do not agree exactly.
QList<BuffMember> recordedBuffMembers(const Fighter& fighter);
void updatePresence(Fighter& fighter);
void updateFormReference(Fighter& fighter, const QJsonObject& catalog);
Replay parse(const QByteArray& bytes, const QJsonObject& officialCatalog = {});

} // namespace BattleReplay
