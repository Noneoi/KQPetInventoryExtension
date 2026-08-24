#include "pet_detail_renderer.h"

#include <QHash>
#include <QSet>
#include <QUrl>

namespace {

QString battlePowerAnalysisHtml(const PetDetailViewModel& model) {
  const PetBattlePowerState& state = model.battlePower;
  if (!state.hasCurrent || !state.hasExtreme) {
    return PetDetailRenderer::section(
        QStringLiteral("战斗力分析"),
        PetDetailRenderer::row(
            QStringLiteral("状态"),
            QStringLiteral("当前数据没有战斗力分项；仓库精灵请点击该行或使用“刷新仓库详情”。")));
  }

  QHash<QString, PetBattlePowerGap> gaps;
  for (const PetBattlePowerGap& component : state.componentGaps)
    gaps.insert(component.key, component);

  QStringList missing;
  QSet<QString> seenMissing;
  auto appendMissing = [&](const QString& value) {
    if (value.isEmpty() || seenMissing.contains(value)) return;
    seenMissing.insert(value);
    missing.append(PetDetailRenderer::text(value));
  };

  if (state.stargodSlotsKnown) {
    if (state.missingStars > 0)
      appendMissing(QStringLiteral("星神数量差 %1 个").arg(state.missingStars));
    const int goldOrRedStars = state.goldStars + state.redStars;
    const int missingGoldStars = qMax(0, state.stargodSlots - goldOrRedStars);
    if (missingGoldStars > 0)
      appendMissing(QStringLiteral("金色及以上星神差 %1 个").arg(missingGoldStars));
    if (state.hasChangeableSlot && state.changeableQuality < 5)
      appendMissing(QStringLiteral("万变星神未达到金色品质"));
  }
  if (!state.stargodLevelsFull && state.stargodLevelMissingSlots > 0) {
    appendMissing(QStringLiteral("星神等级还有 %1 个栏位未满")
                      .arg(state.stargodLevelMissingSlots));
  }

  static const QSet<QString> semanticComponents = {
      QStringLiteral("bsv"), QStringLiteral("asv"), QStringLiteral("sjv")};
  for (const PetBattlePowerGap& component : state.componentGaps) {
    if (semanticComponents.contains(component.key)) continue;
    appendMissing(QStringLiteral("%1差 %2 战力").arg(component.label).arg(component.gap));
  }

  if (gaps.contains(QStringLiteral("bsv"))) {
    if (model.badges.isEmpty()) appendMissing(QStringLiteral("元魂未装备"));
    QStringList exclusiveBadges;
    for (const PetBadgeSlot& badge : model.badges) {
      if (!badge.exclusiveName.isEmpty() && !badge.exclusiveAwakened)
        exclusiveBadges.append(badge.exclusiveName);
    }
    exclusiveBadges.removeDuplicates();
    if (!exclusiveBadges.isEmpty()) {
      appendMissing(QStringLiteral("专属元魂未觉醒：%1")
                        .arg(exclusiveBadges.join(QStringLiteral("、"))));
    }
    appendMissing(QStringLiteral("元魂差 %1 战力")
                      .arg(gaps.value(QStringLiteral("bsv")).gap));
  }

  bool sacredGapDescribed = false;
  if (model.sacred.equipped) {
    if (model.sacred.maxStar > 0 && model.sacred.star < model.sacred.maxStar) {
      appendMissing(QStringLiteral("神源兽星级差 %1 星")
                        .arg(model.sacred.maxStar - model.sacred.star));
      sacredGapDescribed = true;
    }
    if (model.sacred.maxStage > 0 && model.sacred.stage < model.sacred.maxStage) {
      appendMissing(QStringLiteral("神源兽阶级差 %1 阶")
                        .arg(model.sacred.maxStage - model.sacred.stage));
      sacredGapDescribed = true;
    }
  } else if (gaps.contains(QStringLiteral("sjv"))) {
    appendMissing(QStringLiteral("神源兽未装备"));
    sacredGapDescribed = true;
  }
  if (gaps.contains(QStringLiteral("sjv")) && !sacredGapDescribed) {
    appendMissing(QStringLiteral("神源兽差 %1 战力")
                      .arg(gaps.value(QStringLiteral("sjv")).gap));
  }

  int ordinaryAstrolabeUnlit = 0;
  QStringList exclusiveAstrolabeUnlit;
  for (const PetAstrolabeStar& star : model.astrolabe.stars) {
    if (star.activated) continue;
    if (star.exclusive)
      exclusiveAstrolabeUnlit.append(star.name);
    else
      ++ordinaryAstrolabeUnlit;
  }
  if (ordinaryAstrolabeUnlit > 0) {
    appendMissing(QStringLiteral("天迹星轮还有 %1 个未点亮")
                      .arg(ordinaryAstrolabeUnlit));
  }
  exclusiveAstrolabeUnlit.removeDuplicates();
  if (!exclusiveAstrolabeUnlit.isEmpty()) {
    appendMissing(QStringLiteral("专属星轮未点亮：%1")
                      .arg(exclusiveAstrolabeUnlit.join(QStringLiteral("、"))));
  }
  if (gaps.contains(QStringLiteral("asv")) && model.astrolabe.stars.isEmpty()) {
    appendMissing(QStringLiteral("天迹星轮差 %1 战力")
                      .arg(gaps.value(QStringLiteral("asv")).gap));
  }

  QString extremeAnalysis;
  if (missing.isEmpty()) {
    extremeAnalysis = QStringLiteral("<span class='ok'>已达到普通极限。</span>");
  } else {
    extremeAnalysis = QStringLiteral("<div class='warning'><b>未满：</b></div>"
                                     "<div>• %1</div>")
                          .arg(missing.join(QStringLiteral("</div><div>• ")));
  }
  if (state.currentLocallyCalculated && state.current < state.extreme) {
    extremeAnalysis += QStringLiteral(" 距离普通极限 <b>%1</b>。")
                           .arg(state.extreme - state.current);
  }

  QString highestAnalysis =
      QStringLiteral("<div>当前精灵装备及背包的红色星神为 <b>%1</b>，"
                     "距离满红色星神差 <b>%2</b>。</div>")
          .arg(state.redStars)
          .arg(state.missingRedStars);
  if (!state.stargodSlotsKnown) {
    highestAnalysis = QStringLiteral("<div class='warning'>星神栏位待确认。</div>");
  }
  if (state.hasChangeableSlot && !state.changeableRed)
    highestAnalysis += QStringLiteral("<div class='warning'>不是红色万变。</div>");
  if (!state.breakthrough)
    highestAnalysis += QStringLiteral("<div class='warning'>天迹星轮未突破。</div>");
  if (!state.hasHighest) {
    highestAnalysis += QStringLiteral("<div class='warning'>最高战力待确认。</div>");
  } else {
    highestAnalysis += state.isHighest
                           ? QStringLiteral("<div class='highest'>已达到最高战斗力。</div>")
                           : QStringLiteral("<div class='warning'>距离最高战斗力差 <b>%1</b>。</div>")
                                 .arg(state.highestGap);
  }

  QString rows = PetDetailRenderer::row(
      QStringLiteral("战力基准"),
      QStringLiteral("当前（服务器显示）<b>%1</b>　实际（星神重算后）<b>%2</b>　"
                     "普通极限 <b>%3</b>　最高 <b>%4</b>")
          .arg(state.serverCurrent)
          .arg(state.currentLocallyCalculated ? QString::number(state.current)
                                              : QStringLiteral("待确认"))
          .arg(state.extreme)
          .arg(state.hasHighest ? QString::number(state.highest)
                                : QStringLiteral("待确认")));
  rows += PetDetailRenderer::row(QStringLiteral("普通极限差距"), extremeAnalysis);
  rows += PetDetailRenderer::row(QStringLiteral("最高战力差距"), highestAnalysis);
  return PetDetailRenderer::section(QStringLiteral("战斗力分析"), rows);
}

QString talentHtml(const PetTalentState& talent) {
  QString rows = PetDetailRenderer::row(
      QStringLiteral("评价"),
      QStringLiteral("<b>%1</b>（%2级）")
          .arg(PetDetailRenderer::text(talent.levelName)).arg(talent.level));
  rows += PetDetailRenderer::row(
      QStringLiteral("单星能 / 其它"),
      talent.normalLines.isEmpty()
          ? QStringLiteral("—")
          : PetDetailRenderer::text(talent.normalLines.join(QStringLiteral("　"))));
  rows += PetDetailRenderer::row(
      QStringLiteral("双星能"),
      talent.doubleEnergyLines.isEmpty()
          ? QStringLiteral("—")
          : QStringLiteral("<b>%1</b>").arg(
                PetDetailRenderer::text(talent.doubleEnergyLines.join(QStringLiteral("　")))));
  rows += PetDetailRenderer::row(
      QStringLiteral("天赋战斗力 / 满天赋战斗力"),
      QStringLiteral("<b>%1</b> / %2").arg(talent.currentPower, talent.fullPower));
  return PetDetailRenderer::section(QStringLiteral("天赋"), rows);
}

QString relationshipHtml(const PetRelationshipState& relationships) {
  auto relatedText = [](const PetRelatedDisplay& related) {
    return related.facts.isEmpty()
               ? QStringLiteral("<b>%1</b>").arg(PetDetailRenderer::text(related.name))
               : QStringLiteral("<b>%1</b>（%2）")
                     .arg(PetDetailRenderer::text(related.name),
                          PetDetailRenderer::text(
                              related.facts.join(QStringLiteral("，"))));
  };
  auto rowsHtml = [&](const QList<PetRelationRow>& rows) {
    QString result;
    for (const PetRelationRow& row : rows) {
      QStringList values;
      for (const PetRelatedDisplay& related : row.pets)
        values.append(relatedText(related));
      if (!values.isEmpty())
        result += PetDetailRenderer::row(row.label, values.join(QStringLiteral("<br>")));
    }
    return result;
  };
  const QString summonRows = rowsHtml(relationships.summonRows);
  const QString carryRows = rowsHtml(relationships.carryRows);
  QString result;
  if (!summonRows.isEmpty())
    result += PetDetailRenderer::section(QStringLiteral("召唤关系"), summonRows);
  if (!carryRows.isEmpty())
    result += PetDetailRenderer::section(QStringLiteral("携带 / 神使关系"), carryRows);
  return result;
}

QString badgeHtml(const QList<PetBadgeSlot>& badges) {
  QStringList badgeRows;
  int slotNumber = 1;
  for (const PetBadgeSlot& badge : badges) {
    QString line = QStringLiteral("<b>%1</b> · %2级")
                       .arg(PetDetailRenderer::text(badge.jobName)).arg(badge.level);
    if (!badge.exclusiveName.isEmpty()) {
      line += QStringLiteral("<br><span class='%1'>%2 · %3</span>")
                  .arg(badge.exclusiveAwakened ? QStringLiteral("ok")
                                               : QStringLiteral("muted"),
                       PetDetailRenderer::text(badge.exclusiveName),
                       badge.exclusiveAwakened ? QStringLiteral("已觉醒")
                                               : QStringLiteral("未觉醒"));
    }
    badgeRows.append(PetDetailRenderer::row(
        QStringLiteral("元魂 %1").arg(slotNumber++), line));
  }
  if (badgeRows.isEmpty())
    badgeRows.append(PetDetailRenderer::row(QStringLiteral("状态"),
                                            QStringLiteral("未装备元魂")));
  return PetDetailRenderer::section(QStringLiteral("元魂"), badgeRows.join(QString()));
}

QString sacredHtml(const PetSacredState& sacred) {
  if (!sacred.equipped)
    return PetDetailRenderer::section(
        QStringLiteral("神源兽"),
        PetDetailRenderer::row(QStringLiteral("状态"), QStringLiteral("未装备神源兽")));
  const QString starState = sacred.maxStar > 0
                                ? QStringLiteral("%1/%2 星（%3）")
                                      .arg(sacred.star).arg(sacred.maxStar)
                                      .arg(sacred.fullStar ? QStringLiteral("满星")
                                                           : QStringLiteral("未满星"))
                                : QStringLiteral("%1 星").arg(sacred.star);
  QString stageState = sacred.maxStage > 0
                           ? QStringLiteral("%1/%2 阶（%3）")
                                 .arg(sacred.stage).arg(sacred.maxStage)
                                 .arg(sacred.fullStage ? QStringLiteral("满阶")
                                                       : QStringLiteral("未满阶"))
                           : QStringLiteral("%1 阶").arg(sacred.stage);
  if (!sacred.fullStage && sacred.maxStage > sacred.stage)
    stageState += QStringLiteral("，还差 %1 阶").arg(sacred.maxStage - sacred.stage);
  QString rows = PetDetailRenderer::row(
      QStringLiteral("名称"),
      QStringLiteral("<b>%1</b>").arg(PetDetailRenderer::text(sacred.name)));
  rows += PetDetailRenderer::row(QStringLiteral("星级"),
                                 PetDetailRenderer::text(starState));
  rows += PetDetailRenderer::row(QStringLiteral("阶级"),
                                 PetDetailRenderer::text(stageState));
  return PetDetailRenderer::section(QStringLiteral("神源兽"), rows);
}

QString astrolabeHtml(const PetAstrolabeState& astrolabe) {
  QStringList allStars;
  for (const PetAstrolabeStar& star : astrolabe.stars) {
    QString name = PetDetailRenderer::text(star.name);
    if (!star.activated && !star.lightUpCosts.isEmpty())
      name += QStringLiteral(" <span class='muted'>（点亮需 %1）</span>")
                  .arg(PetDetailRenderer::text(
                      star.lightUpCosts.join(QStringLiteral("、"))));
    if (!star.activated) name += QStringLiteral("（未点亮）");
    if (star.selected) {
      allStars.append(
          QStringLiteral("<span style='color:#159947;font-weight:700'>%1</span>")
              .arg(name));
    } else if (star.activated) {
      allStars.append(QStringLiteral("<span>%1</span>").arg(name));
    } else {
      allStars.append(QStringLiteral("<span class='muted'>%1</span>").arg(name));
    }
  }
  QString rows = PetDetailRenderer::row(
      QStringLiteral("全部星灵"),
      allStars.isEmpty() ? QStringLiteral("没有星轮数据")
                         : allStars.join(QStringLiteral("　")));
  rows += PetDetailRenderer::row(
      QStringLiteral("突破"),
      astrolabe.breakthrough ? QStringLiteral("<span class='ok'>已突破</span>")
                             : QStringLiteral("<span class='muted'>未突破</span>"));
  return PetDetailRenderer::section(QStringLiteral("天迹星轮"), rows);
}

QString stargodHtml(const QList<PetStargodEntry>& stargods,
                    const QList<PetStargodBackpackEntry>& backpack,
                    const PetBattlePowerState& power) {
  QList<PetStargodEntry> ordinary;
  QList<PetStargodEntry> changeable;
  for (const PetStargodEntry& entry : stargods)
    (entry.changeable ? changeable : ordinary).append(entry);
  auto renderEntry = [](const PetStargodEntry& entry, bool showBase) {
    const QString color = entry.quality == 6 ? QStringLiteral("#d63b3b")
                          : entry.quality == 5 ? QStringLiteral("#c58b00")
                                               : QStringLiteral("#4b5563");
    if (showBase)
      return QStringLiteral(
                 "<span class='star' style='color:%1'><u>%2</u> · 当前变为 %3 · Lv.%4</span>")
          .arg(color, PetDetailRenderer::text(entry.sourceName),
               PetDetailRenderer::text(entry.name))
          .arg(entry.level);
    return QStringLiteral("<span class='star' style='color:%1'>%2 · Lv.%3</span>")
        .arg(color, PetDetailRenderer::text(entry.name))
        .arg(entry.level);
  };
  QString rows = PetDetailRenderer::row(
      QStringLiteral("固定万变"),
      changeable.isEmpty()
          ? QStringLiteral("<span class='muted'><u>固定万变栏位</u>：未装备</span>")
          : [&]() {
              QStringList values;
              for (const PetStargodEntry& entry : changeable)
                values.append(renderEntry(entry, true));
              return values.join(QStringLiteral("<br>"));
            }());
  QHash<QString, QStringList> grouped;
  for (const PetStargodEntry& entry : ordinary)
    grouped[entry.category].append(renderEntry(entry, false));
  for (const QString& group : {QStringLiteral("进攻"), QStringLiteral("防御"),
                               QStringLiteral("功能性")}) {
    rows += PetDetailRenderer::row(
        group, grouped.value(group).isEmpty()
                   ? QStringLiteral("—")
                   : grouped.value(group).join(QStringLiteral("<br>")));
  }

  QStringList backpackValues;
  for (const PetStargodBackpackEntry& entry : backpack) {
    const QString color = entry.quality == 6 ? QStringLiteral("#d63b3b")
                          : entry.quality == 5 ? QStringLiteral("#c58b00")
                                               : QStringLiteral("#4b5563");
    backpackValues.append(
        QStringLiteral("<span class='star' style='color:%1'>%2%3</span>")
            .arg(color, PetDetailRenderer::text(entry.name),
                 entry.changeable ? QStringLiteral("（万变，不计入）")
                                  : QString()));
  }
  rows += PetDetailRenderer::row(
      QStringLiteral("精灵星神背包"),
      !power.stargodBackpackKnown
          ? QStringLiteral("<span class='muted'>详情未返回背包数据</span>")
          : backpackValues.isEmpty()
                ? QStringLiteral("<span class='muted'>背包为空</span>")
                : backpackValues.join(QStringLiteral("　")));

  rows += PetDetailRenderer::row(
      QStringLiteral("普通栏位"),
      !power.stargodSlotsKnown
          ? QStringLiteral("<span class='muted'>无法从详情确定栏位数量</span>")
          : QStringLiteral("已装备 %1 / %2；背包可用 %3；合计 %4 / %2（%5）")
                .arg(power.equippedStars)
                .arg(power.stargodSlots)
                .arg(power.backpackStars)
                .arg(power.availableStars)
                .arg(power.stargodFull ? QStringLiteral("数量已满足满战力")
                                       : QStringLiteral("还缺 %1 个").arg(
                                             power.missingStars)));
  return PetDetailRenderer::section(QStringLiteral("星神"), rows);
}

}  // namespace

