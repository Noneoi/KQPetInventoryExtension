#include "pet_skill_renderer.h"
#include "html_document.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQueue>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTextDocument>

namespace {
using kqpet::html::escaped;

const QJsonObject& skillSupplement() {
  static const QJsonObject root = [] {
    QFile file(QStringLiteral(":/kqpet/pet-skill-supplement.json"));
    if (!file.open(QIODevice::ReadOnly)) return QJsonObject{};
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return QJsonObject{};
    const auto candidate = document.object();
    if (candidate.value(QStringLiteral("schema")).toInt() != 1 ||
        candidate.value(QStringLiteral("updatePolicy")).toString() != QStringLiteral("manual-static"))
      return QJsonObject{};
    return candidate;
  }();
  return root;
}

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
      ".assessment{padding:12px 14px;background:#edf5fc;border-left:4px solid #3976aa;color:#263d52;}"
      "ul{margin:5px 0 8px 20px;padding:0;}li{margin:4px 0;}"), body);
}

const QList<QPair<QString, QString>>& skillSlots() {
  static const QList<QPair<QString, QString>> values{
      {QStringLiteral("normal"), QStringLiteral("普通技")},
      {QStringLiteral("ultimate"), QStringLiteral("超杀技")},
      {QStringLiteral("super"), QStringLiteral("超必杀技")},
      {QStringLiteral("hero"), QStringLiteral("英雄技")},
      {QStringLiteral("large"), QStringLiteral("大招")},
      {QStringLiteral("dragonJob"), QStringLiteral("龙职技")},
      {QStringLiteral("yuanLi"), QStringLiteral("源力技")},
      {QStringLiteral("psychics"), QStringLiteral("通灵技")},
      {QStringLiteral("fate"), QStringLiteral("天命 / 传说 / 神运技能")},
      {QStringLiteral("transform"), QStringLiteral("元素 / 转化 / 赋能 / 幻元技")},
  };
  return values;
}

QString slotLabel(const QString& key, const QJsonObject& pet, const QJsonObject& skill) {
  if (key == QStringLiteral("fate")) {
    const QString signs = pet.value(QStringLiteral("signs")).toString();
    if (signs.contains(QStringLiteral("灵初"))) return QStringLiteral("灵初天命技");
    if (signs.contains(QStringLiteral("神运"))) return QStringLiteral("神运技能");
    if (signs.contains(QStringLiteral("天启"))) return QStringLiteral("天启 / 传说技");
    return QStringLiteral("天命 / 传说技");
  }
  if (key == QStringLiteral("transform")) {
    const QString name = skill.value(QStringLiteral("name")).toString();
    for (const auto& token : {QStringLiteral("元素"), QStringLiteral("转化"), QStringLiteral("转换"),
                              QStringLiteral("赋能"), QStringLiteral("幻元")})
      if (name.contains(token)) return token + QStringLiteral("技");
  }
  if (key == QStringLiteral("yuanLi")) {
    const QString signs = pet.value(QStringLiteral("signs")).toString();
    if (signs.contains(QStringLiteral("寰力量"))) return QStringLiteral("寰技");
    if (signs.contains(QStringLiteral("异形族"))) return QStringLiteral("异形技");
  }
  for (const auto& value : skillSlots()) if (value.first == key) return value.second;
  return key;
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
  if (context.isEmpty()) return QStringLiteral("当前技能将其作为触发条件或效果状态使用。");
  int position = context.indexOf(QStringLiteral("[%1]").arg(display));
  if (position < 0) position = context.indexOf(display);
  if (position < 0) return QStringLiteral("当前技能中的作用：%1").arg(context.left(360));
  const QString boundaries = QStringLiteral("\n；。！？");
  int begin = position;
  while (begin > 0 && !boundaries.contains(context.at(begin - 1))) --begin;
  int end = position + display.size();
  while (end < context.size() && !boundaries.contains(context.at(end))) ++end;
  if (end < context.size()) ++end;
  QString sentence = context.mid(begin, end - begin).trimmed();
  if (sentence.size() > 360) sentence = sentence.left(359) + QChar(0x2026);
  return QStringLiteral("当前技能中的作用：%1").arg(sentence);
}

