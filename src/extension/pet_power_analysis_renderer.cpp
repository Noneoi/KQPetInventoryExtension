#include "pet_power_analysis_renderer.h"
#include "html_document.h"

namespace {
using kqpet::html::escaped;
using kqpet::html::number;
QString gain(bool known, int value) {
  return known ? (value > 0 ? QStringLiteral("+%1").arg(value) : QString::number(value)) : QStringLiteral("—");
}
QString document(const QString& body) {
  return kqpet::html::document(QStringLiteral(
      "h3{font-size:14px;margin:16px 0 6px;color:#24547f;}"
      "td,th{padding:5px 4px;vertical-align:top;}th{text-align:left;background:#eff4fa;color:#345777;}"
      ".notice{background:#fff6df;color:#94651d;padding:8px;}"
      ".value{font-size:16px;font-weight:600;color:#24547f;}.positive{color:#b45309;}"), body);
}
QString retry() { return QStringLiteral("<p><a href='kqanalysis://retry'>重新准备本地分析</a></p>"); }
QString metric(const QString& title, bool known, int value) {
  return QStringLiteral("<tr><td width='54%'>%1</td><td class='value'>%2</td></tr>")
      .arg(escaped(title), number(known, value));
}
QString requirementTable(const PetCultivationRequirements& requirements) {
  if (requirements.items.isEmpty()) {
    return QStringLiteral("<p class='muted'>%1</p>").arg(requirements.completeKnown && requirements.complete
        ? QStringLiteral("已无养成缺口") : QStringLiteral("详情不足，暂无法列出养成缺口"));
  }
  QString html = QStringLiteral("<table width='100%' cellspacing='0'><tr><th width='32%'>项目</th><th>还需培养</th></tr>");
  for (const auto& item : requirements.items) {
    const QString name = item.name.isEmpty() ? item.category : item.name;
    html += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>")
        .arg(escaped(name), escaped(item.status.isEmpty() ? QStringLiteral("待确认") : item.status));
  }
  return html + QStringLiteral("</table>");
}
struct MaterialTotal {
  int type = 0, id = 0, extra = 0;
  QString name, scope;
  qint64 required = 0;
  bool known = true;
};
QString materialTable(const PetCultivationRequirements& requirements, const MaterialInventorySnapshot& inventory) {
  QList<MaterialTotal> totals;
  QHash<QString, qsizetype> positions;
  for (const auto& item : requirements.items) {
    for (const auto& material : item.materials) {
      if (material.known && material.count == 0) continue;
      const bool identified = material.type > 0 && material.id > 0 && material.extra >= 0;
      const QString key = material.scope + QChar(0x1f) + (identified
          ? cultivationMaterialKey(material.type, material.id, material.extra)
          : QStringLiteral("unknown:") + item.category + QChar(0x1f) + material.name);
      auto found = positions.constFind(key);
      if (found == positions.cend()) {
        MaterialTotal total;
        total.type = material.type;
        total.id = material.id;
        total.extra = material.extra;
        total.name = material.name.isEmpty() ? QStringLiteral("对应%1材料").arg(item.category) : material.name;
        total.scope = material.scope;
        positions.insert(key, totals.size());
        totals.append(total);
        found = positions.constFind(key);
      }
      auto& total = totals[found.value()];
      total.known = total.known && material.known && material.count >= 0;
      if (material.known && material.count > 0) total.required += material.count;
    }
  }
  if (totals.isEmpty()) return {};
  QString html = QStringLiteral("<h3>所需材料</h3><table width='100%' cellspacing='0'><tr>"
      "<th width='43%'>材料</th><th>需要</th><th>背包已有</th><th>还差</th></tr>");
  for (const auto& material : totals) {
    const auto stock = inventory.counts.constFind(cultivationMaterialKey(material.type, material.id, material.extra));
    const bool stockKnown = material.scope == QStringLiteral("account") && material.type > 0 && material.id > 0 &&
        material.extra >= 0 && (stock != inventory.counts.cend() ? stock.value() >= 0 : material.extra == 0
            ? inventory.knownTypes.contains(material.type)
            : inventory.knownExtraGroups.contains(QStringLiteral("%1:%2").arg(material.type).arg(material.extra)));
    const qint64 owned = stockKnown && stock != inventory.counts.cend() ? stock.value() : 0;
    const bool missingKnown = material.known && stockKnown;
    const qint64 missing = missingKnown ? qMax<qint64>(0, material.required - owned) : 0;
    html += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td><td>%3</td><td%4>%5</td></tr>")
        .arg(escaped(material.name), material.known ? QString::number(material.required) : QStringLiteral("待确认"),
             stockKnown ? QString::number(owned) : QStringLiteral("未读取"),
             missingKnown && missing > 0 ? QStringLiteral(" class='positive'") : QString(),
             missingKnown ? QString::number(missing) : QStringLiteral("—"));
  }
  return html + QStringLiteral("</table>");
}
QString componentTable(const QList<PetBattlePowerComponent>& components) {
  if (components.isEmpty()) return QStringLiteral("<p class='muted'>详情尚未提供可分析的战力分项。</p>");
  QString html = QStringLiteral("<table width='100%' cellspacing='0'><tr><th width='25%'>构成</th>"
      "<th>本地当前</th><th>官方极限<br>分项</th><th>至高<br>分项</th><th>尚缺<br>战斗力</th></tr>");
  for (const auto& component : components) {
    if (!component.applicable) continue;
    html += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td><td>%3</td><td>%4</td><td>%5</td></tr>")
        .arg(escaped(component.label), number(component.currentKnown, component.current),
             number(component.extremeKnown, component.extreme),
             number(component.highestKnown, component.highest),
             gain(component.gapKnown, component.gap));
  }
  return html + QStringLiteral("</table>");
}
}

