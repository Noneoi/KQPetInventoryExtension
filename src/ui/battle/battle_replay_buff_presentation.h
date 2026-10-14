#pragma once

#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

// Positive evidence of an official explanation is separate from a Buff's
// identity, its observed runtime role and its movie. No name-based partition.
// Source: BufferDescDefine; BattleAvatar.playBuffEffect; BufferConsts.
inline QStringList replayBuffStateCues(const QJsonObject& config) {
  static const QSet<QString> attributes{
    QStringLiteral("物攻"), QStringLiteral("魔攻"), QStringLiteral("超攻"), QStringLiteral("攻"), QStringLiteral("命中"),
    QStringLiteral("暴击"), QStringLiteral("破击"), QStringLiteral("伤害"), QStringLiteral("物防"), QStringLiteral("魔防"),
    QStringLiteral("超防"), QStringLiteral("防"), QStringLiteral("闪避"), QStringLiteral("格挡"), QStringLiteral("防暴"),
    QStringLiteral("减伤"), QStringLiteral("受伤加深"), QStringLiteral("全属性"), QStringLiteral("超物攻"),
    QStringLiteral("超魔攻"), QStringLiteral("超物防"), QStringLiteral("超魔防")};
  static const QSet<QString> special{
    QStringLiteral("战魂"), QStringLiteral("不灭意志"), QStringLiteral("九天印记*1"), QStringLiteral("九天印记*2"),
    QStringLiteral("九天印记*3"), QStringLiteral("印记*1"), QStringLiteral("印记*2"), QStringLiteral("印记*3"),
    QStringLiteral("睡眠"), QStringLiteral("控制"), QStringLiteral("禁锢"), QStringLiteral("清除增益")};
  static const QRegularExpression change(QStringLiteral("^(.+?)(↑+|↓+)$"));
  const auto effect = config[QStringLiteral("beffect")].toString();
  QStringList result;
  if (special.contains(effect)) result.append(effect);
  else for (const auto& part : effect.split(QChar(0xff0c), Qt::SkipEmptyParts)) {
    const auto match = change.match(part);
    if (part == QStringLiteral("潜能爆发") || (match.hasMatch() && attributes.contains(match.captured(1)))) result.append(part);
  }
  result.removeDuplicates();
  return result;
}


inline bool replayBuffHasExplanation(const QJsonObject& config) {
  // A message is an official displayed explanation, not verification of a
  // server formula. bhs only annotates a recorded number; it is not a state.
  auto message = config[QStringLiteral("mess")].toString();
  message.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
  return !message.trimmed().isEmpty() || !replayBuffStateCues(config).isEmpty();
}

inline QString replayBuffAvailability(const QJsonObject& config) {
  if (config.isEmpty()) return QStringLiteral("本资料库未收录此 Buff 配置；已记录的增删和数值仍保留，表现规则待核对。");
  QStringList missing;
  if (config[QStringLiteral("nm")].toString().trimmed().isEmpty()) missing << QStringLiteral("名称");
  if (config[QStringLiteral("mess")].toString().trimmed().isEmpty()) missing << QStringLiteral("独立说明");
  return QStringLiteral("官方 Buff 配置已收录。") + (missing.isEmpty() ? QString() :
    QStringLiteral("官方%1为空，不影响显示战报已记录的增删和数值。").arg(missing.join(QStringLiteral("、"))));
}

enum class ReplayBuffSourceSide { Own = 0, Enemy = 1, Unknown = 2 };

inline ReplayBuffSourceSide replayBuffSourceSide(const QString& source, int sourceTeam, int ownTeam) {
  // A team number alone is not evidence of an applier. In particular, never
  // substitute the holder's side or the currently acting unit here.
  if (source.isEmpty() || sourceTeam < 0 || ownTeam < 0) return ReplayBuffSourceSide::Unknown;
  return sourceTeam == ownTeam ? ReplayBuffSourceSide::Own : ReplayBuffSourceSide::Enemy;
}

