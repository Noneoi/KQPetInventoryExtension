#pragma once
#include "domain/battle_replay.h"

namespace BattleReplay {
// Shared by raw parsing and direct command reconstruction. Deduplicates only
// the same reference in one action; later actions retain their own evidence.
void retainUnlinkedReference(Step& step, const UnlinkedReference& reference, QStringList& issues);
// Reconstruct command boundaries from official client consumption rules.
// Does not simulate skills or manufacture animation frames / server effects.
void advanceAction(Step& step, QMap<QString, Fighter>& state,
                   const QJsonObject& catalog, QStringList& issues);
}