QString termTooltip(const QString& initial, const QJsonObject& entries, const QString& context) {
  QQueue<QString> queue;
  queue.enqueue(initial);
  QSet<QString> seen;
  QStringList sections;
  static const QRegularExpression expression(QStringLiteral("\\[([^\\]]+)\\]"));
  while (!queue.isEmpty() && sections.size() < 16) {
    const QString display = queue.dequeue();
    QString key = display;
    QString argument;
    const int separator = display.indexOf(QChar(0x00b7));
    // Prefer a future exact official definition.  Only fall back to the
    // parameterized base entry when no exact key exists.
    if (!entries.contains(display) && separator > 0 && entries.contains(display.left(separator))) {
      key = display.left(separator);
      argument = display.mid(separator + 1);
    }
    if (!entries.contains(key)) {
      QString base = display;
      base.remove(QRegularExpression(QStringLiteral("[·\\-]?[ⅠⅡⅢⅣⅤⅥⅦⅧⅨⅩIVX]+$"),
                                     QRegularExpression::CaseInsensitiveOption));
      QString fuzzy;
      for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        if (!it.value().isString() || base.size() < 2) continue;
        if (it.key().endsWith(base) || base.endsWith(it.key())) {
          if (fuzzy.isEmpty() || it.key().size() < fuzzy.size()) fuzzy = it.key();
        }
      }
      if (!fuzzy.isEmpty()) key = fuzzy;
    }
    if (!entries.contains(key) || !entries.value(key).isString()) {
      const QString needle = QStringLiteral("[%1]").arg(display);
      QStringList related;
      for (auto it = entries.constBegin(); it != entries.constEnd() && related.size() < 4; ++it) {
        if (it.value().isString() && it.value().toString().contains(needle))
          related.append(QStringLiteral("由「%1」关联：%2")
                             .arg(it.key(), plainSkillText(it.value().toString())));
      }
      if (related.isEmpty()) related.append(contextualExplanation(display, context));
      sections.append(QStringLiteral("[%1]\n%2").arg(display, related.join(QStringLiteral("\n"))));
      continue;
    }
    if (seen.contains(key)) continue;
    seen.insert(key);
    QString description = plainSkillText(entries.value(key).toString());
    if (description.isEmpty()) {
      sections.append(QStringLiteral("[%1]\n%2").arg(display, contextualExplanation(display, context)));
      continue;
    }
    description.replace(QStringLiteral("ARG"), argument.isEmpty() ? QStringLiteral("X") : argument);
    sections.append(QStringLiteral("[%1]\n%2").arg(display, description));
    auto nested = expression.globalMatch(description);
    while (nested.hasNext()) queue.enqueue(nested.next().captured(1));
  }
  return sections.join(QStringLiteral("\n\n"));
}

QString mechanismLines(const QString& text, const QJsonObject& entries) {
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
    QString tooltip = termTooltip(display, entries, source);
    tooltip.remove(QChar('\r'));
    QString tooltipAttribute = escaped(tooltip);
    // Escape first, then introduce the line-feed entity. Escaping afterwards
    // would turn it into literal text such as "&#10;" in the popup.
    tooltipAttribute.replace(QChar('\n'), QStringLiteral("&#10;"));
    html += QStringLiteral("<a class='term-ref' href='kqterm:%1' title=\"%2\"><b>%3</b></a>")
        .arg(serial++).arg(tooltipAttribute, escaped(match.captured(0)));
    cursor = match.capturedEnd();
  }
  html += escaped(source.mid(cursor));
  html.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
  return html;
}

QString skillCard(const QString& label, qint64 id, const QJsonObject& skills, const QJsonObject& combos,
                  const QJsonObject& buffs, const QJsonObject& entries) {
  const auto skill = skills.value(QString::number(id)).toObject();
  if (skill.isEmpty()) return QStringLiteral("<table class='skill-card' width='100%' cellspacing='0'><tr>"
      "<td class='skill-type' width='33%'><b>%1</b></td><td class='skill-head' width='67%'><b>未知技能</b></td></tr>"
      "<tr><td class='skill-body muted' colspan='2'>技能 %2 暂无本地说明</td></tr></table>")
      .arg(escaped(label)).arg(id);
  const QString description = skill.value(QStringLiteral("description")).toString();
  QString html = QStringLiteral("<table class='skill-card' width='100%' cellspacing='0'><tr>"
      "<td class='skill-type' width='33%'><b>%1</b></td><td class='skill-head' width='67%'><b>%2</b><br><span class='id'>技能 ID %3</span></td></tr>"
      "<tr><td class='skill-body' colspan='2'>")
      .arg(escaped(label), escaped(skill.value(QStringLiteral("name")).toString())).arg(id);
  const QString animation = animationText(skill);
  if (!animation.isEmpty()) html += QStringLiteral("<p class='meta-strip'>演出信息 · %1</p>").arg(escaped(animation));
  html += description.isEmpty() ? QStringLiteral("<p class='muted'>官方客户端未提供文字说明。</p>")
                                : QStringLiteral("<p>%1</p>").arg(mechanismLines(description, entries));
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
                                  : QStringLiteral("处于「%1」状态").arg(escaped(buffName)),
               escaped(targetName.isEmpty() ? QStringLiteral("对应技能形态") : targetName));
    }
    html += QStringLiteral("</ul></div>");
  }
  return html + QStringLiteral("</td></tr></table>");
}