QString PetDetailRenderer::render(const PetDetailViewModel& model,
                                  const PetDetailRenderOptions& options) {
  if (!model.available)
    return QStringLiteral("<p style='color:#6b7280'>该实例没有可用的本地详情。</p>");

  const QString imageHtml = model.imagePath.isEmpty()
                                ? QStringLiteral("<div class='image-placeholder'>图片首次显示后<br>自动缓存到本地</div>")
                                : QStringLiteral("<img class='pet-picture' src='%1' width='150'>")
                                      .arg(QUrl::fromLocalFile(model.imagePath)
                                               .toString(QUrl::FullyEncoded));
  QString identityRows = row(QStringLiteral("名称"),
                             QStringLiteral("<b>%1</b>").arg(valueOrDash(model.name)));
  identityRows += row(QStringLiteral("原名"), valueOrDash(model.originalName));
  if (!model.customName.isEmpty())
    identityRows += row(QStringLiteral("自定义昵称"), text(model.customName));
  identityRows += row(QStringLiteral("属性"), text(model.attributes));
  identityRows += row(QStringLiteral("职业"), text(model.jobs));
  identityRows += row(QStringLiteral("时代"), text(model.era));
  const PetBattlePowerState& power = model.battlePower;
  identityRows += row(
      QStringLiteral("战斗力"),
      power.hasCurrent
          ? QStringLiteral("<b>%1</b>（%2；%3）")
                .arg(power.current)
                .arg(power.isHighest ? QStringLiteral("已达最高战斗力")
                                     : QStringLiteral("未达最高战斗力"))
                .arg(power.currentLocallyCalculated
                         ? QStringLiteral("星神战力已本地重算")
                         : QStringLiteral("服务器战力"))
          : QStringLiteral("—"));
  identityRows += row(
      QStringLiteral("极限战斗力"),
      power.hasExtreme
          ? QStringLiteral("<b>%1</b>（最高战斗力 %2）")
                .arg(power.extreme)
                .arg(power.hasHighest ? QString::number(power.highest)
                                      : QStringLiteral("待确认"))
          : QStringLiteral("—"));

  QString warning;
  if (model.fetchingLatest) {
    warning = QStringLiteral("<div class='warning'>已显示本地缓存，正在按实例 ID 获取最新详情……</div>");
  } else if (model.visualMismatch) {
    warning = options.visualMismatchRefreshPending
                  ? QStringLiteral("<div class='warning'>检测到形态或皮肤变化，当前显示缓存战力，正在刷新最新详情。</div>")
                  : QStringLiteral("<div class='warning'>检测到形态或皮肤变化，当前显示缓存战力。</div>");
  }
  const QString header =
      QStringLiteral("<div class='hero'><div class='pet-name'>%1</div>"
                     "<div class='sub'>实例 %2　种族 %3　等级 %4</div>%5</div>")
          .arg(valueOrDash(model.name))
          .arg(model.instanceId)
          .arg(model.raceId)
          .arg(model.level)
          .arg(warning);
  const QString body = identitySection(identityRows, imageHtml) + talentHtml(model.talent) +
                       relationshipHtml(model.relationships) + badgeHtml(model.badges) +
                       sacredHtml(model.sacred) + astrolabeHtml(model.astrolabe) +
                       stargodHtml(model.stargods, model.stargodBackpack,
                                   model.battlePower) +
                       battlePowerAnalysisHtml(model);
  return document(header, body);
}

