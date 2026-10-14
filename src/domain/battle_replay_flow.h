#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace BattleReplay {

enum class FlowPlanItemKind { SegmentBarrier, Action };

struct FlowPlanItem {
  FlowPlanItemKind kind = FlowPlanItemKind::SegmentBarrier;
  int frIndex = -1;
  int rlIndex = -1;  // A segment barrier has no action index.
  int lineIndex = -1;
  int batchId = -1;
  QString path;
};

struct BattleFlowPlan {
  // Barriers occur when a segment is admitted to a lane, before its pr.dl/ml
  // processing and shift callbacks finish. Actions with the same batchId run
  // together; lane enumeration does not establish their internal hit order.
  // A trailing cleanup barrier may have batchId == batchCount.
  QList<FlowPlanItem> items;
  // On failure these retain every unplanned segment/action in original order.
  // batchId is -1. The caller must not play an incomplete plan as confirmed.
  QList<FlowPlanItem> unscheduled;
  QStringList issues;
  int batchCount = 0;
  bool complete = false;
};

// Only plans the official three-lane command barriers. It never reads combat
// values, modifies the input, assigns animation durations or merges snapshots.
// flowPath is the original JSON path, e.g. /fr or /br/brs/2/fr.
BattleFlowPlan planBattleFlow(const QJsonArray& flow,
                             const QString& flowPath = QStringLiteral("/br/fr"));

}  // namespace BattleReplay
