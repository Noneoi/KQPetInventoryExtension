#pragma once
// Shared audit contract copied from the previously verified native preview
// audit on 2026-10-14. It deliberately uses only QtCore and domain values;
// downloaded candidates cannot change this raw-log invariant checker.
#include "domain/battle_replay.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>
#include <cmath>

struct BattleReplayEffectAudit {
  QString effectsSha256;
  qint64 actions = 0, moments = 0, effects = 0, numericEffects = 0, buffEffects = 0, progressEffects = 0, appearances = 0;
  qint64 momentErrors = 0, summaryErrors = 0, beforeAfterErrors = 0, snapshotErrors = 0, attributionErrors = 0, rawPathErrors = 0;
  QJsonArray examples;
  qint64 errors() const { return momentErrors + summaryErrors + beforeAfterErrors + snapshotErrors + attributionErrors + rawPathErrors; }
  QJsonObject report() const {
    return {{"actions", double(actions)}, {"moments", double(moments)}, {"effects", double(effects)},
      {"numericEffects", double(numericEffects)}, {"buffEffects", double(buffEffects)}, {"progressEffects", double(progressEffects)},
      {"appearances", double(appearances)}, {"momentErrors", double(momentErrors)}, {"summaryErrors", double(summaryErrors)},
      {"beforeAfterErrors", double(beforeAfterErrors)}, {"snapshotErrors", double(snapshotErrors)},
      {"attributionErrors", double(attributionErrors)}, {"rawPathErrors", double(rawPathErrors)},
      {"errors", double(errors())}, {"examples", examples}};
  }
};

namespace BattleReplayEffectAuditDetail {
inline std::optional<qint64> number(const QJsonValue& v) {
  if (v.isString()) {
    bool ok = false; const auto result = v.toString().toLongLong(&ok);
    if (ok && result >= -9007199254740991LL && result <= 9007199254740991LL) return result;
  } else if (v.isDouble()) {
    const auto result = v.toDouble();
    if (std::isfinite(result) && std::floor(result) == result && std::abs(result) <= 9007199254740991.0) return qint64(result);
  }
  return {};
}
inline bool same(const BattleReplay::Effect& a, const BattleReplay::Effect& b) {
  return a.kind == b.kind && a.source == b.source && a.target == b.target && a.cause == b.cause &&
    a.causeId == b.causeId && a.causeName == b.causeName && a.metric == b.metric && a.text == b.text &&
    a.path == b.path && a.raw == b.raw && a.before == b.before && a.after == b.after && a.delta == b.delta &&
    a.changed == b.changed && a.moment == b.moment && a.recordedBefore == b.recordedBefore &&
    a.recordedAfter == b.recordedAfter && a.recordedDelta == b.recordedDelta &&
    a.buffWaitType == b.buffWaitType && a.buffPlaybackBatch == b.buffPlaybackBatch &&
    a.afterWaitCallback == b.afterWaitCallback && a.visibilityCommand == b.visibilityCommand &&
    a.restoresDisplayLayers == b.restoresDisplayLayers;
}
inline QJsonValue lookup(QJsonValue value, const QString& path) {
  for (auto token : path.split(QLatin1Char('/'), Qt::KeepEmptyParts).mid(1)) {
    token.replace(QStringLiteral("~1"), QStringLiteral("/")).replace(QStringLiteral("~0"), QStringLiteral("~"));
    if (value.isObject()) value = value.toObject().value(token);
    else if (value.isArray()) {
      bool ok = false; const int i = token.toInt(&ok); const auto a = value.toArray();
      if (!ok || i < 0 || i >= a.size()) return QJsonValue(QJsonValue::Undefined);
      value = a[i];
    } else return QJsonValue(QJsonValue::Undefined);
  }
  return value;
}
inline std::optional<qint64> metric(const BattleReplay::Fighter& f, const QString& name) {
  if (name == QStringLiteral("生命")) return f.hp;
  if (name == QStringLiteral("生命上限")) return f.maxHp;
  if (name == QStringLiteral("护盾")) return f.shield;
  if (name == QStringLiteral("气势")) return f.power;
  return {};
}
inline void setMetric(BattleReplay::Fighter& f, const QString& name, std::optional<qint64> value) {
  if (name == QStringLiteral("生命")) f.hp = value;
  else if (name == QStringLiteral("生命上限")) f.maxHp = value;
  else if (name == QStringLiteral("护盾")) f.shield = value;
  else if (name == QStringLiteral("气势")) f.power = value;
}
inline bool projected(const BattleReplay::Effect& e) {
  return e.kind == QStringLiteral("target") || e.kind == QStringLiteral("team-target") ||
    e.kind == QStringLiteral("value") || e.kind == QStringLiteral("buff") ||
    e.kind == QStringLiteral("progress") || e.kind == QStringLiteral("transform") || e.kind == QStringLiteral("attributes") ||
    (e.kind == QStringLiteral("observation") && e.cause == QStringLiteral("record"));
}
inline QByteArray projection(const BattleReplay::Effect& e) {
  QJsonValue delta(QJsonValue::Null);
  if ((e.kind == QStringLiteral("value") && e.cause != QStringLiteral("record")) || e.kind == QStringLiteral("progress"))
    if (e.delta) delta = double(*e.delta);
  return QJsonDocument(QJsonArray{e.kind, e.cause, e.source, e.target, e.causeId, e.metric, e.path, e.raw, delta}).toJson(QJsonDocument::Compact);
}
}

