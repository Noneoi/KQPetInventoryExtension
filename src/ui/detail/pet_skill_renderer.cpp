#include "pet_skill_renderer.h"
#include "public_data_detail_dialog.h"
#include "html_document.h"
#include "domain/public_skill_view.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QQueue>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTextDocument>

namespace {
using kqpet::html::escaped;

QString document(const QString& body) {
  return kqpet::html::document(QStringLiteral(
      "body{margin:16px;font-size:13px;line-height:1.55;}"
      "h2{font-size:22px;color:#183b5b;margin:0 0 5px;}"
      "h3{font-size:15px;margin:22px 0 9px;padding:0 0 6px;color:#173f63;border-bottom:2px solid #dbe9f5;}"
      "h4{font-size:13px;margin:12px 0 5px;color:#2f5f88;}"
      "table{border-collapse:collapse;}td,th{vertical-align:top;}"
      ".hero{background:#edf5fc;border:1px solid #d4e4f2;}"
      ".hero td{padding:13px 15px;}"
      ".hero-meta{color:#567086;font-size:12px;margin:2px 0 9px;}"
      ".summary{color:#263d52;margin:8px 0;}"
      ".reading-hint{margin:10px 0;padding:7px 10px;background:#f6f9fc;color:#52687b;border-left:3px solid #8bb7dd;}"
      ".slot-table{border:1px solid #dce6ef;background:#fff;}"
      ".slot-table td{padding:8px 10px;border-bottom:1px solid #e8eef4;}"
      ".slot-table .slot-name{width:32%;color:#315f86;background:#f5f9fc;font-weight:600;}"
      ".skill-card{margin:0 0 12px;border:1px solid #d8e4ee;background:#fff;}"
      ".skill-head{padding:10px 12px;background:#edf5fb;border-bottom:1px solid #d8e4ee;color:#173f63;}"
      ".skill-type{width:33%;padding:10px 12px;background:#3976aa;color:#fff;border-bottom:1px solid #2f6797;}"
      ".skill-body{padding:10px 12px;color:#263746;}"
      ".meta-strip{margin:0 0 7px;color:#718294;font-size:11px;}"
      ".variant{margin:9px 0 0;padding:7px 9px;background:#f7f9fb;color:#42576a;}"
      ".tag{display:inline-block;color:#315f86;background:#e8f1f9;padding:2px 6px;margin:0 5px 4px 0;}"
      ".notice{background:#fff6df;color:#8a5d18;padding:9px 11px;}"
      ".term-ref{color:#155b91;background:#e7f2fb;text-decoration:none;font-weight:700;}"
      ".id{color:#7b8794;font-size:11px;font-weight:400;}"
      ".secondary{padding:9px 11px;background:#f8fafc;border-left:3px solid #9bbbd5;}"
      "ul{margin:5px 0 8px 20px;padding:0;}li{margin:4px 0;}"), body);
}

QString animationText(const QJsonObject& skill) {
  const auto animation = skill.value(QStringLiteral("animation")).toObject();
  if (animation.isEmpty()) return {};
  QStringList values;
  if (animation.contains(QStringLiteral("action"))) values << QStringLiteral("动作帧 %1").arg(animation.value(QStringLiteral("action")).toInt());
  if (animation.contains(QStringLiteral("effect"))) values << QStringLiteral("特效帧 %1").arg(animation.value(QStringLiteral("effect")).toInt());
  if (animation.value(QStringLiteral("shock")).toInt()) values << QStringLiteral("震屏");
  if (animation.value(QStringLiteral("black")).toInt()) values << QStringLiteral("黑屏演出");
  if (animation.value(QStringLiteral("ef")).toInt()) values << QStringLiteral("全屏特效");
  return values.join(QStringLiteral(" · "));
}

QString plainSkillText(QString text) {
  text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  text.replace(QChar('\r'), QChar('\n'));
  // The official cache mixes HTML line breaks/tags with ordinary newlines.
  // QTextDocument decodes entities and strips markup in one well-tested pass.
  text.replace(QChar('\n'), QStringLiteral("<br>"));
  QTextDocument document;
  document.setHtml(text);
  QString result = document.toPlainText();
  result.replace(QChar::ParagraphSeparator, QChar('\n'));
  result.replace(QChar::LineSeparator, QChar('\n'));
  result.replace(QChar::Nbsp, QChar(' '));
  return result.trimmed();
}

QString contextualExplanation(const QString& display, const QString& source) {
  const QString context = plainSkillText(source);
  if (context.isEmpty()) return {};
  int position = context.indexOf(QStringLiteral("[%1]").arg(display));
  if (position < 0) return {};
  const QString boundaries = QStringLiteral("\n；。！？");
  int begin = position;
  while (begin > 0 && !boundaries.contains(context.at(begin - 1))) --begin;
  int end = position + display.size();
  while (end < context.size() && !boundaries.contains(context.at(end))) ++end;
  if (end < context.size()) ++end;
  QString sentence = context.mid(begin, end - begin).trimmed();
  if (sentence.size() > 360) sentence = sentence.left(359) + QChar(0x2026);
  return QStringLiteral("当前原文：%1").arg(sentence);
}

QString termTooltip(const QString& initial, const PublicSkillView& view, const QString& context) {
  QQueue<QPair<QString, QString>> queue;
  queue.enqueue({initial, context});
  QSet<QString> seen;
  QStringList sections;
  static const QRegularExpression expression(QStringLiteral("\\[([^\\]]+)\\]"));
  while (!queue.isEmpty() && sections.size() < 16) {
    const auto pending = queue.dequeue();
    const QString display = pending.first;
    if (seen.contains(display)) continue;
    seen.insert(display);
    const auto term = view.term(display);
    const QString description = plainSkillText(term.description);
    if (description.isEmpty()) {
      const QString original = contextualExplanation(display, pending.second);
      sections.append(QStringLiteral("[%1]\n暂无官方说明。%2").arg(display,
          original.isEmpty() ? QString() : QStringLiteral("\n") + original));
      continue;
    }
    sections.append(QStringLiteral("[%1]\n%2").arg(display, description));
    auto nested = expression.globalMatch(description);
    while (nested.hasNext()) queue.enqueue({nested.next().captured(1), description});
  }
  return sections.join(QStringLiteral("\n\n"));
}

QString mechanismLines(const QString& text, const PublicSkillView& view) {
  const QString source = plainSkillText(text);
  static const QRegularExpression expression(QStringLiteral("\\[([^\\]]+)\\]"));
  QString html;
  qsizetype cursor = 0;
  int serial = 0;
  auto matches = expression.globalMatch(source);
  while (matches.hasNext()) {
    const auto match = matches.next();
    html += escaped(source.mid(cursor, match.capturedStart() - cursor));
    const QString display = match.captured(1);
    QString tooltip = termTooltip(display, view, source);
    tooltip.remove(QChar('\r'));
    QString tooltipAttribute = escaped(tooltip);
    // Escape first, then introduce the line-feed entity. Escaping afterwards
    // would turn it into literal text such as "&#10;" in the popup.
    tooltipAttribute.replace(QChar('\n'), QStringLiteral("&#10;"));
    const auto term=view.term(display);
    const auto href=term.hasOfficialEntry() ? PublicDataDetailLinks::url({PublicDataViewKind::PetSkills,QStringLiteral("entries"),display}).toString(QUrl::FullyEncoded)
        : QStringLiteral("kqterm:%1").arg(serial++);
    html += QStringLiteral("<a class='term-ref' href='%1' title=\"%2\"><b>%3</b></a>")
        .arg(escaped(href),tooltipAttribute,escaped(match.captured(0)));
    cursor = match.capturedEnd();
  }
  html += escaped(source.mid(cursor));
  html.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
  return html;
}

QString skillCard(const QString& label, qint64 id, const QJsonObject& skills, const QJsonObject& combos,
                  const QJsonObject& buffs, const PublicSkillView& view) {
  const auto skill = skills.value(QString::number(id)).toObject();
  if (skill.isEmpty()) return QStringLiteral("<table class='skill-card' width='100%' cellspacing='0'><tr>"
      "<td class='skill-type' width='33%'><b>%1</b></td><td class='skill-head' width='67%'><b>未知技能</b></td></tr>"
      "<tr><td class='skill-body muted' colspan='2'>暂无本地说明</td></tr></table>")
      .arg(escaped(label));
  const QString description = skill.value(QStringLiteral("description")).toString();
  QString html = QStringLiteral("<table class='skill-card' width='100%' cellspacing='0'><tr>"
      "<td class='skill-type' width='33%'><b>%1</b></td><td class='skill-head' width='67%'><b>%2</b></td></tr>"
      "<tr><td class='skill-body' colspan='2'>")
      .arg(escaped(label), PublicDataDetailLinks::anchor({PublicDataViewKind::PetSkills,QStringLiteral("skills"),QString::number(id)},skill.value(QStringLiteral("name")).toString()));
  const QString animation = animationText(skill);
  if (!animation.isEmpty()) html += QStringLiteral("<p class='meta-strip'>演出信息 · %1</p>").arg(escaped(animation));
  html += description.isEmpty() ? QStringLiteral("<p class='muted'>官方客户端未提供文字说明。</p>")
                                : QStringLiteral("<p>%1</p>").arg(mechanismLines(description, view));
  const auto variants = combos.value(QString::number(id)).toArray();
  if (!variants.isEmpty()) {
    html += QStringLiteral("<div class='variant'><b>形态切换</b><ul>");
    for (const auto& value : variants) {
      const auto variant = value.toObject();
      const int buff = variant.value(QStringLiteral("buff")).toInt();
      const int target = variant.value(QStringLiteral("skill")).toInt();
      const QString buffName = buffs.value(QString::number(buff)).toObject().value(QStringLiteral("name")).toString();
      const QString targetName = skills.value(QString::number(target)).toObject().value(QStringLiteral("name")).toString();
      html += QStringLiteral("<li>%1时，切换为「%2」</li>")
          .arg(buffName.isEmpty() ? QStringLiteral("满足对应状态")
                                  : QStringLiteral("处于「%1」状态").arg(PublicDataDetailLinks::anchor({PublicDataViewKind::PetSkills,QStringLiteral("buffs"),QString::number(buff)},buffName)),
               PublicDataDetailLinks::anchor({PublicDataViewKind::PetSkills,QStringLiteral("skills"),QString::number(target)},targetName.isEmpty() ? QStringLiteral("对应技能形态") : targetName));
    }
    html += QStringLiteral("</ul></div>");
  }
  return html + QStringLiteral("</td></tr></table>");
}

QString ruleSection(const QJsonObject& root, const QJsonObject& pet, const PublicSkillView& view) {
  const int type = pet.value(QStringLiteral("qiYunType")).toInt();
  if (type <= 0) return {};
  if (PublicSkillView::hasTag(pet, QStringLiteral("灵初")) ||
      !PublicSkillView::hasTag(pet, QStringLiteral("神运"))) return {};
  const auto rule = root.value(QStringLiteral("shenyunRules")).toObject()
      .value(QString::number(type)).toObject();
  if (rule.isEmpty()) return {};
  const QList<QPair<QString, QString>> fields{{QStringLiteral("bj"),QStringLiteral("暴击率")},
      {QStringLiteral("fb"),QStringLiteral("防暴率")},{QStringLiteral("mz"),QStringLiteral("命中率")},
      {QStringLiteral("sb"),QStringLiteral("闪避率")},{QStringLiteral("pj"),QStringLiteral("破击率")},
      {QStringLiteral("gd"),QStringLiteral("格挡率")},{QStringLiteral("qh"),QStringLiteral("强化之力")},
      {QStringLiteral("dk"),QStringLiteral("抵抗之力")}};
  QString html = QStringLiteral("<h3>神运气运规则</h3><table class='slot-table' width='100%'><tr><td class='slot-name'>项目</td><td><b>阈值 / 档位</b></td><td><b>实质效果</b></td></tr>");
  for (const auto& field : fields) {
    const QString description = rule.value(field.first + QStringLiteral("E")).toString();
    if (description.isEmpty()) continue;
    html += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td><td>%3</td></tr>")
        .arg(escaped(field.second), escaped(rule.value(field.first + QStringLiteral("T")).toString()),
             mechanismLines(description, view));
  }
  return html + QStringLiteral("</table>");
}

QString relationSection(const QJsonObject& pet, const PublicSkillView& view) {
  const auto relations = pet.value(QStringLiteral("relations")).toArray();
  if (relations.isEmpty()) return {};
  QString html = QStringLiteral("<h3>羁绊团队</h3>");
  for (const auto& value : relations) {
    const auto relation = value.toObject();
    html += QStringLiteral("<h4>%1</h4><p class='muted'>拥有该精灵 +%2 点；战力达到 %3 再 +%4 点</p><ul>")
        .arg(escaped(relation.value(QStringLiteral("name")).toString()))
        .arg(relation.value(QStringLiteral("ownedPoints")).toInt())
        .arg(relation.value(QStringLiteral("power")).toInt())
        .arg(relation.value(QStringLiteral("powerPoints")).toInt());
    for (const auto& phaseValue : relation.value(QStringLiteral("phases")).toArray()) {
      const auto phase = phaseValue.toObject();
      html += QStringLiteral("<li>%1（%2 点）%3</li>")
          .arg(escaped(phase.value(QStringLiteral("name")).toString()))
          .arg(phase.value(QStringLiteral("points")).toInt())
          .arg(phase.value(QStringLiteral("effect")).toString().isEmpty() ? QString() :
               QStringLiteral("：") + mechanismLines(phase.value(QStringLiteral("effect")).toString(), view));
    }
    html += QStringLiteral("</ul>");
  }
  return html;
}

QString relatedSection(const QJsonObject& pet, const PublicSkillView& view) {
  QString html;
  const auto summons = pet.value(QStringLiteral("summons")).toArray();
  if (!summons.isEmpty()) {
    html += QStringLiteral("<h3>召唤关系</h3><ul>");
    for (const auto& value : summons) {
      const auto item = value.toObject();
      const QString role = item.value(QStringLiteral("role")).toString() == QStringLiteral("summoner")
          ? QStringLiteral("召唤者") : QStringLiteral("召唤物");
      html += QStringLiteral("<li><span class='tag'>%1</span>%2%3</li>").arg(role,
          mechanismLines(item.value(QStringLiteral("description")).toString(), view),
          item.value(QStringLiteral("effect")).toString().isEmpty() ? QString() :
              QStringLiteral("；") + mechanismLines(item.value(QStringLiteral("effect")).toString(), view));
    }
    html += QStringLiteral("</ul>");
  }
  const auto carries = pet.value(QStringLiteral("carry")).toArray();
  if (!carries.isEmpty()) {
    html += QStringLiteral("<h3>契约 / 携同关系</h3><ul>");
    for (const auto& value : carries) {
      const auto item = value.toObject();
      html += QStringLiteral("<li><span class='tag'>%1</span>%2</li>")
          .arg(item.value(QStringLiteral("role")).toString() == QStringLiteral("carrier")
                   ? QStringLiteral("携同者") : QStringLiteral("被携同"),
               mechanismLines(item.value(QStringLiteral("description")).toString(), view));
    }
    html += QStringLiteral("</ul>");
  }
  const auto huan = pet.value(QStringLiteral("huan")).toArray();
  if (!huan.isEmpty()) {
    html += QStringLiteral("<h3>寰技</h3><ul>");
    for (const auto& value : huan) html += QStringLiteral("<li>%1</li>").arg(mechanismLines(value.toString(), view));
    html += QStringLiteral("</ul>");
  }
  const auto almighty = pet.value(QStringLiteral("almighty")).toArray();
  if (!almighty.isEmpty()) {
    html += QStringLiteral("<h3>天觉者 / 全能技</h3>");
    for (const auto& value : almighty) {
      const auto item = value.toObject();
      html += QStringLiteral("<div class='secondary'><b>%1</b><p>%2</p><p><b>%3：</b>%4</p><p><b>%5：</b>%6</p></div>")
          .arg(escaped(item.value(QStringLiteral("heroName")).toString()),
               mechanismLines(item.value(QStringLiteral("heroDescription")).toString(), view),
               escaped(item.value(QStringLiteral("summonName")).toString()),
               mechanismLines(item.value(QStringLiteral("summonDescription")).toString(), view),
               escaped(item.value(QStringLiteral("psychicsName")).toString()),
               mechanismLines(item.value(QStringLiteral("psychicsDescription")).toString(), view));
    }
  }
  return html;
}

}