QString ruleSection(const QJsonObject& root, const QJsonObject& pet, const QJsonObject& entries) {
  const int type = pet.value(QStringLiteral("qiYunType")).toInt();
  if (type <= 0) return {};
  const bool lingchu = pet.value(QStringLiteral("signs")).toString().contains(QStringLiteral("灵初"));
  if (lingchu) return {};
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
             mechanismLines(description, entries));
  }
  return html + QStringLiteral("</table>");
}

QString relationSection(const QJsonObject& pet, const QJsonObject& entries) {
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
               QStringLiteral("：") + mechanismLines(phase.value(QStringLiteral("effect")).toString(), entries));
    }
    html += QStringLiteral("</ul>");
  }
  return html;
}

QString relatedSection(const QJsonObject& pet, const QJsonObject& entries) {
  QString html;
  const auto summons = pet.value(QStringLiteral("summons")).toArray();
  if (!summons.isEmpty()) {
    html += QStringLiteral("<h3>召唤关系</h3><ul>");
    for (const auto& value : summons) {
      const auto item = value.toObject();
      const QString role = item.value(QStringLiteral("role")).toString() == QStringLiteral("summoner")
          ? QStringLiteral("召唤者") : QStringLiteral("召唤物");
      html += QStringLiteral("<li><span class='tag'>%1</span>%2%3</li>").arg(role,
          mechanismLines(item.value(QStringLiteral("description")).toString(), entries),
          item.value(QStringLiteral("effect")).toString().isEmpty() ? QString() :
              QStringLiteral("；") + mechanismLines(item.value(QStringLiteral("effect")).toString(), entries));
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
               mechanismLines(item.value(QStringLiteral("description")).toString(), entries));
    }
    html += QStringLiteral("</ul>");
  }
  const auto huan = pet.value(QStringLiteral("huan")).toArray();
  if (!huan.isEmpty()) {
    html += QStringLiteral("<h3>寰技</h3><ul>");
    for (const auto& value : huan) html += QStringLiteral("<li>%1</li>").arg(mechanismLines(value.toString(), entries));
    html += QStringLiteral("</ul>");
  }
  const auto almighty = pet.value(QStringLiteral("almighty")).toArray();
  if (!almighty.isEmpty()) {
    html += QStringLiteral("<h3>天觉者 / 全能技</h3>");
    for (const auto& value : almighty) {
      const auto item = value.toObject();
      html += QStringLiteral("<div class='secondary'><b>%1</b><p>%2</p><p><b>%3：</b>%4</p><p><b>%5：</b>%6</p></div>")
          .arg(escaped(item.value(QStringLiteral("heroName")).toString()),
               mechanismLines(item.value(QStringLiteral("heroDescription")).toString(), entries),
               escaped(item.value(QStringLiteral("summonName")).toString()),
               mechanismLines(item.value(QStringLiteral("summonDescription")).toString(), entries),
               escaped(item.value(QStringLiteral("psychicsName")).toString()),
               mechanismLines(item.value(QStringLiteral("psychicsDescription")).toString(), entries));
    }
  }
  return html;
}

}

