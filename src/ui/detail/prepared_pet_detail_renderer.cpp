#include "prepared_pet_detail_renderer.h"
#include "html_document.h"
#include "stargod_ring_object.h"
#include <QHash>
#include <QSet>

namespace {
using kqpet::html::escaped;
using kqpet::html::number;
QString sectionName(DetailSection section) {
  switch (section) {
    case DetailSection::Overview: return QStringLiteral("全部概览");
    case DetailSection::Badges: return QStringLiteral("元魂");
    case DetailSection::Astrolabe: return QStringLiteral("星轮");
    case DetailSection::EquippedStargods: return QStringLiteral("已装备星神");
    case DetailSection::StargodBackpack: return QStringLiteral("星神背包");
    case DetailSection::SummonRelations: return QStringLiteral("召唤物关系");
    case DetailSection::CarryRelations: return QStringLiteral("神使契约");
  }
  return {};
}
QString link(DetailSection section, int page, const QString& title) {
  return QStringLiteral("<a href='kqdetail://page/%1/%2'>%3</a>").arg(int(section)).arg(page).arg(escaped(title));
}
QString row(const QString& label, const QString& text) {
  return QStringLiteral("<tr><td width='32%' style='color:#64748b'>%1</td><td>%2</td></tr>")
      .arg(escaped(label), escaped(text));
}
QString fields(const QVector<DetailField>& values) {
  QString text = QStringLiteral("<table width='100%' cellspacing='3'>");
  for (const auto& value : values) {
    QString shown = value.text;
    if (shown.isEmpty()) shown = value.state == DetailKnowledge::Known ? QStringLiteral("无") : QStringLiteral("未知");
    if (value.state == DetailKnowledge::Invalid) shown = QStringLiteral("无法解释：%1").arg(shown);
    text += row(value.label, shown);
  }
  return text + QStringLiteral("</table>");
}
QString document(const QString& body) {
  return kqpet::html::document(QStringLiteral(
      "h3{font-size:14px;margin:14px 0 6px;color:#24547f;}"
      "td{padding:3px 0;vertical-align:top;}"
      ".entry{margin:5px 0;padding:6px;background:#f5f7fa;}"), body);
}
QString valueFor(const DetailEntry& entry, const QString& label) {
  for (const auto& value : entry.fields) if (value.label == label) return value.text;
  return {};
}
QString badgeCell(const DetailEntry& entry) {
  QString text = QStringLiteral("<b>%1</b>").arg(escaped(entry.name));
  const QString level = valueFor(entry,QStringLiteral("等级"));
  if (!level.isEmpty()) text += QStringLiteral("　%1级").arg(escaped(level));
  const QString exclusive = valueFor(entry,QStringLiteral("专属元魂"));
  if (!exclusive.isEmpty()) text += QStringLiteral("<br>%1　%2").arg(escaped(exclusive),escaped(valueFor(entry,QStringLiteral("觉醒"))));
  if (!entry.problem.isEmpty()) text += QStringLiteral("<br><span class='muted'>%1</span>").arg(escaped(entry.problem));
  return text;
}
QString badges(const DetailPage& page) {
  QString html = QStringLiteral("<table width='100%' cellspacing='0' cellpadding='7'>");
  for (int i = 0; i < page.entries.size(); i += 2) {
    html += QStringLiteral("<tr><td width='49%'>%1</td><td width='1' bgcolor='#ccd5df'></td><td width='49%'>%2</td></tr>")
        .arg(badgeCell(page.entries[i]), i + 1 < page.entries.size() ? badgeCell(page.entries[i+1]) : QString());
  }
  return html + QStringLiteral("</table>");
}
QString astrolabe(const DetailPage& page, bool breakthroughApplicable) {
  QStringList lighted, dark, unknown; QString breakthrough;
  for (const auto& entry : page.entries) {
    if (entry.name == QStringLiteral("星轮状态")) {
      if (breakthroughApplicable) breakthrough = valueFor(entry,QStringLiteral("突破"));
      continue;
    }
    QString name = escaped(entry.name);
    if (entry.selected) name = QStringLiteral("<b style='color:#dc2626'>%1</b>").arg(name);
    if (entry.fields.isEmpty() || entry.fields[0].state != DetailKnowledge::Known) { unknown.append(name); continue; }
    if (entry.activated) lighted.append(name);
    else {
      QStringList costs;
      for (const auto& value : entry.fields) if (value.label == QStringLiteral("点亮需要")) costs.append(escaped(value.text));
      if (!costs.isEmpty()) name += QStringLiteral("（点亮需要：%1）").arg(costs.join(QStringLiteral("，")));
      dark.append(name);
    }
  }
  QString html = QStringLiteral("<p>已点亮：%1</p><p>未点亮：%2</p>").arg(lighted.isEmpty() ? QStringLiteral("—") : lighted.join(QStringLiteral("　")),dark.isEmpty() ? QStringLiteral("—") : dark.join(QStringLiteral("　")));
  if (!breakthrough.isEmpty()) html += QStringLiteral("<p>突破状态：%1</p>").arg(escaped(breakthrough));
  return html;
}
QString starCell(const DetailEntry& entry, bool center = false) {
  const QString color = entry.quality >= 6 ? QStringLiteral("#ff7373") : entry.quality == 5 ? QStringLiteral("#ffd76a") : entry.quality == 4 ? QStringLiteral("#d2a0ff") : QStringLiteral("#ffffff");
  QString content;
  if (entry.emptySlot) content = center ? QStringLiteral("<span style='color:#cbd5e1'>万变</span>") : QStringLiteral("&nbsp;");
  else {
    if (entry.imageDefineId > 0) content += QStringLiteral("<img src='kqstargod://icon/%1' width='48' height='48'><br>").arg(entry.imageDefineId);
    content += QStringLiteral("<b style='color:%1'>%2</b>").arg(color,escaped(entry.name));
    const QString level = valueFor(entry,QStringLiteral("等级"));
    if (!level.isEmpty()) content += QStringLiteral("<br><span style='color:#e2e8f0'>%1级</span>").arg(escaped(level));
  }
  return QStringLiteral("<table width='100%' border='1' bordercolor='%1' bgcolor='#334155' cellspacing='0' cellpadding='5'><tr><td align='center' height='84'>%2</td></tr></table>")
      .arg(center ? QStringLiteral("#d6b85a") : QStringLiteral("#8b9aae"),content);
}
QString equippedStars(const DetailPage& page) {
  QVector<StargodRingSlot> cells;
  for (const auto& entry : page.entries) cells.append({entry.imageDefineId,entry.quality,
      valueFor(entry,QStringLiteral("等级")).toInt(),entry.name,entry.emptySlot,entry.changeable});
  return cells.isEmpty() ? QString{} : QStringLiteral("<p><img src='%1' width='400' height='400'></p>")
      .arg(escaped(stargodRingUrl(cells)));
}
QString backpackStars(const DetailPage& page) {
  QString html = QStringLiteral("<table width='100%' cellspacing='5' cellpadding='0'>");
  for (int i = 0; i < page.entries.size(); i += 3) {
    html += QStringLiteral("<tr>");
    for (int col = 0; col < 3; ++col) html += QStringLiteral("<td width='33%'>%1</td>").arg(i + col < page.entries.size() ? starCell(page.entries[i+col]) : QString());
    html += QStringLiteral("</tr>");
  }
  return html + QStringLiteral("</table>");
}
// Where this account keeps a related pet, in the words the inventory tabs use.
QString ownershipName(DetailOwnership ownership) {
  switch (ownership) {
    case DetailOwnership::Backpack: return QStringLiteral("背包");
    case DetailOwnership::WarehouseNormal: return QStringLiteral("普通仓库");
    case DetailOwnership::WarehouseElite: return QStringLiteral("精英仓库");
    case DetailOwnership::WarehouseGoodbye: return QStringLiteral("告别仓库");
    case DetailOwnership::Missing: return QStringLiteral("不在本账号");
    case DetailOwnership::Unknown: break;
  }
  return QStringLiteral("位置待确认");
}
// Held means this account's own rosters list the pet; an entry the client sent
// but could not identify is still held, and keeps its row so its problem shows.
bool held(const DetailEntry& entry) { return entry.ownership != DetailOwnership::Missing; }
// A held pet opens its own detail popup; one this account does not hold, or one
// with no usable instance, has no detail to open and stays plain text.
QString relatedName(const DetailEntry& entry, const QString& color) {
  const QString text = QStringLiteral("<b style='color:%1'>%2</b>").arg(color, escaped(entry.name));
  if (!held(entry) || entry.relatedInstanceId <= 0) return text;
  return QStringLiteral("<a href='kqdetail://pet/%1'>%2</a>").arg(entry.relatedInstanceId).arg(text);
}
QString relationRows(const QVector<const DetailEntry*>& entries, const QString& color) {
  QString html = QStringLiteral("<table width='100%' cellspacing='0' cellpadding='4'>"
      "<tr><td width='26%' style='color:#64748b'>精灵</td><td width='18%' style='color:#64748b'>原名</td>"
      "<td width='14%' style='color:#64748b'>位置</td><td width='10%' style='color:#64748b'>等级</td>"
      "<td width='16%' style='color:#64748b'>当前战力</td><td style='color:#64748b'>极限战力</td></tr>");
  for (const auto* entry : entries) {
    html += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td><td>%5</td><td>%6</td></tr>")
        .arg(relatedName(*entry, color),
             escaped(entry->originalName.isEmpty() ? QStringLiteral("—") : entry->originalName),
             escaped(ownershipName(entry->ownership)), escaped(valueFor(*entry,QStringLiteral("等级"))),
             escaped(valueFor(*entry,QStringLiteral("当前战力"))),
             escaped(valueFor(*entry,QStringLiteral("极限战力"))));
    const QString era = valueFor(*entry,QStringLiteral("时代"));
    if (!era.isEmpty())
      html += QStringLiteral("<tr><td colspan='6' class='muted'>时代 %1</td></tr>").arg(escaped(era));
    if (!entry->problem.isEmpty())
      html += QStringLiteral("<tr><td colspan='6' class='muted'>%1</td></tr>").arg(escaped(entry->problem));
  }
  return html + QStringLiteral("</table>");
}
// A long candidate list is unreadable as one wrapped line of names, so it is
// laid out as a fixed-width grid instead. Cells arrive as finished HTML.
QString grid(const QStringList& cells, int columns) {
  if (cells.isEmpty() || columns < 1) return {};
  QString html = QStringLiteral("<table width='100%' cellspacing='0' cellpadding='4'>");
  for (int index = 0; index < cells.size(); index += columns) {
    html += QStringLiteral("<tr>");
    for (int column = 0; column < columns; ++column)
      html += QStringLiteral("<td width='%1%'>%2</td>").arg(100 / columns)
          .arg(index + column < cells.size() ? cells[index + column] : QString());
    html += QStringLiteral("</tr>");
  }
  return html + QStringLiteral("</table>");
}
QString candidateCell(const DetailEntry& entry) {
  QString cell = relatedName(entry, QStringLiteral("#233044"));
  QStringList note;
  if (!entry.originalName.isEmpty()) note.append(escaped(entry.originalName));
  note.append(escaped(ownershipName(entry.ownership)));
  return cell + QStringLiteral("<br><span class='muted'>%1</span>").arg(note.join(QStringLiteral(" · ")));
}
QString heading(const QString& title, const QString& note) {
  if (note.isEmpty()) return QStringLiteral("<p><b>%1</b></p>").arg(escaped(title));
  return QStringLiteral("<p><b>%1</b>　<span class='muted'>%2</span></p>").arg(escaped(title), escaped(note));
}
// Pets this account does not hold carry no level, power or detail of their own.
// They are listed apart so the held ones stay comparable row by row.
QString missingGroup(const QStringList& names, const QString& title) {
  if (names.isEmpty()) return {};
  return QStringLiteral("<p class='muted'>%1　共 %2 只不在本账号的背包或仓库</p>")
      .arg(escaped(title)).arg(names.size()) + grid(names, 4);
}
QString relationBlock(const QString& title, const QString& note, const QVector<const DetailEntry*>& entries,
                      const QString& color, const QString& emptyText) {
  QVector<const DetailEntry*> rows; QStringList missing;
  for (const auto* entry : entries) {
    if (held(*entry)) rows.append(entry);
    else missing.append(QStringLiteral("<span class='muted'>%1</span>").arg(escaped(entry->name)));
  }
  QString html = heading(title, note);
  if (rows.isEmpty() && missing.isEmpty()) return html + QStringLiteral("<p class='muted'>%1</p>").arg(escaped(emptyText));
  if (!rows.isEmpty()) html += relationRows(rows, color);
  return html + missingGroup(missing, QStringLiteral("以下未拥有"));
}
QString carry(const DetailPage& page) {
  QVector<const DetailEntry*> carried, carriers, candidates;
  for (const auto& entry : page.entries) {
    if (entry.group == DetailRelationGroup::carryCandidate()) candidates.append(&entry);
    else if (entry.group == DetailRelationGroup::carrierOwner()) carriers.append(&entry);
    else carried.append(&entry);
  }
  QString html;
  // Only the side this pet is actually on is shown: a carrier is never told
  // that nobody carries it, and an envoy is never offered a candidate list.
  if (page.contractCarrierRole) {
    if (page.pageIndex == 0 || !carried.isEmpty())
      html += relationBlock(QStringLiteral("已契约神使"), QStringLiteral("本精灵当前携带的精灵"),
                            carried, QStringLiteral("#b91c1c"), QStringLiteral("未契约任何神使"));
    if (page.hasCarryCandidates) {
      // Expanded, the grid is the exact list; collapsed, the client's own list
      // length is reported without resolving every candidate identity.
      const int total = page.carryCandidatesExpanded ? int(candidates.size()) : page.carryCandidateCount;
      html += QStringLiteral("<p><b>%1</b>　<span class='muted'>%2</span>　%3</p>")
          .arg(QStringLiteral("可契约候选"),
               total >= 0 ? QStringLiteral("共 %1 只").arg(total) : QStringLiteral("数量待确认"),
               page.carryCandidatesExpanded ? link(DetailSection::Overview,0,QStringLiteral("▼ 收起"))
                                            : link(DetailSection::CarryRelations,0,QStringLiteral("▶ 展开全部")));
      if (page.carryCandidatesExpanded) {
        QStringList heldCells, missingNames;
        for (const auto* entry : candidates) {
          if (held(*entry)) heldCells.append(candidateCell(*entry));
          else missingNames.append(QStringLiteral("<span class='muted'>%1</span>").arg(escaped(entry->name)));
        }
        html += grid(heldCells, 3) + missingGroup(missingNames, QStringLiteral("以下未拥有"));
      }
    } else if (page.pageIndex == 0) {
      html += heading(QStringLiteral("可契约候选"),
                      page.carryCandidatesKnown ? QStringLiteral("无") : QStringLiteral("待确认"));
    }
  }
  if (page.contractEnvoyRole && (page.pageIndex == 0 || !carriers.isEmpty()))
    html += relationBlock(QStringLiteral("被契约"), QStringLiteral("契约本精灵的精灵"),
                          carriers, QStringLiteral("#1d4ed8"), QStringLiteral("没有精灵契约本精灵"));
  return html;
}
QString summon(const DetailPage& page) {
  QStringList order; QHash<QString, QVector<const DetailEntry*>> grouped;
  for (const auto& entry : page.entries) {
    if (!grouped.contains(entry.group)) order.append(entry.group);
    grouped[entry.group].append(&entry);
  }
  QString html;
  for (const auto& group : order)
    html += relationBlock(group, {}, grouped.value(group), QStringLiteral("#233044"), QStringLiteral("无"));
  return html;
}
}
QString PreparedPetDetailRenderer::waiting(const QString& name, const QString& error) {
  QString body = QStringLiteral("<h2>%1</h2><p class='muted'>%2</p>")
      .arg(escaped(name), escaped(error.isEmpty() ? QStringLiteral("正在准备本地详情……") : error));
  if (!error.isEmpty()) body += link(DetailSection::Overview, 0, QStringLiteral("重试本地详情"));
  return document(body);
}
QString PreparedPetDetailRenderer::render(const PreparedPetDetailHandle& detail, const QString& imageUrl, bool compact, bool fetching) {
  if (!detail) return waiting(QStringLiteral("精灵详情"));
  const auto& identity = detail->identity;
  QString header = QStringLiteral("<h2>%1</h2><p class='muted'>实例 %2　种族 %3</p>")
      .arg(escaped(identity.customName.isEmpty() ? identity.name : identity.customName)).arg(identity.instanceId)
      .arg(identity.raceId > 0 ? QString::number(identity.raceId) : QStringLiteral("—"));
  const auto identityField = [](const QString& label, const QString& text) {
    return DetailField{{}, label, text, text.isEmpty() ? DetailKnowledge::Unknown : DetailKnowledge::Known};
  };
  header += fields({identityField(QStringLiteral("原名"), identity.originalName), identity.level,
      identityField(QStringLiteral("属性"), identity.attributes), identityField(QStringLiteral("职业"), identity.jobs),
      identityField(QStringLiteral("时代"), identity.era)});
  if (!imageUrl.isEmpty()) {
    const QString picture = QStringLiteral("<img src='%1' width='160' height='160' alt='精灵图片'>").arg(escaped(imageUrl));
    header = compact ? picture + header : QStringLiteral("<table width='100%'><tr><td>%1</td><td width='170'>%2</td></tr></table>").arg(header, picture);
  }
  if (fetching) header += QStringLiteral("<p class='muted'>正在获取最新详情……</p>");
  if (!detail->sourceVerified) header += QStringLiteral("<p class='muted'>当前显示的是本地缓存，可能不是最新数据。</p>");
  if (detail->visualMismatch) header += QStringLiteral("<p class='muted'>形态已变化，培养信息待最新详情确认。</p>");
  QString navigation = link(DetailSection::Overview, 0, sectionName(DetailSection::Overview)) + QStringLiteral("　");
  QSet<int> seen;
  for (const auto& page : detail->pages) {
    if (!page.relationsApplicable || seen.contains(int(page.section))) continue;
    seen.insert(int(page.section));
    if (!page.problem.startsWith(QStringLiteral("该时代没有")))
      navigation += link(page.section, 0, sectionName(page.section)) + QStringLiteral("　");
  }
  QString body = header + QStringLiteral("<p>%1</p><h3>战斗力</h3><table width='100%'>").arg(navigation);
  const auto& power = detail->battlePower;
  body += row(QStringLiteral("官方当前 / 官方极限"), number(power.hasServerCurrent, power.serverCurrent) + " / " + number(power.hasExtreme, power.extreme));
  body += row(QStringLiteral("持有可达 / 至高战力"), number(power.hasCurrent, power.current) + " / " + number(power.hasHighest, power.highest));
  const bool known = detail->detailKnown && power.completionKnown && detail->cultivationRequirements.completeKnown;
  body += row(QStringLiteral("培养状态"), known ? (power.isHighest && detail->cultivationRequirements.complete ? QStringLiteral("已达至高（升无可升）") : QStringLiteral("尚有培养空间，见精灵分析")) : QStringLiteral("部分分析数据待补齐"));
  body += QStringLiteral("</table>");
  if (!detail->talent.isEmpty()) {
    body += QStringLiteral("<h3>天赋</h3>");
    for (const auto& value : detail->talent) body += QStringLiteral("<p>%1：%2</p>").arg(escaped(value.label),escaped(value.text));
  }
  if (!detail->proficient.isEmpty()) body += QStringLiteral("<h3>潜能</h3>") + fields(detail->proficient);
  if (!detail->equipment.isEmpty()) body += QStringLiteral("<h3>源兽装备</h3>") + fields(detail->equipment);
  if (!detail->legendStone.isEmpty()) body += QStringLiteral("<h3>传说石</h3>") + fields(detail->legendStone);
  if (!detail->sacred.isEmpty()) body += QStringLiteral("<h3>神源兽</h3>") + fields(detail->sacred);
  bool starHeading = false;
  for (const auto& page : detail->pages) {
    if (page.problem.startsWith(QStringLiteral("该时代没有"))) continue;
    // A pet that takes part in neither system gets no empty relation section.
    if (!page.relationsApplicable) continue;
    const bool relation = page.section == DetailSection::SummonRelations || page.section == DetailSection::CarryRelations;
    const bool stars = page.section == DetailSection::EquippedStargods || page.section == DetailSection::StargodBackpack;
    if (stars && !starHeading) { body += QStringLiteral("<h3>星神</h3>"); starHeading = true; }
    body += stars ? QStringLiteral("<p><b>%1</b></p>").arg(page.section == DetailSection::EquippedStargods ? QStringLiteral("已装备星神") : QStringLiteral("对应背包的星神")) : QStringLiteral("<h3>%1</h3>").arg(sectionName(page.section));
    if (!page.problem.isEmpty()) body += QStringLiteral("<p class='muted'>%1</p>").arg(escaped(page.problem));
    if (page.entries.isEmpty() && !relation) body += QStringLiteral("<p class='muted'>%1</p>").arg(page.state == DetailKnowledge::Known ? QStringLiteral("无记录") : QStringLiteral("信息未确认"));
    if (page.section == DetailSection::Badges) body += badges(page);
    else if (page.section == DetailSection::Astrolabe) body += astrolabe(page, power.breakthroughApplicable);
    else if (page.section == DetailSection::EquippedStargods && !page.entries.isEmpty()) body += equippedStars(page);
    else if (page.section == DetailSection::StargodBackpack) body += backpackStars(page);
    else if (page.section == DetailSection::CarryRelations) body += carry(page);
    else if (page.section == DetailSection::SummonRelations) body += summon(page);
    else for (const auto& entry : page.entries) {
      body += QStringLiteral("<div class='entry'><b>%1</b>　<span class='muted'>%2</span>%3")
          .arg(escaped(entry.name), escaped(entry.group), fields(entry.fields));
      if (!entry.problem.isEmpty()) body += QStringLiteral("<p class='muted'>%1</p>").arg(escaped(entry.problem));
      body += QStringLiteral("</div>");
    }
    if (page.hasPrevious() || page.hasNext()) {
      body += QStringLiteral("<p class='muted'>第 %1 页 · 共 %2 项　").arg(page.pageIndex + 1).arg(page.totalItems);
      if (page.hasPrevious()) body += link(page.section, page.pageIndex - 1, QStringLiteral("上一页")) + QStringLiteral("　");
      if (page.hasNext()) body += link(page.section, page.pageIndex + 1, QStringLiteral("下一页"));
      body += QStringLiteral("</p>");
    }
  }
  return document(body);
}
bool PreparedPetDetailRenderer::petLink(const QUrl& url, qint64* instanceId) {
  if (!instanceId || url.scheme() != "kqdetail" || url.host() != "pet") return false;
  const auto parts = url.path().split('/', Qt::SkipEmptyParts);
  if (parts.size() != 1) return false;
  bool ok = false;
  const qint64 parsed = parts[0].toLongLong(&ok);
  if (!ok || parsed <= 0) return false;
  *instanceId = parsed; return true;
}
bool PreparedPetDetailRenderer::pageLink(const QUrl& url, DetailSection* section, int* pageIndex) {
  if (!section || !pageIndex || url.scheme() != "kqdetail" || url.host() != "page") return false;
  const auto parts = url.path().split('/', Qt::SkipEmptyParts);
  if (parts.size() != 2) return false;
  bool sectionOk = false, pageOk = false;
  const int parsedSection = parts[0].toInt(&sectionOk), parsedPage = parts[1].toInt(&pageOk);
  if (!sectionOk || !pageOk || parsedSection < 0 || parsedSection > int(DetailSection::CarryRelations) || parsedPage < 0) return false;
  *section = DetailSection(parsedSection); *pageIndex = parsedPage; return true;
}