QString PetPowerAnalysisRenderer::waiting(const QString& name, const QString& error) {
  QString html = QStringLiteral("<h2>%1</h2><p class='muted'>%2</p>").arg(escaped(name),
      escaped(error.isEmpty() ? QStringLiteral("正在准备本地分析……") : error));
  if (!error.isEmpty()) html += retry();
  return document(html);
}

QString PetPowerAnalysisRenderer::render(const PreparedPetDetailHandle& detail, const QDateTime& observedAt,
                                         bool refreshing, const QString& error,
                                         const MaterialInventorySnapshot& materials) {
  if (!detail) return waiting(QStringLiteral("精灵分析"), error);
  const auto& power = detail->battlePower;
  const QString name = detail->identity.customName.isEmpty() ? detail->identity.name : detail->identity.customName;
  QString html = QStringLiteral("<h2>%1</h2><p class='muted'>实例 %2　数据时间：%3</p>")
      .arg(escaped(name)).arg(detail->identity.instanceId)
      .arg(observedAt.isValid() ? observedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QStringLiteral("未记录"));
  if (refreshing) {
    html += QStringLiteral("<p class='notice'>%1；保留上次分析及其数据时间。</p>")
        .arg(escaped(error.isEmpty() ? QStringLiteral("正在根据更新后的详情重新分析") : error));
    if (!error.isEmpty()) html += retry();
  } else if (!error.isEmpty()) {
    html += QStringLiteral("<p class='notice'>%1</p>").arg(escaped(error)) + retry();
  }
  if (!detail->detailKnown) html += QStringLiteral("<p class='notice'>详情未齐全，缺失项显示为 —。</p>");

  html += QStringLiteral("<h3>距离至高还缺</h3>") + requirementTable(detail->cultivationRequirements);
  html += materialTable(detail->cultivationRequirements, materials);

  html += QStringLiteral("<h3>战斗力对照</h3><table width='100%' cellspacing='0'>");
  html += metric(QStringLiteral("官方返回当前战斗力"), power.hasServerCurrent, power.serverCurrent);
  html += metric(QStringLiteral("官方极限战斗力"), power.hasExtreme, power.extreme);
  html += metric(QStringLiteral("已装备本地战斗力"), power.equippedCurrentKnown, power.equippedCurrent);
  html += metric(QStringLiteral("本地持有可达战斗力"), power.hasCurrent && power.currentLocallyCalculated, power.current);
  html += metric(QStringLiteral("至高战斗力（升无可升）"), power.hasHighest, power.highest);
  html += metric(QStringLiteral("距至高尚缺战斗力"), power.highestGapKnown, power.highestGap);
  html += QStringLiteral("</table>");
  html += QStringLiteral("<p><b>培养状态：</b>%1</p>").arg(power.completionKnown && detail->cultivationRequirements.completeKnown
      ? (power.isHighest && detail->cultivationRequirements.complete ? QStringLiteral("已具备至高培养条件") : QStringLiteral("仍有可提升项目"))
      : QStringLiteral("数据尚未齐全"));

  html += QStringLiteral("<h3>战斗力具体构成</h3>") + componentTable(power.components);

  return document(html);
}
