#pragma once

#include "ui/battle/battle_replay_links.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QLabel>
#include <QMap>
#include <QVariantMap>

// Presentation-only fixtures: their captions are supplied text, not claims
// about the attributes or results of a particular official pet/skill.
inline bool verifyRelationRibbonCaptions() {
  const auto fail = [](const char* reason) { qCritical() << "arrow ribbon captions:" << reason; return false; };
  QWidget board; board.resize(1040, 610); board.setStyleSheet(QStringLiteral("background:#f2f5f8"));
  const auto cell = [&](const QString& key, const QRect& geometry, const QString& title) {
    auto* widget = new QWidget(&board); widget->setObjectName(QStringLiteral("replayCell") + key); widget->setGeometry(geometry);
    widget->setStyleSheet(QStringLiteral("background:#e6edf4;border:1px solid #b7c8d8;border-radius:6px"));
    auto* name = new QLabel(title, widget); name->setObjectName(QStringLiteral("unitName")); name->setGeometry(8, 8, geometry.width() - 16, 24);
    name->setStyleSheet(QStringLiteral("color:#1c2b3a;border:none;font-weight:bold"));
    for (int i = 0; i < 3; ++i) {
      auto* meter = new QLabel(QStringLiteral("100 / 100"), widget);
      meter->setObjectName(QStringList{QStringLiteral("hpBar"), QStringLiteral("shieldValue"), QStringLiteral("powerBar")}[i]);
      meter->setGeometry(10, 42 + i * 26, geometry.width() - 20, 20);
      meter->setStyleSheet(QStringLiteral("background:%1;color:#102030;border:0;border-radius:3px")
        .arg(QStringList{QStringLiteral("#ee9a9a"), QStringLiteral("#91beeb"), QStringLiteral("#eec687")}[i]));
      meter->setAlignment(Qt::AlignCenter);
    }
    return widget;
  };
  auto* source = cell(QStringLiteral("Source"), QRect(80, 225, 190, 190), QStringLiteral("行动精灵（合成展示）"));
  auto* enemy = cell(QStringLiteral("Enemy"), QRect(780, 70, 190, 190), QStringLiteral("较远目标"));
  auto* ally = cell(QStringLiteral("Ally"), QRect(280, 225, 190, 190), QStringLiteral("相邻队友"));
  auto* lower = cell(QStringLiteral("Lower"), QRect(80, 425, 190, 150), QStringLiteral("下方队友"));
  auto* other = cell(QStringLiteral("Other"), QRect(520, 95, 190, 180), QStringLiteral("另一目标"));
  ReplayLinks links(&board);
  QList<ReplayRelation> relations;
  const auto add = [&](QWidget* target, int targetTeam, const QString& key, const QStringList& labels) {
    ReplayRelation relation; relation.source = source; relation.target = target; relation.sourceTeam = 0;
    relation.targetTeam = targetTeam; relation.sourceKey = QStringLiteral("source"); relation.targetKey = key;
    relation.labels = labels; relations.append(relation);
  };
  add(enemy, 1, QStringLiteral("enemy"), {QStringLiteral("神草强效")});
  add(ally, 0, QStringLiteral("ally"), {QStringLiteral("弱效")});
  add(lower, 0, QStringLiteral("lower"), {QStringLiteral("神火强效")});
  add(other, 1, QStringLiteral("other"), {QStringLiteral("神草强效"), QStringLiteral("神水强效"), QStringLiteral("神火强效")});
  add(source, 0, QStringLiteral("self"), {QStringLiteral("开局幻元")});
  links.setRelations(relations, QStringLiteral("caption-fixture"), false);
  board.show(); QCoreApplication::processEvents(); const auto capture = board.grab();
  const auto directory = qEnvironmentVariable("KQPET_REPLAY_QA_DIR");
  if (!directory.isEmpty()) {
    QDir().mkpath(directory);
    if (!capture.save(QDir(directory).filePath(QStringLiteral("arrow-ribbon-captions.png")))) return fail("capture failed");
  }
  if (links.property("captionPlacement").toString() != QStringLiteral("inside-arrow")) return fail("caption style still detached");
  const auto layouts = links.property("relationCaptionLayouts").toList();
  if (layouts.size() != relations.size()) return fail("a short, long or self relation lost its text");
  QMap<QString, QWidget*> targets{{QStringLiteral("enemy"), enemy}, {QStringLiteral("ally"), ally},
    {QStringLiteral("lower"), lower}, {QStringLiteral("other"), other}, {QStringLiteral("self"), source}};
  QList<QRectF> captions;
  for (const auto& entry : layouts) {
    const auto item = entry.toMap(); const auto key = item[QStringLiteral("targetKey")].toString();
    const auto rect = item[QStringLiteral("captionRect")].toRectF();
    const auto text = item[QStringLiteral("textRect")].toRectF();
    const auto tip = item[QStringLiteral("targetTip")].toPointF();
    const auto head = item[QStringLiteral("headBounds")].toRectF();
    if (!targets.contains(key) || !QRectF(board.rect()).contains(rect) || !item[QStringLiteral("textInsideArrow")].toBool() ||
        item[QStringLiteral("placement")].toString() != QStringLiteral("inside-arrow"))
      return fail("caption escaped the continuous arrow or the board");
    if (!rect.contains(text) || rect.intersects(head) || rect.contains(tip)) return fail("caption hid the arrowhead");
    const QRectF targetRect(targets[key]->geometry());
    if (!targetRect.adjusted(-4, -4, 4, 4).contains(tip)) return fail("arrow tip no longer meets its target");
    for (const auto& earlier : captions) if (earlier.intersects(rect)) {
      qCritical() << "overlapping target" << key << rect << "earlier" << earlier;
      return fail("multi-target text overlaps");
    }
    captions.append(rect);
    const bool enemyTarget = key == QStringLiteral("enemy") || key == QStringLiteral("other");
    if (item[QStringLiteral("color")].toString() != (enemyTarget ? QStringLiteral("red") : QStringLiteral("green")))
      return fail("caption changed relative allegiance colors");
    if (key == QStringLiteral("other") && item[QStringLiteral("text")].toString() != QStringLiteral("神草强效 · 神水强效 · 神火强效"))
      return fail("multiple attribute labels were truncated");
  }
  // A source-less observation must not be turned into an invented attack
  // arrow merely to make room for its caption.
  ReplayRelation unknown; unknown.target = enemy; unknown.targetKey = QStringLiteral("unknown"); unknown.labels = {QStringLiteral("弱效")};
  links.setRelations({unknown}, QStringLiteral("unknown"), false); board.grab();
  if (!links.property("relationCaptionLayouts").toList().isEmpty() || links.property("connectionCount").toInt() != 0)
    return fail("source-less record acquired a fabricated arrow");
  source->setProperty("visualScale", 1.16);
  links.setRelations({relations[1]}, QStringLiteral("enlarged-adjacent"), false); board.grab();
  const auto enlarged = links.property("relationCaptionLayouts").toList();
  if (enlarged.size() != 1) return fail("enlarging actor removed its adjacent caption");
  const auto enlargedCaption = enlarged.first().toMap();
  const auto enlargedTip = enlargedCaption[QStringLiteral("targetTip")].toPointF();
  const auto enlargedHead = enlargedCaption[QStringLiteral("headBounds")].toRectF();
  if (enlargedTip.x() <= enlargedHead.center().x() || enlargedCaption[QStringLiteral("captionRect")].toRectF().contains(enlargedTip))
    return fail("overlapping enlarged borders reversed or covered the arrowhead");
  board.hide(); return true;
}