QString PetSkillRenderer::render(const std::shared_ptr<const PetSkillCatalogSnapshot>& catalog,
                                 int raceId, const QString& fallbackName, const QString& originalName) {
  if (!catalog || !catalog->loaded)
    return document(QStringLiteral("<h2>%1</h2><p class='notice'>本地技能资料尚未加载。可在“设置 → 数据更新 → 精灵技能资料”检查更新。</p>")
        .arg(escaped(fallbackName.isEmpty() ? QStringLiteral("技能资料") : fallbackName)));
  const auto root = catalog->root;
  const auto pet = root.value(QStringLiteral("pets")).toObject().value(QString::number(raceId)).toObject();
  if (pet.isEmpty())
    return document(QStringLiteral("<h2>%1</h2><p class='notice'>暂无对应技能资料；可检查技能数据更新。</p>")
        .arg(escaped(fallbackName.isEmpty() ? QStringLiteral("未知精灵") : fallbackName)));

  const auto skills = root.value(QStringLiteral("skills")).toObject();
  const auto buffs = root.value(QStringLiteral("buffs")).toObject();
  const auto combos = root.value(QStringLiteral("combos")).toObject();
  // Archived supplement/evaluation records remain in the bundled artifact;
  // ordinary UI explains only this immutable current official catalog.
  const PublicSkillView view(root);
  const auto slotsObject = pet.value(QStringLiteral("slots")).toObject();
  const QString formName = pet.value(QStringLiteral("name")).toString(fallbackName);
  const QString name = originalName.isEmpty() ? formName : originalName;
  QString html = QStringLiteral("<table class='hero' width='100%' cellspacing='0'><tr><td><h2>%1</h2>")
      .arg(PublicDataDetailLinks::anchor({PublicDataViewKind::PetSkills,QStringLiteral("pets"),QString::number(raceId)},name));
  if (formName!=name) html += QStringLiteral("<p class='muted'>皮肤 / 形态：%1</p>").arg(escaped(formName));
  QStringList meta;
  for (const auto* key : {"position", "quality"}) {
    const auto value = pet.value(QLatin1String(key)).toString();
    if (!value.isEmpty()) meta.append(escaped(value));
  }
  if (!meta.isEmpty()) html += QStringLiteral("<p class='hero-meta'>%1</p>").arg(meta.join(QStringLiteral(" · ")));
  if (!pet.value(QStringLiteral("officialRole")).toString().isEmpty())
    html += QStringLiteral("<p class='summary'><b>官方定位</b> · %1</p>")
        .arg(mechanismLines(pet.value(QStringLiteral("officialRole")).toString(), view));
  if (!pet.value(QStringLiteral("strategy")).toString().isEmpty())
    html += QStringLiteral("<p class='summary'><b>机制摘要</b> · %1</p>")
        .arg(mechanismLines(pet.value(QStringLiteral("strategy")).toString(), view));
  const auto traits = pet.value(QStringLiteral("traits")).toArray();
  if (!traits.isEmpty()) {
    html += QStringLiteral("<p class='summary'><b>特殊机制</b> · ");
    for (const auto& value : traits) html += QStringLiteral("<span class='tag'>%1</span>").arg(escaped(value.toString()));
    html += QStringLiteral("</p>");
  }
  html += QStringLiteral("</td></tr></table><p class='reading-hint'>点击技能名或词条可打开资料详情；鼠标移入蓝色词条可快速查看解释。</p>");

  QStringList slotRows;
  for (const auto& slot : PublicSkillView::slotDefinitions()) {
    const int id = slotsObject.value(slot.first).toInt();
    if (id <= 0) continue;
    const auto skill = skills.value(QString::number(id)).toObject();
    slotRows.append(QStringLiteral("<tr><td class='slot-name'>%1</td><td><b>%2</b></td></tr>")
        .arg(escaped(view.slotLabel(slot.first, pet)),
             PublicDataDetailLinks::anchor({PublicDataViewKind::PetSkills,QStringLiteral("skills"),QString::number(id)},skill.value(QStringLiteral("name")).toString(QStringLiteral("未命名")))));
  }
  const auto starGodSkill = pet.value(QStringLiteral("starGodSkill")).toObject();
  if (!starGodSkill.isEmpty())
    slotRows.append(QStringLiteral("<tr><td class='slot-name'>星神技</td><td><b>%1</b></td></tr>")
        .arg(PublicDataDetailLinks::anchor({PublicDataViewKind::PetSkills,QStringLiteral("petStarGodSkills"),QString::number(raceId)},starGodSkill.value(QStringLiteral("name")).toString())));
  if (!slotRows.isEmpty())
    html += QStringLiteral("<h3>技能槽总览 <span class='id'>· 仅显示已拥有的 %1 个槽位</span></h3>"
                           "<table class='slot-table' width='100%'>%2</table>")
        .arg(slotRows.size())
        .arg(slotRows.join(QString()));

  html += QStringLiteral("<h3>技能与实质效果 <span class='id'>· 按槽位顺序阅读</span></h3>");
  for (const auto& slot : PublicSkillView::slotDefinitions()) {
    const int id = slotsObject.value(slot.first).toInt();
    if (id > 0) html += skillCard(view.slotLabel(slot.first, pet), id, skills, combos, buffs, view);
  }
  if (!starGodSkill.isEmpty()) {
    const QString description = starGodSkill.value(QStringLiteral("description")).toString();
    html += QStringLiteral("<table class='skill-card' width='100%' cellspacing='0'><tr>"
                           "<td class='skill-type' width='33%'><b>星神技</b></td><td class='skill-head' width='67%'><b>%1</b></td></tr>"
                           "<tr><td class='skill-body' colspan='2'><p>%2</p></td></tr></table>")
        .arg(PublicDataDetailLinks::anchor({PublicDataViewKind::PetSkills,QStringLiteral("petStarGodSkills"),QString::number(raceId)},starGodSkill.value(QStringLiteral("name")).toString()), mechanismLines(description, view));
  }

  QString advancedHtml;
  const int transformId = slotsObject.value(QStringLiteral("transform")).toInt();
  const auto transform = root.value(QStringLiteral("transformSkills")).toObject()
      .value(QString::number(transformId)).toObject();
  if (!transform.isEmpty()) {
    advancedHtml += QStringLiteral("<h4>%1规则</h4><div class='secondary'><b>%2</b><p>%3%4</p></div>")
        .arg(escaped(view.slotLabel(QStringLiteral("transform"), pet)),
             PublicDataDetailLinks::anchor({PublicDataViewKind::PetSkills,QStringLiteral("transformSkills"),QString::number(transformId)},transform.value(QStringLiteral("name")).toString()),
             mechanismLines(transform.value(QStringLiteral("description")).toString(), view),
             transform.value(QStringLiteral("teamDescription")).toString().isEmpty() ? QString() :
                 QStringLiteral("；") + mechanismLines(transform.value(QStringLiteral("teamDescription")).toString(), view));
  }
  const int heroId = slotsObject.value(QStringLiteral("hero")).toInt();
  const auto heroRules = root.value(QStringLiteral("heroSkills")).toObject()
      .value(QString::number(heroId)).toArray();
  if (!heroRules.isEmpty()) {
    advancedHtml += QStringLiteral("<h4>英雄技生效配置</h4><table class='slot-table' width='100%'><tr><td class='slot-name'>目标条件</td><td><b>条件参数</b></td><td><b>数值</b></td></tr>");
    for (const auto& value : heroRules) {
      const auto rule = value.toObject();
      QStringList numbers;
      if (rule.contains(QStringLiteral("z1"))) numbers << QStringLiteral("固定值 %1").arg(rule.value(QStringLiteral("z1")).toVariant().toString());
      if (rule.contains(QStringLiteral("z2"))) numbers << QStringLiteral("比例 %1").arg(rule.value(QStringLiteral("z2")).toVariant().toString());
      advancedHtml += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td></tr>")
          .arg(escaped(rule.value(QStringLiteral("c")).toVariant().toString()),
               escaped(rule.value(QStringLiteral("cP")).toString()), escaped(numbers.join(QStringLiteral("；"))));
    }
    advancedHtml += QStringLiteral("</table>");
  }
  const int psychicsId = slotsObject.value(QStringLiteral("psychics")).toInt();
  const auto psychicRules = root.value(QStringLiteral("psychicsSkills")).toObject()
      .value(QString::number(psychicsId)).toArray();
  if (!psychicRules.isEmpty()) {
    advancedHtml += QStringLiteral("<h4>通灵配置</h4><div class='secondary'><ul>");
    for (const auto& value : psychicRules) {
      const auto rule = value.toObject();
      const auto summoned = root.value(QStringLiteral("pets")).toObject()
          .value(QString::number(rule.value(QStringLiteral("cpr")).toInt())).toObject();
      advancedHtml += QStringLiteral("<li>被通灵精灵：%1；条件类型：%2；条件参数：%3</li>")
          .arg(escaped(summoned.value(QStringLiteral("name")).toString(QStringLiteral("名称待补充"))),
               escaped(rule.value(QStringLiteral("c")).toVariant().toString()),
               escaped(rule.value(QStringLiteral("cP")).toString()));
    }
    advancedHtml += QStringLiteral("</ul></div>");
  }

  advancedHtml += ruleSection(root, pet, view);
  advancedHtml += relatedSection(pet, view);
  advancedHtml += relationSection(pet, view);

  const int evolutionFrom = pet.value(QStringLiteral("evolutionFrom")).toInt();
  if (evolutionFrom > 0) {
    const auto previous = root.value(QStringLiteral("pets")).toObject().value(QString::number(evolutionFrom)).toObject();
    advancedHtml += QStringLiteral("<h4>进化前技能</h4><p>%1</p><ul>")
        .arg(escaped(previous.value(QStringLiteral("name")).toString(QStringLiteral("未知形态"))));
    const auto previousSlots = previous.value(QStringLiteral("slots")).toObject();
    for (const auto& slot : PublicSkillView::slotDefinitions()) {
      const int id = previousSlots.value(slot.first).toInt();
      if (id <= 0) continue;
      const auto skill = skills.value(QString::number(id)).toObject();
      advancedHtml += QStringLiteral("<li>%1：<b>%2</b> — %3</li>")
          .arg(escaped(view.slotLabel(slot.first, previous)), escaped(skill.value(QStringLiteral("name")).toString()),
               mechanismLines(skill.value(QStringLiteral("description")).toString(), view));
    }
    advancedHtml += QStringLiteral("</ul>");
  }
  const auto skins = pet.value(QStringLiteral("skins")).toArray();
  if (!skins.isEmpty()) {
    advancedHtml += QStringLiteral("<h4>皮肤 / 异形态</h4><ul>");
    for (const auto& value : skins) {
      const auto skin = value.toObject();
      advancedHtml += QStringLiteral("<li>%1</li>").arg(escaped(skin.value(QStringLiteral("name")).toString()));
    }
    advancedHtml += QStringLiteral("</ul>");
  }
  if (!advancedHtml.isEmpty()) html += QStringLiteral("<h3>进阶规则与关联效果</h3>") + advancedHtml;
  html += QStringLiteral("<p class='muted'>资料版本：%1。伤害结算公式和未公开的服务器数值不在客户端缓存中。</p>")
      .arg(escaped(root.value(QStringLiteral("source")).toObject().value(QStringLiteral("version")).toString()));
  return document(html);
}