// No catalog assumptions: the Python companion independently checks the raw
// relation multiset and official command gates against effectsSha256.
inline BattleReplayEffectAudit auditBattleReplayEffects(const BattleReplay::Replay& replay) {
  using namespace BattleReplayEffectAuditDetail;
  BattleReplayEffectAudit out;
  QCryptographicHash digest(QCryptographicHash::Sha256);
  for (const auto& step : replay.steps) {
    if (step.moments.isEmpty()) continue;
    ++out.actions; out.moments += step.moments.size();
    QList<QByteArray> rows;
    for (const auto& effect : step.effects) if (projected(effect)) rows.append(projection(effect));
    std::sort(rows.begin(), rows.end()); digest.addData((step.path + QLatin1Char('\n')).toUtf8());
    for (const auto& row : rows) { digest.addData(row); digest.addData(QByteArrayLiteral("\n")); }
    QStringList counterSources;
    for (const auto& v : step.raw.toObject()[QStringLiteral("trl")].toArray()) {
      const auto t = v.toObject();
      if (t[QStringLiteral("isse")].toBool() && t[QStringLiteral("tn")].isObject())
        counterSources.append(BattleReplay::fighterKey(t[QStringLiteral("tti")].toInt(-1), BattleReplay::valueText(t[QStringLiteral("tpn")]),
          t[QStringLiteral("tci")].isUndefined() || t[QStringLiteral("tci")].isNull() ? QStringLiteral("@team") : BattleReplay::valueText(t[QStringLiteral("tci")])));
    }
    qsizetype summary = 0;
    for (int mi = 0; mi < step.moments.size(); ++mi) {
      const auto& moment = step.moments[mi];
      auto live = mi > 0 ? step.moments[mi - 1].state : moment.state;
      QMap<QString, QSet<QString>> touched;
      QSet<QString> touchedBuffs;
      const auto error = [&](qint64& counter, const QString& path, const QString& message) {
        ++counter;
        if (out.examples.size() < 8) out.examples.append(QJsonObject{{"step", step.path}, {"moment", mi}, {"path", path}, {"message", message}});
      };
      for (const auto& e : moment.effects) {
        ++out.effects;
        if (e.moment != mi) error(out.momentErrors, e.path, QStringLiteral("Effect moment differs from containing snapshot"));
        if (summary >= step.effects.size() || !same(e, step.effects[summary]))
          error(out.summaryErrors, e.path, QStringLiteral("Step effects differ from the ordered concatenation of moments"));
        ++summary;
        if (e.text.isEmpty() || !e.path.startsWith(step.path + QLatin1Char('/')))
          error(out.rawPathErrors, e.path, QStringLiteral("Missing effect description or action-scoped path"));
        const auto original = lookup(replay.root, e.path);
        const bool stringToken = original.isString() && e.raw.isString() &&
          original.toString().split(QLatin1Char(','), Qt::SkipEmptyParts).contains(e.raw.toString());
        if (original.isUndefined() || (original != e.raw && !stringToken))
          error(out.rawPathErrors, e.path, QStringLiteral("Effect raw value does not match its original path or encoded entry"));
        if (!moment.state.contains(e.target)) error(out.attributionErrors, e.path, QStringLiteral("Unknown recipient identity"));
        if (e.cause == QStringLiteral("skill") || e.cause == QStringLiteral("team") || e.cause == QStringLiteral("target")) {
          if (e.source != step.actor) error(out.attributionErrors, e.path, QStringLiteral("Direct action has the wrong source"));
        } else if (e.cause == QStringLiteral("counter")) {
          if (counterSources.size() != 1 || e.source != counterSources.value(0))
            error(out.attributionErrors, e.path, QStringLiteral("Counter source is not the sole effective target"));
        } else if (e.cause == QStringLiteral("buff") || e.cause == QStringLiteral("record") || e.cause == QStringLiteral("calibration")) {
          if (!e.source.isEmpty()) error(out.attributionErrors, e.path, QStringLiteral("Unrecorded applier was inferred"));
        } else error(out.attributionErrors, e.path, QStringLiteral("Unverified attribution category"));

        if (e.kind == QStringLiteral("value") || e.cause == QStringLiteral("calibration")) {
          ++out.numericEffects;
          if (e.before != metric(live.value(e.target), e.metric))
            error(out.beforeAfterErrors, e.path, QStringLiteral("Numeric before value differs from the preceding state"));
          std::optional<qint64> expected;
          if (e.cause == QStringLiteral("calibration")) expected = number(e.raw);
          else if (e.cause == QStringLiteral("record"))
            expected = number(e.raw.toObject()[e.metric == QStringLiteral("生命") ? QStringLiteral("new_curv") : QStringLiteral("new_maxv")]);
          else if (e.before && e.delta) {
            const auto sum = *e.before + *e.delta;
            if (sum >= -9007199254740991LL && sum <= 9007199254740991LL) expected = sum;
          }
          if (e.metric == QStringLiteral("生命") && expected) expected = std::max<qint64>(0, *expected);
          if (e.after != expected) error(out.beforeAfterErrors, e.path, QStringLiteral("Numeric after value differs from the recorded operation"));
          if (e.cause == QStringLiteral("record")) {
            // t=2 supplies its own old/new values for the official displayed
            // difference. Its old value need not equal our preceding snapshot:
            // keep both facts and do not silently replace the recorded delta.
            const auto raw = e.raw.toObject();
            const bool life = e.metric == QStringLiteral("生命");
            const auto recordedBefore = number(raw[life ? QStringLiteral("old_curv") : QStringLiteral("old_maxv")]);
            const auto recordedAfter = number(raw[life ? QStringLiteral("new_curv") : QStringLiteral("new_maxv")]);
            const auto expectedDelta = recordedBefore && recordedAfter
              ? std::optional<qint64>(*recordedAfter - *recordedBefore) : std::nullopt;
            if (e.recordedBefore != recordedBefore || e.recordedAfter != recordedAfter || e.recordedDelta != expectedDelta)
              error(out.beforeAfterErrors, e.path, QStringLiteral("Special-record display values differ from its original old/new fields"));
          }
          if (e.cause == QStringLiteral("calibration") || e.cause == QStringLiteral("record")) {
            const auto expectedDelta = e.before && e.after ? std::optional<qint64>(*e.after - *e.before) : std::nullopt;
            if (e.delta != expectedDelta) error(out.beforeAfterErrors, e.path, QStringLiteral("Snapshot difference is inconsistent"));
          }
          if (e.cause == QStringLiteral("calibration") && e.before && e.after && e.changed != (e.before != e.after))
            error(out.beforeAfterErrors, e.path, QStringLiteral("Unchanged calibration incorrectly marked as a change"));
          setMetric(live[e.target], e.metric, e.after); touched[e.target].insert(e.metric);
        } else if (e.kind == QStringLiteral("buff")) {
          ++out.buffEffects;
          auto& f = live[e.target]; const auto before = f.buffs.count(e.causeId);
          const auto parts = e.raw.toString().split(QLatin1Char('_'));
          const auto operation = parts.size() == 3 ? number(parts[0]) : std::nullopt;
          if (operation == 1) f.buffs.append(e.causeId);
          else if (operation == 0) f.buffs.removeOne(e.causeId);
          else error(out.beforeAfterErrors, e.path, QStringLiteral("Unknown Buff operation"));
          const auto after = f.buffs.count(e.causeId);
          if (e.before != before || e.after != after || e.delta != after - before)
            error(out.beforeAfterErrors, e.path, QStringLiteral("Buff membership before/after differs from the preceding state"));
          touchedBuffs.insert(e.target);
        } else if (e.kind == QStringLiteral("progress")) {
          ++out.progressEffects; const auto raw = e.raw.toObject();
          if (e.before != number(raw[QStringLiteral("prob")]) || e.after != number(raw[QStringLiteral("proa")]) || e.delta != number(raw[QStringLiteral("vc")]))
            error(out.beforeAfterErrors, e.path, QStringLiteral("Progress endpoints or delta differ from the log"));
        } else if (e.kind == QStringLiteral("appearance")) {
          ++out.appearances; const auto f = moment.state.value(e.target);
          if (!f.entered || !f.entryKnown) error(out.snapshotErrors, e.path, QStringLiteral("Appearance effect has no confirmed appearance snapshot"));
        }
      }
      for (auto it = touched.cbegin(); it != touched.cend(); ++it) for (const auto& name : it.value())
        if (metric(live.value(it.key()), name) != metric(moment.state.value(it.key()), name))
          error(out.snapshotErrors, step.path, QStringLiteral("Final numeric effect differs from the moment snapshot"));
      for (const auto& identity : touchedBuffs)
        if (live.value(identity).buffs != moment.state.value(identity).buffs)
          error(out.snapshotErrors, step.path, QStringLiteral("Buff effect sequence differs from the moment snapshot"));
    }
    if (summary != step.effects.size()) ++out.summaryErrors;
  }
  out.effectsSha256 = QString::fromLatin1(digest.result().toHex());
  return out;
}
