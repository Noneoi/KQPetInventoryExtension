#include "cultivation_material_panel.h"
#include "cultivation_material_display.h"

#include <QFont>
#include <QScrollBar>
#include <QTextDocument>

namespace {
QString muted(const QString& text) {
  return QStringLiteral("<span style='color:#65758a;font-size:9pt'>%1</span>").arg(text.toHtmlEscaped());
}

QString heading(const QString& title, const QString& summary) {
  // Leave a rounding allowance: Qt's rich-text tables can overflow by a pixel
  // at 100%, causing an otherwise unnecessary horizontal scrollbar.
  return QStringLiteral("<table width='99%' cellspacing='0' cellpadding='0'><tr>"
      "<td><span style='font-size:11pt;font-weight:600'>%1</span></td>"
      "<td align='right'>%2</td></tr></table>")
      .arg(title.toHtmlEscaped(),muted(summary));
}
}

CultivationMaterialPanel::CultivationMaterialPanel(QWidget* parent) : QTextBrowser(parent) {
  setObjectName(QStringLiteral("KQCultivationTotals"));
  setAccessibleName(QStringLiteral("养成材料清单"));
  setReadOnly(true);
  setOpenLinks(false);
  setOpenExternalLinks(false);
  setFont(QFont(QStringLiteral("Microsoft YaHei UI"),10));
  setMinimumHeight(150);
  setStyleSheet(QStringLiteral("QTextBrowser { background:#ffffff; color:#25364a; "
      "border:1px solid #dce5ef; border-radius:6px; padding:4px; }"));
  document()->setDocumentMargin(10);
  resetPlan();
}

void CultivationMaterialPanel::showDocument(const QString& body, const QString& scope) {
  const QString html = QStringLiteral("<html><head><style>"
      "body { color:#25364a; font-family:'Microsoft YaHei UI'; font-size:10pt; }"
      "p { margin-top:7px; margin-bottom:7px; }"
      "td { vertical-align:middle; }"
      "</style></head><body>%1</body></html>").arg(body);
  if (renderedHtml_ == html) return;
  const int scroll = scope_ == scope ? verticalScrollBar()->value() : 0;
  scope_ = scope;
  renderedHtml_ = html;
  setHtml(html);
  verticalScrollBar()->setValue(scroll);
}

void CultivationMaterialPanel::resetPlan() {
  showDocument(heading(QStringLiteral("材料清单"),{}) +
      QStringLiteral("<p>%1</p>").arg(muted(QStringLiteral("请先重新计算养成分析，查看材料缺口和对应精灵。"))),
      QStringLiteral("initial"));
}

