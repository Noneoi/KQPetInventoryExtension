#include "pet_detail_renderer.h"

#include <QHash>
#include <QUrl>

namespace {

QString battlePowerAnalysisHtml(const PetBattlePowerState& state) {
  if (!state.hasCurrent || !state.hasExtreme) {
    return PetDetailRenderer::section(
        QStringLiteral("战斗力分析"),
        PetDetailRenderer::row(
            QStringLiteral("状态"),
            QStringLiteral("当前数据没有战斗力分项；仓库精灵请点击该行或使用“刷新仓库详情”。")));
  }

  QStringList missing;
  for (const PetBattlePowerGap& component : state.componentGaps) {
    missing.append(QStringLiteral("<li><b>%1</b>：当前 %2 / 极限 %3，差 <b>%4</b> 战斗力</li>")
                       .arg(PetDetailRenderer::text(component.label))
                       .arg(component.current)
                       .arg(component.extreme)
                       .arg(component.gap));
  }
  const QString extremeAnalysis =
      missing.isEmpty()
          ? QStringLiteral("<span class='ok'>普通养成项目已达到极限配置。</span>")
          : QStringLiteral("<ul class='analysis'>%1</ul><b>已识别的普通极限缺口：%2</b>")
                .arg(missing.join(QString()))
                .arg(state.knownExtremeGap);

  const int missingRedEquivalent = (state.missingRedBonus + 149) / 150;
  QString highestAnalysis;
  if (state.missingRedBonus == 0) {
    highestAnalysis += QStringLiteral(
        "<div class='ok'>星神：服务器实际加成 +1200，已达到8红星最高加成。</div>");
  } else {
    highestAnalysis += QStringLiteral(
                           "<div>星神：界面识别红星 %1/8、金星 %2/8；服务器实际额外战力 "
                           "+%3/+1200，还差 <b>%4</b>（约 %5 个红星加成）。"
                           "若颜色已全红，请检查星神等级、万变底座和星神组合。</div>")
                           .arg(state.redStars)
                           .arg(state.goldStars)
                           .arg(state.starBonus)
                           .arg(state.missingRedBonus)
                           .arg(missingRedEquivalent);
  }
  if (state.astrolabeBonus >= 150) {
    highestAnalysis += QStringLiteral(
        "<div class='ok'>天迹星轮：服务器实际加成 +150，突破加成已满。</div>");
  } else {
    highestAnalysis += QStringLiteral(
                           "<div>天迹星轮：%1；服务器实际额外战力 +%2/+150，还差 <b>%3</b>。</div>")
                           .arg(state.breakthrough ? QStringLiteral("显示已突破")
                                                   : QStringLiteral("未突破"))
                           .arg(state.astrolabeBonus)
                           .arg(150 - state.astrolabeBonus);
  }
  highestAnalysis += state.isHighest
                         ? QStringLiteral("<div class='highest'>已达到最高战斗力。</div>")
                         : QStringLiteral("<div class='warning'>当前距离最高战斗力还差 <b>%1</b>。</div>")
                               .arg(state.highestGap);

  QString rows = PetDetailRenderer::row(
      QStringLiteral("战力基准"),
      QStringLiteral("当前 <b>%1</b>　普通极限 <b>%2</b>　最高 <b>%3</b>")
          .arg(state.current)
          .arg(state.extreme)
          .arg(state.highest));
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
    if (!star.lightUpCosts.isEmpty())
      name += QStringLiteral(" <span class='muted'>（点亮需 %1）</span>")
                  .arg(PetDetailRenderer::text(
                      star.lightUpCosts.join(QStringLiteral("、"))));
    allStars.append(star.selected
                        ? QStringLiteral("<span style='color:#159947;font-weight:700'>%1（已选）</span>")
                              .arg(name)
                        : QStringLiteral("<span>%1</span>").arg(name));
  }
  QString rows = PetDetailRenderer::row(
      QStringLiteral("全部星灵"),
      allStars.isEmpty() ? QStringLiteral("没有星轮数据")
                         : allStars.join(QStringLiteral("　")));
  rows += PetDetailRenderer::row(QStringLiteral("已选数量"),
                                 QString::number(astrolabe.selectedCount));
  rows += PetDetailRenderer::row(
      QStringLiteral("突破"),
      astrolabe.breakthrough ? QStringLiteral("<span class='ok'>已突破</span>")
                             : QStringLiteral("<span class='muted'>未突破</span>"));
  return PetDetailRenderer::section(QStringLiteral("天迹星轮"), rows);
}

QString stargodHtml(const QList<PetStargodEntry>& stargods) {
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
  const int ordinaryCount = static_cast<int>(ordinary.size());
  const int missing = qMax(0, 7 - ordinaryCount);
  rows += PetDetailRenderer::row(
      QStringLiteral("普通栏位"),
      QStringLiteral("已装备 %1 / 7%2")
          .arg(qMin(ordinaryCount, 7))
          .arg(missing > 0 ? QStringLiteral("，还缺 %1 个").arg(missing)
                           : QStringLiteral("，已装满")));
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
          ? QStringLiteral("<b>%1</b>（%2）")
                .arg(power.current)
                .arg(power.isHighest ? QStringLiteral("已达最高战斗力")
                                     : QStringLiteral("未达最高战斗力"))
          : QStringLiteral("—"));
  identityRows += row(
      QStringLiteral("极限战斗力"),
      power.hasExtreme
          ? QStringLiteral("<b>%1</b>（最高战斗力 %2）")
                .arg(power.extreme).arg(power.highest)
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
                       stargodHtml(model.stargods) +
                       battlePowerAnalysisHtml(model.battlePower);
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