QString PetDetailRenderer::text(const QString& value) {
  return value.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
}

QString PetDetailRenderer::valueOrDash(const QString& value) {
  return value.isEmpty() ? QStringLiteral("—") : text(value);
}

QString PetDetailRenderer::row(const QString& label, const QString& richValue) {
  return QStringLiteral("<tr><td class='label'>%1</td><td class='value'>%2</td></tr>")
      .arg(text(label), richValue);
}

QString PetDetailRenderer::section(const QString& title, const QString& rows) {
  return QStringLiteral("<div class='section'><div class='section-title'>%1</div>"
                        "<table cellspacing='0' cellpadding='0'>%2</table></div>")
      .arg(text(title), rows);
}

QString PetDetailRenderer::identitySection(const QString& rows,
                                           const QString& imageHtml) {
  return QStringLiteral(
             "<div class='section'><div class='section-title'>基础信息</div>"
             "<table class='identity-layout' cellspacing='0' cellpadding='0'><tr>"
             "<td class='identity-values'><table cellspacing='0' cellpadding='0'>%1</table></td>"
             "<td class='identity-picture'>%2</td></tr></table></div>")
      .arg(rows, imageHtml);
}

QString PetDetailRenderer::document(const QString& header, const QString& body) {
  return QStringLiteral(
             "<html><head><style>"
             "body{font-family:'Microsoft YaHei UI';color:#263238;background:#f5f7fa;}"
             ".hero{background:#263b5a;color:white;padding:14px 16px;margin-bottom:10px;}"
             ".pet-name{font-size:22px;font-weight:700;} .sub{font-size:11px;color:#d9e2ef;margin-top:4px;}"
             ".section{background:white;border:1px solid #dfe5ec;margin:8px 2px;padding:8px 11px;}"
             ".section-title{font-size:15px;font-weight:700;color:#284b73;margin-bottom:5px;}"
             "table{width:100%;} td{padding:4px 2px;vertical-align:top;}"
             ".identity-layout{width:100%;table-layout:fixed;}"
             ".identity-values{width:auto;} .identity-picture{width:164px;text-align:center;}"
             ".pet-picture{max-width:150px;max-height:180px;}"
             ".image-placeholder{width:142px;height:92px;padding-top:48px;background:#eef2f7;"
             "color:#8a94a3;text-align:center;border:1px dashed #c8d1dc;}"
             ".label{width:76px;color:#718096;} .value{color:#1f2937;}"
             ".ok{color:#16824b;font-weight:600;} .muted{color:#8a94a3;}"
             ".warning{color:#b45309;font-weight:600;margin-top:5px;}"
             ".highest{color:#16824b;font-size:15px;font-weight:700;margin-top:5px;}"
             ".analysis{margin:2px 0 4px 18px;padding:0;} .analysis li{margin:3px 0;}"
             ".star{font-size:14px;font-weight:700;line-height:1.6;} .tiny{font-size:10px;font-weight:400;}"
             "u{text-decoration:underline;text-decoration-style:solid;}"
             "</style></head><body>%1%2</body></html>")
      .arg(header, body);
}