void CultivationMaterialPanel::setPlan(const CultivationMaterialPlan& plan, bool onlySelected) {
  const auto scope = plan.category + (onlySelected ? QStringLiteral(":selected") : QStringLiteral(":filtered"));
  const QString title = plan.category.isEmpty() ? QStringLiteral("材料清单") :
      cultivationMaterialCategoryName(plan.category) + QStringLiteral(" · 材料清单");
  if (plan.petCount == 0) {
    showDocument(heading(title,{}) + QStringLiteral("<p>%1</p>").arg(muted(onlySelected ?
        QStringLiteral("请在下方列表选择精灵，可按 Ctrl 或 Shift 多选。") :
        QStringLiteral("当前筛选没有精灵，试试调整筛选条件。"))),scope);
    return;
  }

  int missing = 0, ready = 0, pending = 0;
  bool hasPetScoped = false;
  int visibleMaterials = 0;
  for (const auto& row : plan.materials) {
    if (!showCultivationMaterial(row)) continue;
    ++visibleMaterials;
    if (!row.missing) ++pending;
    else if (*row.missing > 0) ++missing;
    else ++ready;
    hasPetScoped |= row.scope == "pet";
  }
  QStringList summary;
  if (missing) summary.append(QStringLiteral("%1 种待补").arg(missing));
  if (pending) summary.append(QStringLiteral("%1 种待确认").arg(pending));
  if (ready) summary.append(QStringLiteral("%1 种已备齐").arg(ready));
  summary.append(QStringLiteral("%1 %2 只精灵").arg(onlySelected ? QStringLiteral("已选") : QStringLiteral("当前筛选")).arg(plan.petCount));
  QString body = heading(title,summary.join(QStringLiteral("   ·   ")));
  if (plan.unknownPetCount > 0)
    body += QStringLiteral("<p style='color:#93601b;font-size:9pt'>%1 只精灵资料不全，下方仅列已知需求，不能代表全部缺口。</p>").arg(plan.unknownPetCount);

  if (visibleMaterials == 0) {
    body += QStringLiteral("<p>%1</p>").arg(muted(plan.unknownPetCount > 0 ?
        QStringLiteral("暂未找到可确认的材料需求，请补齐精灵详情后重新分析。") :
        QStringLiteral("当前筛选没有需要展示的材料。")));
  } else {
    body += QStringLiteral("<p style='font-size:3pt'>&nbsp;</p>"
        "<table width='99%' cellspacing='0' cellpadding='8'>"
        "<tr bgcolor='#eef3f8' style='color:#53657b;font-size:9pt'>"
        "<td width='29%'>材料</td><td width='41%'>对应精灵</td>"
        "<td width='10%' align='center'>需要</td><td width='10%' align='center'>已有</td>"
        "<td width='10%' align='center'>还缺</td></tr>");
    int index = 0;
    for (const auto& row : plan.materials) {
      if (!showCultivationMaterial(row)) continue;
      QString name = QStringLiteral("<b>%1</b>").arg(row.name.toHtmlEscaped());
      if (plan.category.isEmpty()) name += QStringLiteral("<br>") + muted(cultivationMaterialCategoryName(row.category));
      if (!row.complete) name += QStringLiteral("<br>") + muted(QStringLiteral("需求尚未确认完整"));
      QStringList pets;
      for (const auto& pet : row.pets) {
        QString label = pet.name.isEmpty() ? QStringLiteral("未命名精灵") : pet.name;
        QString detail;
        if (pet.distinguishInstance) detail = QStringLiteral("实例 %1").arg(pet.instanceId);
        // A sole contributor's quantity is already in the numeric columns.
        if (row.pets.size() > 1 || !row.complete) {
          if (!detail.isEmpty()) detail += QStringLiteral(" · ");
          detail += QStringLiteral("%1 %2").arg(row.scope == "pet" ? QStringLiteral("缺") : QStringLiteral("需")).arg(pet.count);
        }
        pets.append(label.toHtmlEscaped() + (detail.isEmpty() ? QString{} : QStringLiteral(" &nbsp; ") + muted(detail)));
      }
      if (pets.isEmpty()) pets.append(muted(QStringLiteral("对应需求待确认")));
      const QString required = row.scope == "pet" ? QStringLiteral("—") :
          (row.complete ? QString{} : QStringLiteral("≥ ")) + QString::number(row.required);
      const QString owned = row.scope == "pet" ? QStringLiteral("已计入") :
          row.owned ? QString::number(*row.owned) : QStringLiteral("未读取");
      const QString deficit = !row.missing ? QStringLiteral("待确认") : *row.missing == 0 ?
          QStringLiteral("已备齐") : QString::number(*row.missing);
      const QString color = !row.missing ? QStringLiteral("#65758a") : *row.missing == 0 ?
          QStringLiteral("#28735a") : QStringLiteral("#a65313");
      body += QStringLiteral("<tr bgcolor='%1'><td>%2</td><td>%3</td>"
          "<td align='center'>%4</td><td align='center'>%5</td>"
          "<td align='center' style='color:%6;font-size:11pt'><b>%7</b></td></tr>")
          .arg(index++ % 2 == 0 ? QStringLiteral("#ffffff") : QStringLiteral("#f6f8fb"),
               name,pets.join(QStringLiteral("<br>")),required,owned,color,deficit);
    }
    body += QStringLiteral("</table>");
  }
  QStringList notes;
  notes.append(QStringLiteral("同种材料合并需求，共享库存只扣一次。"));
  if (hasPetScoped) notes.append(QStringLiteral("红星已扣除各精灵自身持有，直接显示还缺数量。"));
  if (plan.observedAt.isValid()) notes.append(QStringLiteral("库存读取于 %1；获得或消耗后请刷新。")
      .arg(plan.observedAt.toLocalTime().toString(QStringLiteral("MM-dd HH:mm"))));
  else if (pending) notes.append(QStringLiteral("点击“刷新材料库存”可核对持有数量。"));
  body += QStringLiteral("<p>%1</p>").arg(muted(notes.join(QStringLiteral(" "))));
  showDocument(body,scope);
}
