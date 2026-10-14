#include "prepared_pet_detail_renderer.h"
#include "public_data_detail_dialog.h"
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
    case DetailSection::CarryRelations: return QStringLiteral("携带关系");
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
    QString table;
    switch (value.definitionKind) {
      case DetailDefinitionKind::Proficient: table=QStringLiteral("proficiencies"); break;
      case DetailDefinitionKind::SourceBeast: table=QStringLiteral("sourceBeasts"); break;
      case DetailDefinitionKind::LegendStone: table=QStringLiteral("legendStones"); break;
      case DetailDefinitionKind::SacredEquipment: table=QStringLiteral("sacredEquipment"); break;
      case DetailDefinitionKind::None: break;
    }
    if (!table.isEmpty() && value.definitionId>=0) text+=QStringLiteral("<tr><td width='32%' style='color:#64748b'>%1</td><td>%2</td></tr>")
        .arg(escaped(value.label),PublicDataDetailLinks::anchor({PublicDataViewKind::PetDetail,table,QString::number(value.definitionId)},shown));
    else text += row(value.label, shown);
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
  QString text = QStringLiteral("<b>%1</b>").arg(entry.definitionId>=0 ?
      PublicDataDetailLinks::anchor({PublicDataViewKind::PetDetail,QStringLiteral("badges"),QString::number(entry.definitionId)},entry.name) : escaped(entry.name));
  const QString level = valueFor(entry,QStringLiteral("等级"));
  if (!level.isEmpty()) text += QStringLiteral("　%1级").arg(escaped(level));
  const QString exclusive = valueFor(entry,QStringLiteral("专属元魂"));
  if (!exclusive.isEmpty()) text += QStringLiteral("<br>%1　%2").arg(entry.exclusiveDefinitionId>=0 ?
      PublicDataDetailLinks::anchor({PublicDataViewKind::PetDetail,QStringLiteral("badges"),QString::number(entry.exclusiveDefinitionId)},exclusive) : escaped(exclusive),escaped(valueFor(entry,QStringLiteral("觉醒"))));
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
    // This is the user's compact display filter, not a mechanic classification.
    // Domain still retains every node and counts/calculates the full raw record.
    if (!entry.name.startsWith(QStringLiteral("星灵"))) {
      if (entry.state != DetailKnowledge::Known && !entry.problem.isEmpty())
        unknown.append(QStringLiteral("<p class='muted'>%1；完整记录见原始数据。</p>").arg(escaped(entry.problem)));
      continue;
    }
    QString name = entry.definitionId>=0 ? PublicDataDetailLinks::anchor({PublicDataViewKind::PetDetail,QStringLiteral("astrolabe"),QString::number(entry.definitionId)},entry.name) : escaped(entry.name);
    bool unconfirmed = entry.state != DetailKnowledge::Known || entry.fields.isEmpty();
    for (const auto& value : entry.fields) unconfirmed |= value.state != DetailKnowledge::Known;
    if (unconfirmed) {
      QString detail = QStringLiteral("<div class='entry'><b>%1</b>%2").arg(name, fields(entry.fields));
      if (!entry.problem.isEmpty()) detail += QStringLiteral("<p class='muted'>%1</p>").arg(escaped(entry.problem));
      unknown.append(detail + QStringLiteral("</div>"));
      continue;
    }
    if (entry.selected) name = QStringLiteral("<b style='color:#dc2626'>%1</b>").arg(name);
    if (entry.activated) lighted.append(name);
    else {
      QStringList costs;
      for (const auto& value : entry.fields) if (value.label == QStringLiteral("点亮需要")) costs.append(escaped(value.text));
      if (!costs.isEmpty()) name += QStringLiteral("（点亮需要：%1）").arg(costs.join(QStringLiteral("，")));
      dark.append(name);
    }
  }
  QString html = QStringLiteral("<p>已点亮：%1</p><p>未点亮：%2</p>").arg(lighted.isEmpty() ? QStringLiteral("—") : lighted.join(QStringLiteral("　")),dark.isEmpty() ? QStringLiteral("—") : dark.join(QStringLiteral("　")));
  if (!unknown.isEmpty()) html += QStringLiteral("<p><b>状态待确认</b></p>") + unknown.join(QString());
  if (!breakthrough.isEmpty()) html += QStringLiteral("<p>突破状态：%1</p>").arg(escaped(breakthrough));
  return html;
}
QString starCell(const DetailEntry& entry, bool center = false) {
  const QString color = entry.quality >= 6 ? QStringLiteral("#ff7373") : entry.quality == 5 ? QStringLiteral("#ffd76a") : entry.quality == 4 ? QStringLiteral("#d2a0ff") : QStringLiteral("#ffffff");
  QString content;
  if (entry.emptySlot) content = center ? QStringLiteral("<span style='color:#cbd5e1'>万变</span>") : QStringLiteral("&nbsp;");
  else {
    if (entry.imageDefineId > 0) content += QStringLiteral("<img src='kqstargod://icon/%1' width='48' height='48'><br>").arg(entry.imageDefineId);
    content += QStringLiteral("<b style='color:%1'>%2</b>").arg(color,entry.definitionId>=0 ?
        PublicDataDetailLinks::anchor({PublicDataViewKind::PetDetail,QStringLiteral("stargods"),QString::number(entry.definitionId)},entry.name) : escaped(entry.name));
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
  if (cells.isEmpty()) return {};
  QStringList definitions; QSet<int> linked;
  for (const auto& entry : page.entries) if (entry.definitionId>=0 && !linked.contains(entry.definitionId)) {
    linked.insert(entry.definitionId); definitions.append(PublicDataDetailLinks::anchor(
      {PublicDataViewKind::PetDetail,QStringLiteral("stargods"),QString::number(entry.definitionId)},entry.name));
  }
  return QStringLiteral("<p><img src='%1' width='400' height='400'></p><p>%2</p>")
      .arg(escaped(stargodRingUrl(cells)),definitions.join(QStringLiteral("　")));
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
    case DetailOwnership::WarehouseUnknown: return QStringLiteral("仓库（分组未记录）");
    case DetailOwnership::OwnedLocationUnknown: return QStringLiteral("已拥有 · 位置待确认");
    case DetailOwnership::Missing: return QStringLiteral("当前列表未记录");
    case DetailOwnership::Unknown: break;
  }
  return QStringLiteral("位置待确认");
}
// Only a present roster entry can open an account-instance detail. Unidentified
// and currently unrecorded relations remain visible without a misleading link.
bool held(const DetailEntry& entry) {
  return entry.ownership != DetailOwnership::Missing && entry.ownership != DetailOwnership::Unknown;
}
// A held pet opens its own detail popup; one this account does not hold, or one
// with no usable instance, has no detail to open and stays plain text.
QString relatedName(const DetailEntry& entry, const QString& color) {
  const QString name = entry.originalName.isEmpty() ? entry.name : entry.originalName;
  const QString text = QStringLiteral("<b style='color:%1'>%2</b>").arg(color,
      escaped(name.isEmpty() ? QStringLiteral("原名待核实") : name));
  if (!held(entry) || entry.relatedInstanceId <= 0) return text;
  return QStringLiteral("<a href='kqdetail://pet/%1'>%2</a>").arg(entry.relatedInstanceId).arg(text);
}
QString currentRelations(const QString& title, const QVector<const DetailEntry*>& entries, const QString& emptyText = {}) {
  QString html;
  for (const auto* entry : entries) {
    html += QStringLiteral("<p><b>%1：</b>%2　<span class='muted'>%3%4</span></p>").arg(
        escaped(title),relatedName(*entry,QStringLiteral("#24547f")),escaped(ownershipName(entry->ownership)),
        entry->state==DetailKnowledge::Invalid ? QStringLiteral(" · 关联记录需核对") : QString());
  }
  if (entries.isEmpty() && !emptyText.isEmpty())
    html += QStringLiteral("<p><b>%1：</b><span class='muted'>%2</span></p>").arg(escaped(title),escaped(emptyText));
  return html;
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
  const bool missing = entry.ownership==DetailOwnership::Missing;
  QStringList note;
  if (entry.selected) note.append(QStringLiteral("<b style='color:#287743'>当前关联</b>"));
  if (!missing && entry.ownership!=DetailOwnership::Backpack)
    note.append(escaped(ownershipName(entry.ownership)));
  if (entry.state==DetailKnowledge::Invalid) note.append(QStringLiteral("关联记录需核对"));
  const QString color = missing ? QStringLiteral("#64748b") : QStringLiteral("#233044");
  const QString contents = relatedName(entry,color) + (note.isEmpty() ? QString() :
      QStringLiteral("<br><span class='muted'>%1</span>").arg(note.join(QStringLiteral(" · "))));
  return QStringLiteral("<table width='100%' border='1' bordercolor='%1' bgcolor='%2' cellspacing='0' cellpadding='7'>"
      "<tr><td>%3</td></tr></table>").arg(entry.selected ? QStringLiteral("#8ab69a") : QStringLiteral("#dce4ee"),
      entry.selected ? QStringLiteral("#eef8f1") : missing ? QStringLiteral("#f7f7f8") : QStringLiteral("#f3f7fc"),contents);
}
QString heading(const QString& title, const QString& note) {
  if (note.isEmpty()) return QStringLiteral("<p><b>%1</b></p>").arg(escaped(title));
  return QStringLiteral("<p><b>%1</b>　<span class='muted'>%2</span></p>").arg(escaped(title), escaped(note));
}
int ownershipGroup(DetailOwnership ownership) {
  switch (ownership) {
    case DetailOwnership::Backpack: return 0;
    case DetailOwnership::WarehouseNormal:
    case DetailOwnership::WarehouseElite:
    case DetailOwnership::WarehouseGoodbye:
    case DetailOwnership::WarehouseUnknown: return 1;
    case DetailOwnership::OwnedLocationUnknown: return 2;
    case DetailOwnership::Unknown: return 3;
    case DetailOwnership::Missing: return 4;
  }
  return 3;
}
QString locationGroups(const QVector<const DetailEntry*>& entries) {
  const QStringList titles{QStringLiteral("背包内"),QStringLiteral("仓库内"),QStringLiteral("已拥有 · 位置待确认"),
      QStringLiteral("位置待确认"),QStringLiteral("当前列表未记录")};
  QString html;
  for (int location=0; location<titles.size(); ++location) {
    QVector<const DetailEntry*> rows;
    for (const auto* entry : entries) if (ownershipGroup(entry->ownership)==location) rows.append(entry);
    if (rows.isEmpty()) continue;
    html += heading(titles[location],(location==4 ? QStringLiteral("%1 种") : QStringLiteral("%1 只")).arg(rows.size()));
    if (location==4) html += QStringLiteral("<p class='muted'>本地背包和仓库列表中未记录，不等同于未拥有。</p>");
    QStringList cells; for (const auto* entry : rows) cells.append(candidateCell(*entry));
    html += grid(cells,3);
  }
  return html;
}
QString availableRelations(const QString& title, const QVector<const DetailEntry*>& entries,
                           bool known, const DetailPage& page) {
  QString html = heading(title,entries.isEmpty() ? QString() : QStringLiteral("共 %1 项").arg(entries.size()));
  if (page.relationCatalogLoading)
    html += QStringLiteral("<p class='muted'>正在读取官方关联资料…</p>");
  else if (!page.relationCatalogProblem.isEmpty())
    html += QStringLiteral("<p class='muted'>官方关联资料暂不可用，请更新数据后重试。</p>");
  else if (!known)
    html += QStringLiteral("<p class='muted'>暂无可用的官方配对规则。</p>");
  else if (entries.isEmpty())
    html += QStringLiteral("<p class='muted'>无可选对象。</p>");
  if (!entries.isEmpty()) {
    html += known ? QStringLiteral("<p class='muted'>按官方配对列出；标有“当前关联”的对象已选用。</p>")
                  : QStringLiteral("<p class='muted'>以下为已记录的关联对象，完整可选范围尚未确认。</p>");
    html += locationGroups(entries);
  }
  return html;
}
QString carryObjectName(DetailCarryKind kind) {
  switch (kind) {
    case DetailCarryKind::Swallower: return QStringLiteral("吞噬对象");
    case DetailCarryKind::Carrier: return QStringLiteral("携同精灵");
    case DetailCarryKind::Almighty: return QStringLiteral("天觉携带精灵");
    case DetailCarryKind::Unknown: return QStringLiteral("携带精灵");
  }
  return QStringLiteral("携带精灵");
}
QString carryOwnerName(DetailCarryKind kind) {
  switch (kind) {
    case DetailCarryKind::Swallower: return QStringLiteral("吞噬方");
    case DetailCarryKind::Carrier: return QStringLiteral("携同方");
    case DetailCarryKind::Almighty: return QStringLiteral("天觉携带方");
    case DetailCarryKind::Unknown: return QStringLiteral("携带者");
  }
  return QStringLiteral("携带者");
}
QString pageName(const DetailPage& page) {
  if (page.section != DetailSection::CarryRelations) return sectionName(page.section);
  if (!page.contractCarrierRole) return sectionName(page.section);
  if (page.carryKind == DetailCarryKind::Swallower) return QStringLiteral("吞噬关系");
  if (page.carryKind == DetailCarryKind::Carrier) return QStringLiteral("携同关系");
  if (page.carryKind == DetailCarryKind::Almighty) return QStringLiteral("天觉携带关系");
  return sectionName(page.section);
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
      html += currentRelations(QStringLiteral("当前%1").arg(carryObjectName(page.carryKind)),carried,QStringLiteral("当前关联未记录"));
    const auto title = QStringLiteral("可选%1").arg(carryObjectName(page.carryKind));
    if (page.hasCarryCandidates && !page.carryCandidatesExpanded && candidates.isEmpty()) {
      // Legacy raw-only DTOs still resolve long lists on demand. Complete
      // official-rule/roster projections always arrive expanded, including in
      // Overview, so ordinary readers never need this extra click.
      html += heading(title,page.carryCandidateCount>=0 ? QStringLiteral("已记录 %1 项").arg(page.carryCandidateCount) : QString());
      html += QStringLiteral("<p>%1</p>").arg(link(DetailSection::CarryRelations,0,QStringLiteral("查看全部")));
    } else html += availableRelations(title,candidates,page.carryCandidatesKnown,page);
  }
  if (page.contractEnvoyRole && (page.pageIndex == 0 || !carriers.isEmpty())) {
    for (const auto kind : {DetailCarryKind::Carrier,DetailCarryKind::Almighty,DetailCarryKind::Swallower,DetailCarryKind::Unknown}) {
      QVector<const DetailEntry*> owners;
      for (const auto* entry : carriers) if (entry->carryKind==kind) owners.append(entry);
      if (!owners.isEmpty()) html += currentRelations(carryOwnerName(kind),owners);
    }
  }
  return html;
}
QString summon(const DetailPage& page) {
  QVector<const DetailEntry*> current, summoners, candidates;
  for (const auto& entry : page.entries) {
    if (entry.group==DetailRelationGroup::summonList()) candidates.append(&entry);
    else if (entry.group==DetailRelationGroup::summoner()) summoners.append(&entry);
    else current.append(&entry);
  }
  QString html;
  if (!current.isEmpty()) html += currentRelations(QStringLiteral("当前召唤对象"),current);
  if (!summoners.isEmpty()) html += currentRelations(QStringLiteral("召唤者"),summoners);
  if (!candidates.isEmpty() || !current.isEmpty() || page.hasSummonCandidates || page.summonCandidatesKnown || summoners.isEmpty())
    html += availableRelations(QStringLiteral("可选召唤对象"),candidates,page.summonCandidatesKnown,page);
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
  const auto original=identity.originalName.isEmpty() ? QStringLiteral("原名待核实") : identity.originalName;
  QString header = QStringLiteral("<h2>%1</h2>").arg(identity.raceId>0 ? PublicDataDetailLinks::anchor(
      {PublicDataViewKind::PetDetail,QStringLiteral("pets"),QString::number(identity.raceId)},original) : escaped(original));
  if (!identity.name.isEmpty() && identity.name!=identity.originalName)
    header+=QStringLiteral("<p class='muted'>皮肤 / 形态：%1</p>").arg(escaped(identity.name));
  if (!identity.customName.isEmpty()) header+=QStringLiteral("<p class='muted'>昵称：%1</p>").arg(escaped(identity.customName));
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
      navigation += link(page.section, 0, pageName(page)) + QStringLiteral("　");
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
    body += stars ? QStringLiteral("<p><b>%1</b></p>").arg(page.section == DetailSection::EquippedStargods ? QStringLiteral("已装备星神") : QStringLiteral("对应背包的星神")) : QStringLiteral("<h3>%1</h3>").arg(pageName(page));
    if (!page.problem.isEmpty() && !relation) body += QStringLiteral("<p class='muted'>%1</p>").arg(escaped(page.problem));
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
    if (!relation && (page.hasPrevious() || page.hasNext())) {
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