inline QString replayBuffMessage(QString officialHtml, const QString& actualName) {
  // BattleMessageUtil substitutes the affected pet's name. Preserve the
  // official markup, but do not interpret punctuation in an actual name as HTML.
  if (!actualName.isEmpty()) officialHtml.replace(QStringLiteral("#pet#"), actualName.toHtmlEscaped());
  return officialHtml;
}

// These flags control the official avatar, not the lifetime of a Buff member.
// Keep them separate from the recorded state and from applier provenance.
struct ReplayBuffPresentation {
  QStringList labels, explanations;
};

inline ReplayBuffPresentation replayBuffPresentation(const QJsonObject& config) {
  ReplayBuffPresentation result;
  const auto enabled = [&](const QString& field) {
    const auto value = config[field];
    return value.isString() ? value.toString().toInt() == 1 : value.isDouble() && value.toInt() == 1;
  };
  // BufferDefine.parseDefine; BattleAvatar.addBuff / endBuff.
  if (enabled(QStringLiteral("hd"))) {
    result.labels.append(QStringLiteral("隐藏附着动画"));
    result.explanations.append(QStringLiteral("官方隐藏该 Buff 的附着动画；已记录的 Buff 仍保留在当前集合中。"));
  }
  if (enabled(QStringLiteral("once"))) {
    result.labels.append(QStringLiteral("动画仅播一次"));
    result.explanations.append(QStringLiteral("官方附着动画只播放一次；动画结束不表示 Buff 已移除，成员按战报增删记录维护。"));
  }
  const auto integer = [&](const QString& field) {
    const auto value = config[field];
    return value.isString() ? value.toString().toInt() : value.toInt();
  };
  const int wait = integer(QStringLiteral("buffWaitType")), frame = integer(QStringLiteral("buffWaitEndFrame"));
  if (wait > 0) {
    result.labels.append(QStringLiteral("播放等待类型 %1").arg(wait));
    result.explanations.append(QStringLiteral("官方 buffWaitType=%1；这是播放等待配置，不表示状态持续时间或层数。").arg(wait));
  }
  if (frame > 0) {
    result.labels.append(QStringLiteral("回调帧 %1").arg(frame));
    result.explanations.append(QStringLiteral("官方 buffWaitEndFrame=%1；素材回调帧不是 Buff 剩余回合。").arg(frame));
  }
  // BattleAvatar.showBubble / showPowerBubble test these exact substrings.
  const auto effect = config[QStringLiteral("beffect")].toString();
  if (effect.contains(QStringLiteral("血量不显示"))) {
    result.labels.append(QStringLiteral("原版隐藏生命飘字"));
    result.explanations.append(QStringLiteral("官方可抑制此 Buff 关联的生命飘字；本回放仍在生命条内显示战报实际数值。"));
  }
  if (effect.contains(QStringLiteral("气势不显示"))) {
    result.labels.append(QStringLiteral("原版隐藏气势飘字"));
    result.explanations.append(QStringLiteral("官方可抑制此 Buff 关联的气势飘字；本回放仍在气势条内显示战报实际数值。"));
  }
  return result;
}

// Keep exact playback parameters in raw details, never infer gameplay meaning.
inline ReplayBuffPresentation replayBuffPublicPresentation(const QJsonObject& config) {
  auto result = replayBuffPresentation(config);
  for (int i = 0; i < result.labels.size(); ++i) {
    if (result.labels[i].startsWith(QStringLiteral("播放等待类型"))) {
      result.labels[i] = QStringLiteral("等待播放完成");
      result.explanations[i] = QStringLiteral("客户端设置了播放等待；这不是效果的持续回合。");
    } else if (result.labels[i].startsWith(QStringLiteral("回调帧"))) {
      result.labels[i] = QStringLiteral("按动画进度继续");
      result.explanations[i] = QStringLiteral("动画到达指定进度后继续；这不是效果的剩余回合。");
    }
  }
  return result;
}
