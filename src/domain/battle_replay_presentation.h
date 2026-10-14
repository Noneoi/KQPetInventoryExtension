#pragma once

#include "domain/battle_replay.h"

namespace BattleReplay {

// An official CLIENT display decision, not a server calculation or a logged
// combat judgement. The recipient is the card on which the client shows it.
struct ClientPresentationCue {
  QString kind, recipient, source, target, label, explanation;
  QStringList evidence;
  QString attributeId;
};

// phaseBefore must be the observation immediately before this moment, never
// the action's final snapshot. Only the actual attack/counter result boundary
// produces cues. Unknown configuration or identity leaves that cue absent.
QList<ClientPresentationCue> clientPresentationCues(
    const Step& step, const Moment& moment,
    const QMap<QString, Fighter>& phaseBefore, const QJsonObject& catalog);

}  // namespace BattleReplay