QString PetSkillRenderer::render(const std::shared_ptr<const PetSkillCatalogSnapshot>& catalog,
                                 int raceId, const QString& fallbackName) {
  if (!catalog || !catalog->loaded)
    return document(QStringLiteral("<h2>%1</h2><p class='notice'>本地技能资料尚未加载。可在“设置 → 数据更新 → 精灵技能资料”检查更新。</p>")
        .arg(escaped(fallbackName.isEmpty() ? QStringLiteral("技能资料") : fallbackName)));
  const auto root = catalog->root;
  const auto pet = root.value(QStringLiteral("pets")).toObject().value(QString::number(raceId)).toObject();
  if (pet.isEmpty())
    return document(QStringLiteral("<h2>%1</h2><p class='notice'>种族 %2 暂无对应技能资料；可检查技能数据更新。</p>")
        .arg(escaped(fallbackName.isEmpty() ? QStringLiteral("未知精灵") : fallbackName)).arg(raceId));

  const auto skills = root.value(QStringLiteral("skills")).toObject();
  const auto buffs = root.value(QStringLiteral("buffs")).toObject();
  const auto combos = root.value(QStringLiteral("combos")).toObject();
  const auto supplement = skillSupplement();
  QJsonObject entries;
  const auto supplementalMechanisms = supplement.value(QStringLiteral("mechanisms")).toObject();
  for (auto it = supplementalMechanisms.constBegin(); it != supplementalMechanisms.constEnd(); ++it) {
    const auto mechanism = it.value().toObject();
    // Parameterized terms are resolved from the current official base entry at
    // display time, so a later official wording change cannot be shadowed by
    // the bundled snapshot.
    if (mechanism.value(QStringLiteral("source")).toString() ==
        QStringLiteral("parameterized-official-entry")) continue;
    const QString text = mechanism.value(QStringLiteral("text")).toString().trimmed();
    if (!text.isEmpty()) entries.insert(it.key(), text);
  }
  // Insert the live official glossary last.  If the game later publishes an
  // exact definition for a supplemented term, that official definition wins.
  const auto officialEntries = root.value(QStringLiteral("entries")).toObject();
  for (auto it = officialEntries.constBegin(); it != officialEntries.constEnd(); ++it)
    entries.insert(it.key(), it.value());
  const auto slotsObject = pet.value(QStringLiteral("slots")).toObject();
  const QString name = pet.value(QStringLiteral("name")).toString(fallbackName);
  QString html = QStringLiteral("<table class='hero' width='100%' cellspacing='0'><tr><td><h2>%1</h2>"
                                "<p class='hero-meta'>种族 %2 · %3 · %4</p>")
      .arg(escaped(name)).arg(raceId).arg(escaped(pet.value(QStringLiteral("position")).toString()),
                                         escaped(pet.value(QStringLiteral("quality")).toString()));
  if (!pet.value(QStringLiteral("officialRole")).toString().isEmpty())
    html += QStringLiteral("<p class='summary'><b>官方定位</b> · %1</p>")
        .arg(mechanismLines(pet.value(QStringLiteral("officialRole")).toString(), entries));
  if (!pet.value(QStringLiteral("strategy")).toString().isEmpty())
    html += QStringLiteral("<p class='summary'><b>机制摘要</b> · %1</p>")
        .arg(mechanismLines(pet.value(QStringLiteral("strategy")).toString(), entries));
  const auto traits = pet.value(QStringLiteral("traits")).toArray();
  if (!traits.isEmpty()) {
    html += QStringLiteral("<p class='summary'><b>特殊机制</b> · ");
    for (const auto& value : traits) html += QStringLiteral("<span class='tag'>%1</span>").arg(escaped(value.toString()));
    html += QStringLiteral("</p>");
  }
  html += QStringLiteral("</td></tr></table><p class='reading-hint'>蓝色加粗词为机制词条；鼠标移入立即查看解释，移出即关闭。</p>");

  QStringList slotRows;
  for (const auto& slot : skillSlots()) {
    const int id = slotsObject.value(slot.first).toInt();
    if (id <= 0) continue;
    const auto skill = skills.value(QString::number(id)).toObject();
    slotRows.append(QStringLiteral("<tr><td class='slot-name'>%1</td><td><b>%2</b> <span class='id'>ID %3</span></td></tr>")
        .arg(escaped(slotLabel(slot.first, pet, skill)),
             escaped(skill.value(QStringLiteral("name")).toString(QStringLiteral("未命名")))).arg(id));
  }
  const auto starGodSkill = pet.value(QStringLiteral("starGodSkill")).toObject();
  if (!starGodSkill.isEmpty())
    slotRows.append(QStringLiteral("<tr><td class='slot-name'>星神技</td><td><b>%1</b></td></tr>")
        .arg(escaped(starGodSkill.value(QStringLiteral("name")).toString())));
  if (!slotRows.isEmpty())
    html += QStringLiteral("<h3>技能槽总览 <span class='id'>· 仅显示已拥有的 %1 个槽位</span></h3>"
                           "<table class='slot-table' width='100%'>%2</table>")
        .arg(slotRows.size())
        .arg(slotRows.join(QString()));

  html += QStringLiteral("<h3>技能与实质效果 <span class='id'>· 按槽位顺序阅读</span></h3>");
  for (const auto& slot : skillSlots()) {
    const int id = slotsObject.value(slot.first).toInt();
    if (id > 0) html += skillCard(slotLabel(slot.first, pet, skills.value(QString::number(id)).toObject()),
                                 id, skills, combos, buffs, entries);
  }
  if (!starGodSkill.isEmpty()) {
    const QString description = starGodSkill.value(QStringLiteral("description")).toString();
    html += QStringLiteral("<table class='skill-card' width='100%' cellspacing='0'><tr>"
                           "<td class='skill-type' width='33%'><b>星神技</b></td><td class='skill-head' width='67%'><b>%1</b></td></tr>"
                           "<tr><td class='skill-body' colspan='2'><p>%2</p></td></tr></table>")
        .arg(escaped(starGodSkill.value(QStringLiteral("name")).toString()), mechanismLines(description, entries));
  }

  QString advancedHtml;
  const int transformId = slotsObject.value(QStringLiteral("transform")).toInt();
  const auto transform = root.value(QStringLiteral("transformSkills")).toObject()
      .value(QString::number(transformId)).toObject();
  if (!transform.isEmpty()) {
    advancedHtml += QStringLiteral("<h4>元素 / 转化规则</h4><div class='secondary'><b>%1</b><p>%2%3</p></div>")
        .arg(escaped(transform.value(QStringLiteral("name")).toString()),
             mechanismLines(transform.value(QStringLiteral("description")).toString(), entries),
             transform.value(QStringLiteral("teamDescription")).toString().isEmpty() ? QString() :
                 QStringLiteral("；") + mechanismLines(transform.value(QStringLiteral("teamDescription")).toString(), entries));
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
      advancedHtml += QStringLiteral("<li>被通灵精灵种族：%1；条件类型：%2；条件参数：%3</li>")
          .arg(rule.value(QStringLiteral("cpr")).toInt())
          .arg(escaped(rule.value(QStringLiteral("c")).toVariant().toString()),
               escaped(rule.value(QStringLiteral("cP")).toString()));
    }
    advancedHtml += QStringLiteral("</ul></div>");
  }

  advancedHtml += ruleSection(root, pet, entries);
  advancedHtml += relatedSection(pet, entries);
  advancedHtml += relationSection(pet, entries);

  const int evolutionFrom = pet.value(QStringLiteral("evolutionFrom")).toInt();
  if (evolutionFrom > 0) {
    const auto previous = root.value(QStringLiteral("pets")).toObject().value(QString::number(evolutionFrom)).toObject();
    advancedHtml += QStringLiteral("<h4>进化前技能</h4><p>%1（种族 %2）</p><ul>")
        .arg(escaped(previous.value(QStringLiteral("name")).toString(QStringLiteral("未知形态")))).arg(evolutionFrom);
    const auto previousSlots = previous.value(QStringLiteral("slots")).toObject();
    for (const auto& slot : skillSlots()) {
      const int id = previousSlots.value(slot.first).toInt();
      if (id <= 0) continue;
      const auto skill = skills.value(QString::number(id)).toObject();
      advancedHtml += QStringLiteral("<li>%1：<b>%2</b>（%3）— %4</li>")
          .arg(escaped(slotLabel(slot.first, previous, skill)), escaped(skill.value(QStringLiteral("name")).toString()))
          .arg(id).arg(mechanismLines(skill.value(QStringLiteral("description")).toString(), entries));
    }
    advancedHtml += QStringLiteral("</ul>");
  }
  const auto skins = pet.value(QStringLiteral("skins")).toArray();
  if (!skins.isEmpty()) {
    advancedHtml += QStringLiteral("<h4>皮肤 / 异形态技能 ID</h4><ul>");
    for (const auto& value : skins) {
      const auto skin = value.toObject();
      advancedHtml += QStringLiteral("<li>%1（种族 %2）</li>").arg(escaped(skin.value(QStringLiteral("name")).toString()))
          .arg(skin.value(QStringLiteral("raceId")).toInt());
    }
    advancedHtml += QStringLiteral("</ul>");
  }
  if (!advancedHtml.isEmpty()) html += QStringLiteral("<h3>进阶规则与关联效果</h3>") + advancedHtml;
  html += QStringLiteral("<p class='muted'>资料版本：%1。伤害结算公式和未公开的服务器数值不在客户端缓存中。</p>")
      .arg(escaped(root.value(QStringLiteral("source")).toObject().value(QStringLiteral("version")).toString()));
  const QString evaluation = supplement.value(QStringLiteral("evaluations")).toObject()
      .value(QString::number(raceId)).toObject().value(QStringLiteral("text")).toString().trimmed();
  if (!evaluation.isEmpty())
    html += QStringLiteral("<h3>技能简评</h3><div class='assessment'>%1</div>").arg(escaped(evaluation));
  return document(html);
}
