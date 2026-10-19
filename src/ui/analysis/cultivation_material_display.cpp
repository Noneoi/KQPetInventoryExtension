#include "cultivation_material_display.h"

bool showCultivationMaterial(const CultivationMaterialPlanRow& row) {
  // Official items[2078] is 聚星精华. Use its identity so older cached names
  // are also hidden; exact-name fallback covers legacy rows without identity.
  return !(row.type == 4 && row.id == 2078) && row.name.trimmed() != QStringLiteral("聚星精华");
}

CultivationMaterialCell cultivationMaterialCell(const PetAssetRecord& pet, const QString& category) {
  if (!pet.detailAvailable) {
    const auto text = QStringLiteral("缺少精灵详情，读取后可查看材料");
    return {text,text};
  }
  QStringList sections;
  QString html = QStringLiteral("<table width='99%' cellspacing='0' cellpadding='3'>");
  for (const auto& group : {QStringLiteral("sacred"),QStringLiteral("astrolabe"),QStringLiteral("badges"),QStringLiteral("stargods")}) {
    if (!category.isEmpty() && category != group) continue;
    const auto plan = buildCultivationMaterialPlan({&pet},group,{});
    QList<QPair<QString,QString>> lines;
    for (const auto& row : plan.materials) {
      if (!showCultivationMaterial(row)) continue;
      const auto amount = row.complete ? QStringLiteral("× %1").arg(row.required) : row.required > 0 ?
          QStringLiteral("≥ %1").arg(row.required) : QStringLiteral("待确认");
      lines.append({row.name,amount});
    }
    // Keep the original unknownness, without repeating a long explanation for
    // every category or claiming that hidden-only demand means fully trained.
    if (plan.unknownPetCount > 0) lines.append({QStringLiteral("部分材料待确认"),QStringLiteral("—")});
    if (lines.isEmpty()) continue;
    const auto label = group == "astrolabe" ? QStringLiteral("星轮") : cultivationMaterialCategoryName(group);
    QStringList plain;
    for (int i = 0; i < lines.size(); ++i) {
      const auto& line = lines.at(i);
      plain.append(line.second == QStringLiteral("—") ? line.first : line.first + QLatin1Char(' ') + line.second);
      html += QStringLiteral("<tr>");
      if (i == 0) html += QStringLiteral("<td width='54' rowspan='%1' valign='top' class='category'>%2</td>")
          .arg(lines.size()).arg(label.toHtmlEscaped());
      html += QStringLiteral("<td>%1</td><td width='64' align='right'><b>%2</b></td></tr>")
          .arg(line.first.toHtmlEscaped(),line.second.toHtmlEscaped());
    }
    sections.append(label + QLatin1Char('\n') + plain.join(QLatin1Char('\n')));
  }
  html += QStringLiteral("</table>");
  if (sections.isEmpty()) {
    const auto text = QStringLiteral("暂无需要展示的材料");
    return {text,text};
  }
  return {sections.join(QStringLiteral("\n\n")),html};
}
