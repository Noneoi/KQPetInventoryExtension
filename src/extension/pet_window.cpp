#include "pet_window.h"

#include "pet_detail_catalog.h"
#include "pet_identity.h"
#include "pet_image_cache.h"
#include "pet_repository.h"
#include "pet_search.h"

#include <QApplication>
#include <QComboBox>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextLayout>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>

namespace {

QString valueText(const QJsonValue& value) {
  if (value.isString())
    return value.toString();
  if (value.isDouble())
    return QString::number(value.toDouble(), 'g', 16);
  if (value.isBool())
    return value.toBool() ? QStringLiteral("是") : QStringLiteral("否");
  if (value.isNull() || value.isUndefined())
    return QStringLiteral("—");
  if (value.isArray())
    return QStringLiteral("数组（%1 项）").arg(value.toArray().size());
  return QStringLiteral("对象");
}

QString friendlyKey(const QString& key) {
  static const QHash<QString, QString> names = {
      {QStringLiteral("id"), QStringLiteral("实例 ID")},
      {QStringLiteral("r"), QStringLiteral("精灵种族 ID")},
      {QStringLiteral("ri"), QStringLiteral("精灵种族 ID")},
      {QStringLiteral("n"), QStringLiteral("名称")},
      {QStringLiteral("customName"), QStringLiteral("自定义名称")},
      {QStringLiteral("lv"), QStringLiteral("等级")},
      {QStringLiteral("fr"), QStringLiteral("头像 ID")},
      {QStringLiteral("g"), QStringLiteral("性别")},
      {QStringLiteral("gd"), QStringLiteral("获得时间")},
      {QStringLiteral("pr"), QStringLiteral("精英标记/阶级")},
      {QStringLiteral("ce"), QStringLiteral("当前经验")},
      {QStringLiteral("cte"), QStringLiteral("累计经验")},
      {QStringLiteral("ne"), QStringLiteral("下级经验")},
      {QStringLiteral("chp"), QStringLiteral("当前生命")},
      {QStringLiteral("ip"), QStringLiteral("天赋序列")},
      {QStringLiteral("gps"), QStringLiteral("天赋进度")},
      {QStringLiteral("gt"), QStringLiteral("天赋等级")},
      {QStringLiteral("sgs"), QStringLiteral("星神序列")},
      {QStringLiteral("sgp"), QStringLiteral("星神战力")},
      {QStringLiteral("zdl"), QStringLiteral("当前战斗力")},
      {QStringLiteral("xzdl"), QStringLiteral("极限战斗力")},
      {QStringLiteral("czdlv"), QStringLiteral("当前战斗力分项")},
      {QStringLiteral("mzdlv"), QStringLiteral("极限战斗力分项")},
      {QStringLiteral("cps"), QStringLiteral("通灵师/职业序列")},
      {QStringLiteral("eps"), QStringLiteral("装备序列")},
      {QStringLiteral("lss"), QStringLiteral("传说石序列")},
      {QStringLiteral("badge"), QStringLiteral("元魂")},
      {QStringLiteral("astrolabe"), QStringLiteral("天迹星轮")},
      {QStringLiteral("astrolabebr"), QStringLiteral("天迹星轮突破")},
      {QStringLiteral("shenjue"), QStringLiteral("神源兽")},
      {QStringLiteral("us"), QStringLiteral("超必杀技能")},
      {QStringLiteral("cc"), QStringLiteral("培养次数")},
      {QStringLiteral("p"), QStringLiteral("位置")},
      {QStringLiteral("srpi"), QStringLiteral("召唤者实例 ID")},
      {QStringLiteral("srri"), QStringLiteral("召唤者种族 ID")},
      {QStringLiteral("sepi"), QStringLiteral("被召唤精灵实例 ID")},
      {QStringLiteral("sdpi"), QStringLiteral("仓库被召唤精灵实例 ID")},
      {QStringLiteral("sppl"), QStringLiteral("被召唤精灵详情")},
      {QStringLiteral("asps"), QStringLiteral("召唤精灵列表")},
      {QStringLiteral("cepi"), QStringLiteral("被携带精灵实例 ID")},
      {QStringLiteral("cppl"), QStringLiteral("被携带精灵详情")},
      {QStringLiteral("acps"), QStringLiteral("携带/神使精灵列表")},
      {QStringLiteral("crpis"), QStringLiteral("携带者/神使实例 ID 列表")},
      {QStringLiteral("ownership"), QStringLiteral("关系归属标记")},
      {QStringLiteral("relationEffect"), QStringLiteral("关系效果")},
      {QStringLiteral("_packType"), QStringLiteral("背包类型")},
      {QStringLiteral("_position"), QStringLiteral("背包位置")},
      {QStringLiteral("_warehouseGroup"), QStringLiteral("仓库分组")},
      {QStringLiteral("_location"), QStringLiteral("数据位置")}};
  return names.value(key, key);
}

class SearchHighlightDelegate final : public QStyledItemDelegate {
public:
  SearchHighlightDelegate(const QLineEdit* search, QObject* parent)
      : QStyledItemDelegate(parent), search_(search) {}

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override {
    if (index.column() != 0 || !search_ || search_->text().trimmed().isEmpty()) {
      QStyledItemDelegate::paint(painter, option, index);
      return;
    }
    QStyleOptionViewItem styled(option);
    initStyleOption(&styled, index);
    const QString text = styled.text;
    const QPair<int, int> range = petQueryHighlightRange(search_->text(), text);
    if (range.first < 0 || range.second <= 0) {
      QStyledItemDelegate::paint(painter, option, index);
      return;
    }

    const QWidget* widget = option.widget;
    QStyle* style = widget ? widget->style() : QApplication::style();
    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &styled, widget);
    QStyleOptionViewItem background(styled);
    background.text.clear();
    style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);

    QTextLayout layout(text, styled.font);
    QList<QTextLayout::FormatRange> formats;
    QTextLayout::FormatRange normal;
    normal.start = 0;
    normal.length = text.size();
    normal.format.setForeground(styled.state & QStyle::State_Selected
                                    ? styled.palette.highlightedText()
                                    : styled.palette.text());
    formats.append(normal);
    QTextLayout::FormatRange highlight;
    highlight.start = range.first;
    highlight.length = range.second;
    highlight.format.setForeground(QColor(QStringLiteral("#e22929")));
    highlight.format.setFontWeight(QFont::Bold);
    formats.append(highlight);
    layout.setFormats(formats);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) line.setLineWidth(textRect.width());
    layout.endLayout();
    if (!line.isValid()) return;
    const qreal y = textRect.top() + (textRect.height() - line.height()) / 2.0;
    painter->save();
    painter->setClipRect(textRect);
    layout.draw(painter, QPointF(textRect.left(), y));
    painter->restore();
  }

private:
  const QLineEdit* search_ = nullptr;
};

QTableWidget* makeTable(QWidget* parent) {
  auto* table = new QTableWidget(parent);
  table->setColumnCount(7);
  table->setHorizontalHeaderLabels({QStringLiteral("名称"), QStringLiteral("属性"),
                                    QStringLiteral("职业"), QStringLiteral("时代"),
                                    QStringLiteral("等级"),
                                    QStringLiteral("战斗力 / 极限战斗力"),
                                    QStringLiteral("位置")});
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setAlternatingRowColors(true);
  table->setWordWrap(false);
  table->setTextElideMode(Qt::ElideNone);
  table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  table->setIconSize(QSize(24, 24));
  table->verticalHeader()->setVisible(false);
  table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
  table->verticalHeader()->setDefaultSectionSize(27);
  table->horizontalHeader()->setStretchLastSection(false);
  table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
  table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
  table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
  table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
  table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
  table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Interactive);
  table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Fixed);
  table->setColumnWidth(0, 235);
  table->setColumnWidth(1, 82);
  table->setColumnWidth(2, 145);
  table->setColumnWidth(3, 68);
  table->setColumnWidth(4, 50);
  table->setColumnWidth(5, 205);
  table->setColumnWidth(6, 100);
  table->setMinimumWidth(900);
  return table;
}

enum class PetSortMode {
  Default = 0,
  BattlePower = 1,
  ExtremePower = 2,
  CatalogSequence = 3,
  ObtainedAt = 4
};

void addSortChoices(QComboBox* box) {
  box->addItem(QStringLiteral("默认排序"), static_cast<int>(PetSortMode::Default));
  box->addItem(QStringLiteral("战斗力"), static_cast<int>(PetSortMode::BattlePower));
  box->addItem(QStringLiteral("极限战斗力"), static_cast<int>(PetSortMode::ExtremePower));
  box->addItem(QStringLiteral("图鉴序列"), static_cast<int>(PetSortMode::CatalogSequence));
  box->addItem(QStringLiteral("获得时间"), static_cast<int>(PetSortMode::ObtainedAt));
}

QStringList categoryParts(const QString& text) {
  QStringList result;
  for (QString part : text.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
    part = part.trimmed();
    if (!part.isEmpty()) result.append(part);
  }
  if (result.isEmpty() && !text.trimmed().isEmpty()) result.append(text.trimmed());
  return result;
}

QString eraForPet(const QJsonObject& pet) {
  const PetDetailCatalog& catalog = PetDetailCatalog::instance();
  QString name = catalog.originalName(petRaceId(pet));
  if (name.isEmpty()) name = catalog.petName(petRaceId(pet));
  static const QRegularExpression eraPattern(QStringLiteral("^[\\[【]([^\\]】]+)[\\]】]"));
  const QRegularExpressionMatch match = eraPattern.match(name.trimmed());
  return match.hasMatch() ? match.captured(1).trimmed() : QStringLiteral("其它");
}

QString htmlText(const QString& text) {
  return text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
}

QString detailRow(const QString& label, const QString& value) {
  return QStringLiteral(
             "<tr><td class='label'>%1</td><td class='value'>%2</td></tr>")
      .arg(htmlText(label), value);
}

QString detailSection(const QString& title, const QString& rows) {
  return QStringLiteral("<div class='section'><div class='section-title'>%1</div>"
                        "<table cellspacing='0' cellpadding='0'>%2</table></div>")
      .arg(htmlText(title), rows);
}

QString identitySection(const QString& rows, const QString& imageHtml) {
  return QStringLiteral(
             "<div class='section'><div class='section-title'>基础信息</div>"
             "<table class='identity-layout' cellspacing='0' cellpadding='0'><tr>"
             "<td class='identity-values'><table cellspacing='0' cellpadding='0'>%1</table></td>"
             "<td class='identity-picture'>%2</td></tr></table></div>")
      .arg(rows, imageHtml);
}

QString valueOrDash(const QString& value) {
  return value.isEmpty() ? QStringLiteral("—") : htmlText(value);
}

QStringList splitSequence(const QString& value, QChar separator) {
  return value.split(separator, Qt::SkipEmptyParts);
}

struct BattlePowerState {
  int current = 0;
  int extreme = 0;
  int highest = 0;
  int equippedStars = 0;
  int redStars = 0;
  int goldStars = 0;
  bool breakthrough = false;
  bool hasCurrent = false;
  bool hasExtreme = false;
  bool isHighest = false;
};

BattlePowerState battlePowerState(const QJsonObject& pet,
                                  const PetDetailCatalog& catalog) {
  BattlePowerState result;
  result.hasCurrent = pet.contains(QStringLiteral("zdl"));
  result.hasExtreme = pet.contains(QStringLiteral("xzdl"));
  result.current = pet.value(QStringLiteral("zdl")).toInt();
  result.extreme = pet.value(QStringLiteral("xzdl")).toInt();
  result.highest = result.hasExtreme ? result.extreme + 1350 : 0;
  result.breakthrough = pet.value(QStringLiteral("astrolabebr")).toBool();

  for (const QString& slot : splitSequence(pet.value(QStringLiteral("sgs")).toString(),
                                           QLatin1Char('#'))) {
    const QStringList fields = slot.split(QLatin1Char(':'));
    const int defineId = fields.value(0).toInt();
    if (defineId <= 0)
      continue;
    const int sourceId = fields.size() >= 3 ? fields.value(2).toInt() : -1;
    const QJsonObject item = catalog.stargod(defineId);
    const QJsonObject source = sourceId > 0 ? catalog.stargod(sourceId) : QJsonObject();
    int quality = item.value(QStringLiteral("quality")).toInt();
    if (source.value(QStringLiteral("changeable")).toBool())
      quality = source.value(QStringLiteral("quality")).toInt(quality);
    ++result.equippedStars;
    if (quality == 6)
      ++result.redStars;
    else if (quality == 5)
      ++result.goldStars;
  }

  result.isHighest = result.hasCurrent && result.hasExtreme &&
                     result.current >= result.highest;
  return result;
}

QString battlePowerTableText(const BattlePowerState& state) {
  if (!state.hasCurrent && !state.hasExtreme)
    return QStringLiteral("—");
  const QString current = state.hasCurrent ? QString::number(state.current) : QStringLiteral("—");
  const QString extreme = state.hasExtreme ? QString::number(state.extreme) : QStringLiteral("—");
  if (state.isHighest)
    return QStringLiteral("%1 / %2（最高）").arg(current, extreme);
  if (state.hasCurrent && state.hasExtreme)
    return QStringLiteral("%1 / %2（距最高 %3）")
        .arg(current, extreme)
        .arg(qMax(0, state.highest - state.current));
  return QStringLiteral("%1 / %2").arg(current, extreme);
}

QString gridPositionText(const QJsonObject& pet, const QString& location) {
  if (location != QStringLiteral("backpack")) {
    return pet.value(QStringLiteral("_warehouseGroup")).toString() ==
                   QStringLiteral("elite")
               ? QStringLiteral("精英")
               : QStringLiteral("普通");
  }
  if (!pet.contains(QStringLiteral("_position")))
    return QStringLiteral("待刷新");
  const int index = qMax(0, pet.value(QStringLiteral("_position")).toInt());
  const int page = index / 12 + 1;
  const int positionOnPage = index % 12;
  const int row = positionOnPage / 6 + 1;
  return QStringLiteral("第%1页 第%2排").arg(page).arg(row);
}

QString battlePowerAnalysisHtml(const QJsonObject& pet,
                                const PetDetailCatalog& catalog) {
  const BattlePowerState state = battlePowerState(pet, catalog);
  if (!state.hasCurrent || !state.hasExtreme) {
    return detailSection(
        QStringLiteral("战斗力分析"),
        detailRow(QStringLiteral("状态"),
                  QStringLiteral("当前数据没有战斗力分项；仓库精灵请点击该行或使用“刷新仓库详情”。")));
  }

  static const QList<QPair<QString, QString>> components = {
      {QStringLiteral("lv"), QStringLiteral("等级与基础成长")},
      {QStringLiteral("iv"), QStringLiteral("天赋")},
      {QStringLiteral("pl"), QStringLiteral("职业熟练度")},
      {QStringLiteral("sgv"), QStringLiteral("星神")},
      {QStringLiteral("ep"), QStringLiteral("精灵装备")},
      {QStringLiteral("gsv"), QStringLiteral("守护石")},
      {QStringLiteral("lav"), QStringLiteral("学习力")},
      {QStringLiteral("lsv"), QStringLiteral("传说石")},
      {QStringLiteral("bsv"), QStringLiteral("元魂")},
      {QStringLiteral("asv"), QStringLiteral("天迹星轮")},
      {QStringLiteral("sjv"), QStringLiteral("神源兽")}};
  const QJsonObject currentParts = pet.value(QStringLiteral("czdlv")).toObject();
  const QJsonObject extremeParts = pet.value(QStringLiteral("mzdlv")).toObject();
  QStringList missing;
  int knownGap = 0;
  for (const auto& component : components) {
    if (!extremeParts.contains(component.first))
      continue;
    const int current = currentParts.value(component.first).toInt();
    const int extreme = extremeParts.value(component.first).toInt();
    if (current >= extreme)
      continue;
    const int gap = extreme - current;
    knownGap += gap;
    missing.append(QStringLiteral("<li><b>%1</b>：当前 %2 / 极限 %3，差 <b>%4</b> 战斗力</li>")
                       .arg(htmlText(component.second))
                       .arg(current)
                       .arg(extreme)
                       .arg(gap));
  }

  QString extremeAnalysis;
  if (missing.isEmpty()) {
    extremeAnalysis = QStringLiteral("<span class='ok'>普通养成项目已达到极限配置。</span>");
  } else {
    extremeAnalysis = QStringLiteral("<ul class='analysis'>%1</ul><b>已识别的普通极限缺口：%2</b>")
                          .arg(missing.join(QString()))
                          .arg(knownGap);
  }

  const int starBonus = qMax(0, currentParts.value(QStringLiteral("sgv")).toInt() -
                                   extremeParts.value(QStringLiteral("sgv")).toInt());
  const int astrolabeBonus =
      qMax(0, currentParts.value(QStringLiteral("asv")).toInt() -
                  extremeParts.value(QStringLiteral("asv")).toInt());
  const int missingRedBonus = qMax(0, 1200 - starBonus);
  const int missingRedEquivalent = (missingRedBonus + 149) / 150;
  QString highestAnalysis;
  if (missingRedBonus == 0) {
    highestAnalysis += QStringLiteral(
        "<div class='ok'>星神：服务器实际加成 +1200，已达到8红星最高加成。</div>");
  } else {
    highestAnalysis += QStringLiteral(
                           "<div>星神：界面识别红星 %1/8、金星 %2/8；服务器实际额外战力 "
                           "+%3/+1200，还差 <b>%4</b>（约 %5 个红星加成）。"
                           "若颜色已全红，请检查星神等级、万变底座和星神组合。</div>")
                           .arg(state.redStars)
                           .arg(state.goldStars)
                           .arg(starBonus)
                           .arg(missingRedBonus)
                           .arg(missingRedEquivalent);
  }
  if (astrolabeBonus >= 150) {
    highestAnalysis += QStringLiteral(
        "<div class='ok'>天迹星轮：服务器实际加成 +150，突破加成已满。</div>");
  } else {
    highestAnalysis += QStringLiteral(
                           "<div>天迹星轮：%1；服务器实际额外战力 +%2/+150，还差 <b>%3</b>。</div>")
                           .arg(state.breakthrough ? QStringLiteral("显示已突破")
                                                   : QStringLiteral("未突破"))
                           .arg(astrolabeBonus)
                           .arg(150 - astrolabeBonus);
  }
  const int totalHighestGap = qMax(0, state.highest - state.current);
  highestAnalysis += state.isHighest
                         ? QStringLiteral("<div class='highest'>已达到最高战斗力。</div>")
                         : QStringLiteral("<div class='warning'>当前距离最高战斗力还差 <b>%1</b>。</div>")
                               .arg(totalHighestGap);

  QString rows = detailRow(
      QStringLiteral("战力基准"),
      QStringLiteral("当前 <b>%1</b>　普通极限 <b>%2</b>　最高 <b>%3</b>")
          .arg(state.current)
          .arg(state.extreme)
          .arg(state.highest));
  rows += detailRow(QStringLiteral("普通极限差距"), extremeAnalysis);
  rows += detailRow(QStringLiteral("最高战力差距"), highestAnalysis);
  return detailSection(QStringLiteral("战斗力分析"), rows);
}

QString talentHtml(const QJsonObject& pet) {
  static const QStringList propertyNames = {
      QStringLiteral("生命"),   QStringLiteral("物攻"),   QStringLiteral("物防"),
      QStringLiteral("魔攻"),   QStringLiteral("魔防"),   QStringLiteral("超攻"),
      QStringLiteral("超防"),   QStringLiteral("速度"),   QStringLiteral("超物攻"),
      QStringLiteral("超物防"), QStringLiteral("超魔攻"), QStringLiteral("超魔防")};
  static const QStringList levelNames = {
      QStringLiteral("一无是处"), QStringLiteral("十分常见"), QStringLiteral("百里挑一"),
      QStringLiteral("千载难逢"), QStringLiteral("万众瞩目"), QStringLiteral("王者无敌"),
      QStringLiteral("超凡入圣")};
  static const QStringList energyNames = {
      QString(), QStringLiteral("无星能"), QStringLiteral("单星能"), QStringLiteral("双星能")};

  const int level = pet.value(QStringLiteral("gt")).toInt();
  const QString levelName = level >= 0 && level < levelNames.size()
                                ? levelNames.at(level)
                                : QStringLiteral("等级 %1").arg(level);
  const QStringList values = splitSequence(pet.value(QStringLiteral("ip")).toString(),
                                           QLatin1Char('#'));
  const QStringList energies = splitSequence(pet.value(QStringLiteral("gps")).toString(),
                                             QLatin1Char('#'));
  QStringList normalLines;
  QStringList doubleEnergyLines;
  for (int index = 0; index < values.size() && index < propertyNames.size(); ++index) {
    const int value = values.at(index).toInt();
    if (value <= 0)
      continue;
    QString suffix;
    if (index < energies.size()) {
      const int energy = energies.at(index).toInt();
      if (energy >= 0 && energy < energyNames.size() && !energyNames.at(energy).isEmpty())
        suffix = QStringLiteral(" · %1").arg(energyNames.at(energy));
    }
    const QString line = QStringLiteral("%1 %2%3")
                             .arg(propertyNames.at(index))
                             .arg(value)
                             .arg(suffix);
    const int energy = index < energies.size() ? energies.at(index).toInt() : 0;
    if (energy == 3)
      doubleEnergyLines.append(line);
    else
      normalLines.append(line);
  }
  QString rows = detailRow(QStringLiteral("评价"),
                           QStringLiteral("<b>%1</b>（%2级）").arg(htmlText(levelName)).arg(level));
  rows += detailRow(QStringLiteral("单星能 / 其它"),
                    normalLines.isEmpty()
                        ? QStringLiteral("—")
                        : htmlText(normalLines.join(QStringLiteral("　"))));
  rows += detailRow(QStringLiteral("双星能"),
                    doubleEnergyLines.isEmpty()
                        ? QStringLiteral("—")
                        : QStringLiteral("<b>%1</b>")
                              .arg(htmlText(doubleEnergyLines.join(QStringLiteral("　")))));
  const QJsonObject currentParts = pet.value(QStringLiteral("czdlv")).toObject();
  const QJsonObject fullParts = pet.value(QStringLiteral("mzdlv")).toObject();
  const QString currentTalentPower = currentParts.contains(QStringLiteral("iv"))
                                         ? QString::number(currentParts.value(QStringLiteral("iv")).toInt())
                                         : QStringLiteral("—");
  const QString fullTalentPower = fullParts.contains(QStringLiteral("iv"))
                                      ? QString::number(fullParts.value(QStringLiteral("iv")).toInt())
                                      : QStringLiteral("—");
  rows += detailRow(QStringLiteral("天赋战斗力 / 满天赋战斗力"),
                    QStringLiteral("<b>%1</b> / %2")
                        .arg(currentTalentPower, fullTalentPower));
  return detailSection(QStringLiteral("天赋"), rows);
}

qint64 relationId(const QJsonValue& value) {
  if (value.isDouble()) return static_cast<qint64>(value.toDouble());
  if (value.isString()) return value.toString().toLongLong();
  if (value.isObject()) return petInstanceId(value.toObject());
  return 0;
}

QString relatedPetText(const QJsonObject& embedded, qint64 fallbackId,
                       int fallbackRaceId, const PetRepository* repository) {
  QJsonObject related = embedded;
  qint64 id = petInstanceId(related);
  if (id <= 0) id = fallbackId;
  if (id > 0) {
    QJsonObject merged = repository->detailFor(id);
    for (auto iterator = related.begin(); iterator != related.end(); ++iterator)
      merged.insert(iterator.key(), iterator.value());
    related = merged;
  }

  const PetDetailCatalog& catalog = PetDetailCatalog::instance();
  int raceId = petRaceId(related);
  if (raceId <= 0) raceId = fallbackRaceId;
  QString name = related.value(QStringLiteral("customName")).toString();
  if (name.isEmpty()) name = related.value(QStringLiteral("n")).toString();
  if (name.isEmpty() && raceId > 0) name = catalog.petName(raceId);
  if (name.isEmpty()) name = QStringLiteral("未知精灵");

  QStringList facts;
  QJsonObject eraPet = related;
  if (petRaceId(eraPet) <= 0 && raceId > 0)
    eraPet.insert(QStringLiteral("ri"), raceId);
  facts.append(eraForPet(eraPet));
  if (related.contains(QStringLiteral("lv")))
    facts.append(QStringLiteral("LV.%1").arg(related.value(QStringLiteral("lv")).toInt()));
  const QString currentPower = related.contains(QStringLiteral("zdl"))
                                   ? QString::number(related.value(QStringLiteral("zdl")).toInt())
                                   : QStringLiteral("—");
  const QString extremePower = related.contains(QStringLiteral("xzdl"))
                                   ? QString::number(related.value(QStringLiteral("xzdl")).toInt())
                                   : QStringLiteral("—");
  facts.append(QStringLiteral("%1 / %2").arg(currentPower, extremePower));
  return facts.isEmpty()
             ? QStringLiteral("<b>%1</b>").arg(htmlText(name))
             : QStringLiteral("<b>%1</b>（%2）")
                   .arg(htmlText(name), htmlText(facts.join(QStringLiteral("，"))));
}

QString relatedListText(const QJsonArray& array, const PetRepository* repository) {
  QStringList items;
  QSet<qint64> seenIds;
  for (const QJsonValue& value : array) {
    const QJsonObject embedded = value.toObject();
    const qint64 id = relationId(value);
    if (id > 0 && seenIds.contains(id)) continue;
    if (id > 0) seenIds.insert(id);
    if (embedded.isEmpty() && id <= 0) continue;
    items.append(relatedPetText(embedded, id, petRaceId(embedded), repository));
  }
  return items.join(QStringLiteral("<br>"));
}

QString relationshipHtml(const QJsonObject& pet, const PetRepository* repository) {
  QString summonRows;
  QString carryRows;

  const QJsonObject summoned = pet.value(QStringLiteral("sppl")).toObject();
  qint64 summonedId = relationId(pet.value(QStringLiteral("sepi")));
  if (summonedId <= 0) summonedId = relationId(pet.value(QStringLiteral("sdpi")));
  if (summonedId <= 0) summonedId = petInstanceId(summoned);
  if (summonedId > 0 || !summoned.isEmpty())
    summonRows += detailRow(QStringLiteral("被召唤精灵"),
                            relatedPetText(summoned, summonedId, petRaceId(summoned),
                                           repository));

  const QJsonArray summonList = pet.value(QStringLiteral("asps")).toArray();
  const QString summonListText = relatedListText(summonList, repository);
  if (!summonListText.isEmpty())
    summonRows += detailRow(QStringLiteral("召唤关联列表"), summonListText);

  const qint64 summonerId = relationId(pet.value(QStringLiteral("srpi")));
  const int summonerRaceId = pet.value(QStringLiteral("srri")).toInt();
  if (summonerId > 0 || summonerRaceId > 0)
    summonRows += detailRow(QStringLiteral("召唤者"),
                            relatedPetText({}, summonerId, summonerRaceId, repository));

  const QJsonObject carried = pet.value(QStringLiteral("cppl")).toObject();
  qint64 carriedId = relationId(pet.value(QStringLiteral("cepi")));
  if (carriedId <= 0) carriedId = petInstanceId(carried);
  if (carriedId > 0 || !carried.isEmpty())
    carryRows += detailRow(QStringLiteral("被携带精灵"),
                           relatedPetText(carried, carriedId, petRaceId(carried), repository));

  const QJsonArray carryList = pet.value(QStringLiteral("acps")).toArray();
  const QString carryListText = relatedListText(carryList, repository);
  if (!carryListText.isEmpty())
    carryRows += detailRow(QStringLiteral("携带 / 神使列表"), carryListText);

  const QJsonArray carrierIds = pet.value(QStringLiteral("crpis")).toArray();
  const QString carrierListText = relatedListText(carrierIds, repository);
  if (!carrierListText.isEmpty())
    carryRows += detailRow(QStringLiteral("携带者 / 神使"), carrierListText);

  if (summonRows.isEmpty() && carryRows.isEmpty()) return {};
  QString html;
  if (!summonRows.isEmpty()) html += detailSection(QStringLiteral("召唤关系"), summonRows);
  if (!carryRows.isEmpty())
    html += detailSection(QStringLiteral("携带 / 神使关系"), carryRows);
  return html;
}

QString badgeHtml(const QJsonObject& pet, const PetDetailCatalog& catalog) {
  const QString sequence = pet.value(QStringLiteral("badge")).toString();
  QStringList badgeRows;
  int slotNumber = 1;
  for (const QString& slot : splitSequence(sequence, QLatin1Char('|'))) {
    const QStringList parts = splitSequence(slot, QLatin1Char('#'));
    if (parts.isEmpty())
      continue;
    const QStringList job = parts.at(0).split(QLatin1Char(':'));
    const int jobId = job.value(0).toInt();
    const int level = job.value(1).toInt();
    QString line = QStringLiteral("<b>%1</b> · %2级")
                       .arg(htmlText(catalog.badgeName(jobId)))
                       .arg(level);
    bool hasAwakening = false;
    if (parts.size() > 1) {
      const QStringList exclusive = parts.at(1).split(QLatin1Char(':'));
      const int exclusiveId = exclusive.value(0).toInt();
      const bool awakened = exclusive.value(1).toInt() > 0;
      if (exclusiveId > 0) {
        hasAwakening = true;
        line += QStringLiteral("<br><span class='%1'>%2 · %3</span>")
                    .arg(awakened ? QStringLiteral("ok") : QStringLiteral("muted"),
                         htmlText(catalog.badgeName(exclusiveId)),
                         awakened ? QStringLiteral("已觉醒") : QStringLiteral("未觉醒"));
      }
    }
    if (!hasAwakening)
      line += QStringLiteral("<br><span class='muted'>未觉醒</span>");
    badgeRows.append(detailRow(QStringLiteral("元魂 %1").arg(slotNumber++), line));
  }
  if (badgeRows.isEmpty())
    badgeRows.append(detailRow(QStringLiteral("状态"), QStringLiteral("未装备元魂")));
  return detailSection(QStringLiteral("元魂"), badgeRows.join(QString()));
}

QString sacredHtml(const QJsonObject& pet, const PetDetailCatalog& catalog) {
  const QString sequence = pet.value(QStringLiteral("shenjue")).toString();
  if (sequence.isEmpty())
    return detailSection(QStringLiteral("神源兽"),
                         detailRow(QStringLiteral("状态"), QStringLiteral("未装备神源兽")));
  const QStringList parts = sequence.split(QLatin1Char('|'));
  const QStringList define = parts.value(0).split(QLatin1Char('#'));
  const QStringList levels = parts.value(1).split(QLatin1Char(':'));
  const int defineId = define.value(0).toInt();
  const int starPlan = define.value(1).toInt();
  const int stagePlan = define.value(2).toInt();
  const int maxStar = PetDetailCatalog::sacredMaxStar(starPlan);
  const int maxStage = PetDetailCatalog::sacredMaxStage(stagePlan);
  int star = levels.value(0).toInt();
  int stage = levels.value(1).toInt();
  if (star <= 0 && maxStar > 0)
    star = maxStar;
  if (stage <= 0 && maxStage > 0)
    stage = maxStage;
  const bool fullStar = maxStar > 0 && star >= maxStar;
  const bool fullStage = maxStage > 0 && stage >= maxStage;
  const QString starState = maxStar > 0
                                ? QStringLiteral("%1/%2 星（%3）")
                                      .arg(star)
                                      .arg(maxStar)
                                      .arg(fullStar ? QStringLiteral("满星") :
                                                      QStringLiteral("未满星"))
                                : QStringLiteral("%1 星").arg(star);
  QString stageState = maxStage > 0
                           ? QStringLiteral("%1/%2 阶（%3）")
                                 .arg(stage)
                                 .arg(maxStage)
                                 .arg(fullStage ? QStringLiteral("满阶") : QStringLiteral("未满阶"))
                           : QStringLiteral("%1 阶").arg(stage);
  if (!fullStage && maxStage > stage)
    stageState += QStringLiteral("，还差 %1 阶").arg(maxStage - stage);
  QString rows = detailRow(QStringLiteral("名称"),
                           QStringLiteral("<b>%1</b>")
                               .arg(htmlText(catalog.sacredEquipmentName(defineId))));
  rows += detailRow(QStringLiteral("星级"), htmlText(starState));
  rows += detailRow(QStringLiteral("阶级"), htmlText(stageState));
  return detailSection(QStringLiteral("神源兽"), rows);
}

QString astrolabeHtml(const QJsonObject& pet, const PetDetailCatalog& catalog) {
  QStringList allStars;
  int selectedCount = 0;
  for (const QString& chain : splitSequence(pet.value(QStringLiteral("astrolabe")).toString(),
                                            QLatin1Char('|'))) {
    for (const QString& slot : splitSequence(chain, QLatin1Char('#'))) {
      const QStringList fields = slot.split(QLatin1Char(':'));
      const int defineId = fields.value(0).toInt();
      if (fields.size() < 3 || defineId <= 0) continue;
      const QJsonObject definition = catalog.astrolabe(defineId);
      QString name = htmlText(catalog.astrolabeName(defineId));
      if (definition.value(QStringLiteral("exclusive")).toBool()) {
        QStringList essenceCosts;
        for (const QString& material : splitSequence(
                 definition.value(QStringLiteral("lightUpCost")).toString(),
                 QLatin1Char('#'))) {
          const QStringList cost = material.split(QLatin1Char(':'));
          if (cost.size() >= 3 && cost.value(0).toInt() == 4 &&
              cost.value(2).toInt() > 0) {
            essenceCosts.append(QStringLiteral("专属精华 ×%1").arg(cost.value(2).toInt()));
          }
        }
        if (!essenceCosts.isEmpty())
          name += QStringLiteral(" <span class='muted'>（点亮需 %1）</span>")
                      .arg(essenceCosts.join(QStringLiteral("、")));
      }
      const bool selected = fields.at(2).toInt() > 0;
      if (selected) {
        ++selectedCount;
        allStars.append(QStringLiteral(
            "<span style='color:#159947;font-weight:700'>%1（已选）</span>").arg(name));
      } else {
        allStars.append(QStringLiteral("<span>%1</span>").arg(name));
      }
    }
  }
  const bool breakthrough = pet.value(QStringLiteral("astrolabebr")).toBool();
  QString rows = detailRow(QStringLiteral("全部星灵"),
                           allStars.isEmpty() ? QStringLiteral("没有星轮数据")
                                              : allStars.join(QStringLiteral("　")));
  rows += detailRow(QStringLiteral("已选数量"), QString::number(selectedCount));
  rows += detailRow(QStringLiteral("突破"),
                    breakthrough ? QStringLiteral("<span class='ok'>已突破</span>")
                                 : QStringLiteral("<span class='muted'>未突破</span>"));
  return detailSection(QStringLiteral("天迹星轮"), rows);
}

QString stargodHtml(const QJsonObject& pet, const PetDetailCatalog& catalog) {
  struct StarEntry {
    int defineId = 0;
    int level = 0;
    int sourceId = -1;
    QString name;
    QString sourceName;
    int quality = 0;
    bool changeable = false;
  };
  QList<StarEntry> ordinary;
  QList<StarEntry> changeable;
  for (const QString& slot : splitSequence(pet.value(QStringLiteral("sgs")).toString(),
                                           QLatin1Char('#'))) {
    const QStringList fields = slot.split(QLatin1Char(':'));
    const int defineId = fields.value(0).toInt();
    if (defineId <= 0) continue;
    const int sourceId = fields.size() >= 3 ? fields.value(2).toInt() : -1;
    const QJsonObject item = catalog.stargod(defineId);
    const QJsonObject source = sourceId > 0 ? catalog.stargod(sourceId) : QJsonObject();
    StarEntry entry;
    entry.defineId = defineId;
    entry.level = fields.value(1).toInt();
    entry.sourceId = sourceId;
    entry.name = item.value(QStringLiteral("name"))
                     .toString(QStringLiteral("星神 %1").arg(defineId));
    entry.sourceName = source.value(QStringLiteral("name"))
                           .toString(QStringLiteral("万变星神"));
    entry.changeable = item.value(QStringLiteral("changeable")).toBool() ||
                       source.value(QStringLiteral("changeable")).toBool();
    int quality = item.value(QStringLiteral("quality")).toInt();
    if (entry.changeable && source.contains(QStringLiteral("quality")))
      quality = source.value(QStringLiteral("quality")).toInt(quality);
    entry.quality = quality;
    (entry.changeable ? changeable : ordinary).append(entry);
  }

  auto render = [](const StarEntry& entry, bool showBase) {
    const QString color = entry.quality == 6 ? QStringLiteral("#d63b3b")
                          : entry.quality == 5 ? QStringLiteral("#c58b00")
                                               : QStringLiteral("#4b5563");
    if (showBase) {
      return QStringLiteral(
                 "<span class='star' style='color:%1'><u>%2</u> · 当前变为 %3 · Lv.%4</span>")
          .arg(color, htmlText(entry.sourceName), htmlText(entry.name))
          .arg(entry.level);
    }
    return QStringLiteral("<span class='star' style='color:%1'>%2 · Lv.%3</span>")
        .arg(color, htmlText(entry.name))
        .arg(entry.level);
  };

  auto category = [](const StarEntry& entry) {
    static const QStringList functionalNames = {
        QStringLiteral("如有神助"), QStringLiteral("顺应天命"),
        QStringLiteral("天煞孤星"), QStringLiteral("气贯星河")};
    static const QStringList attackNames = {QStringLiteral("乘胜追击")};
    static const QStringList defenseNames = {QStringLiteral("福虎佑灵")};
    for (const QString& name : functionalNames)
      if (entry.name.contains(name)) return QStringLiteral("功能性");
    for (const QString& name : attackNames)
      if (entry.name.contains(name)) return QStringLiteral("进攻");
    for (const QString& name : defenseNames)
      if (entry.name.contains(name)) return QStringLiteral("防御");
    static const QSet<int> defenseIds = {
        10, 15, 16, 17, 18, 19, 21, 26, 34, 35, 37, 38, 39, 40,
        46, 47, 48, 49, 54, 55, 57, 60, 63, 64, 70, 72, 73, 74,
        76, 85, 86};
    static const QSet<int> functionalIds = {11, 20, 22, 29, 33, 45, 67, 68, 81, 84, 88};
    if (defenseIds.contains(entry.defineId)) return QStringLiteral("防御");
    if (functionalIds.contains(entry.defineId)) return QStringLiteral("功能性");
    const QString name = entry.name;
    if (name.contains(QStringLiteral("盾")) || name.contains(QStringLiteral("防")) ||
        name.contains(QStringLiteral("护")) || name.contains(QStringLiteral("躯")) ||
        name.contains(QStringLiteral("霸体")) || name.contains(QStringLiteral("闪")))
      return QStringLiteral("防御");
    if (name.contains(QStringLiteral("人品")) || name.contains(QStringLiteral("命中")) ||
        name.contains(QStringLiteral("幸运")) || name.contains(QStringLiteral("追击")) ||
        name.contains(QStringLiteral("连打")))
      return QStringLiteral("功能性");
    return QStringLiteral("进攻");
  };

  QString rows = detailRow(
      QStringLiteral("固定万变"),
      changeable.isEmpty()
          ? QStringLiteral("<span class='muted'><u>固定万变栏位</u>：未装备</span>")
          : [&]() {
              QStringList values;
              for (const StarEntry& entry : changeable) values.append(render(entry, true));
              return values.join(QStringLiteral("<br>"));
            }());

  QHash<QString, QStringList> grouped;
  for (const StarEntry& entry : ordinary)
    grouped[category(entry)].append(render(entry, false));
  for (const QString& group : {QStringLiteral("进攻"), QStringLiteral("防御"),
                               QStringLiteral("功能性")}) {
    rows += detailRow(group, grouped.value(group).isEmpty()
                                 ? QStringLiteral("—")
                                 : grouped.value(group).join(QStringLiteral("<br>")));
  }
  const int ordinaryCount = static_cast<int>(ordinary.size());
  const int missing = qMax(0, 7 - ordinaryCount);
  rows += detailRow(QStringLiteral("普通栏位"),
                    QStringLiteral("已装备 %1 / 7%2")
                        .arg(qMin(ordinaryCount, 7))
                        .arg(missing > 0 ? QStringLiteral("，还缺 %1 个").arg(missing)
                                         : QStringLiteral("，已装满")));
  return detailSection(QStringLiteral("星神"), rows);
}

}  // namespace

PetWindow::PetWindow(PetRepository* repository, QWidget* parent)
    : QDialog(parent), repository_(repository) {
  imageCache_ = new PetImageCache(repository_->dataRoot(), this);
  setObjectName(QStringLiteral("KQPetInventoryWindow"));
  setWindowTitle(QStringLiteral("原版氪奇 · 精灵背包与仓库"));
  resize(1560, 820);
  setMinimumSize(1280, 700);
  setAttribute(Qt::WA_DeleteOnClose, false);

  auto* root = new QVBoxLayout(this);
  auto* toolbar = new QHBoxLayout();
  search_ = new QLineEdit(this);
  search_->setPlaceholderText(QStringLiteral("搜索名称、实例 ID 或种族 ID"));
  refresh_ = new QPushButton(QStringLiteral("刷新背包/仓库"), this);
  refreshDetails_ = new QPushButton(QStringLiteral("刷新仓库详情"), this);
  pauseDetails_ = new QPushButton(QStringLiteral("暂停详情刷新"), this);
  cancelDetails_ = new QPushButton(QStringLiteral("取消详情刷新"), this);
  settings_ = new QPushButton(QStringLiteral("设置"), this);
  pauseDetails_->setEnabled(false);
  cancelDetails_->setEnabled(false);
  toolbar->addWidget(new QLabel(QStringLiteral("精灵查询"), this));
  toolbar->addWidget(search_, 1);
  toolbar->addWidget(refresh_);
  toolbar->addWidget(refreshDetails_);
  toolbar->addWidget(pauseDetails_);
  toolbar->addWidget(cancelDetails_);
  toolbar->addWidget(settings_);
  root->addLayout(toolbar);

  auto* filterBar = new QHBoxLayout();
  attributeFilter_ = new QComboBox(this);
  jobFilter_ = new QComboBox(this);
  eraFilter_ = new QComboBox(this);
  auto* resetFilters = new QPushButton(QStringLiteral("重置查询/筛选"), this);
  attributeFilter_->setMinimumWidth(115);
  jobFilter_->setMinimumWidth(145);
  eraFilter_->setMinimumWidth(105);
  filterBar->addWidget(new QLabel(QStringLiteral("属性"), this));
  filterBar->addWidget(attributeFilter_);
  filterBar->addWidget(new QLabel(QStringLiteral("职业"), this));
  filterBar->addWidget(jobFilter_);
  filterBar->addWidget(new QLabel(QStringLiteral("时代"), this));
  filterBar->addWidget(eraFilter_);
  filterBar->addWidget(resetFilters);
  filterBar->addStretch(1);
  root->addLayout(filterBar);

  progress_ = new QLabel(QStringLiteral("仓库详情刷新：未启动"), this);
  progress_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  root->addWidget(progress_);

  auto* splitter = new QSplitter(Qt::Horizontal, this);
  auto* listSplitter = new QSplitter(Qt::Vertical, splitter);

  auto* backpackPane = new QWidget(listSplitter);
  auto* backpackLayout = new QVBoxLayout(backpackPane);
  backpackLayout->setContentsMargins(0, 0, 0, 0);
  backpackLayout->setSpacing(3);
  auto* backpackControls = new QHBoxLayout();
  backpackTitle_ = new QLabel(QStringLiteral("背包"), backpackPane);
  backpackTitle_->setStyleSheet(QStringLiteral("font-weight:600;"));
  backpackSort_ = new QComboBox(backpackPane);
  backpackSortDirection_ = new QComboBox(backpackPane);
  addSortChoices(backpackSort_);
  backpackSortDirection_->addItem(QStringLiteral("正序"), true);
  backpackSortDirection_->addItem(QStringLiteral("倒序"), false);
  backpackControls->addWidget(backpackTitle_);
  backpackControls->addStretch(1);
  backpackControls->addWidget(new QLabel(QStringLiteral("背包排序"), backpackPane));
  backpackControls->addWidget(backpackSort_);
  backpackControls->addWidget(backpackSortDirection_);
  backpackLayout->addLayout(backpackControls);
  backpackTable_ = makeTable(backpackPane);
  backpackTable_->setMinimumHeight(220);
  backpackLayout->addWidget(backpackTable_, 1);
  backpackPages_ = new QHBoxLayout();
  backpackPages_->setSpacing(4);
  backpackPages_->addStretch(1);
  backpackLayout->addLayout(backpackPages_);

  auto* warehousePane = new QWidget(listSplitter);
  auto* warehouseLayout = new QVBoxLayout(warehousePane);
  warehouseLayout->setContentsMargins(0, 0, 0, 0);
  warehouseLayout->setSpacing(3);
  auto* warehouseControls = new QHBoxLayout();
  auto* warehouseTitle = new QLabel(QStringLiteral("仓库"), warehousePane);
  warehouseTitle->setStyleSheet(QStringLiteral("font-weight:600;"));
  warehouseSort_ = new QComboBox(warehousePane);
  warehouseSortDirection_ = new QComboBox(warehousePane);
  addSortChoices(warehouseSort_);
  warehouseSortDirection_->addItem(QStringLiteral("正序"), true);
  warehouseSortDirection_->addItem(QStringLiteral("倒序"), false);
  warehouseControls->addWidget(warehouseTitle);
  warehouseControls->addStretch(1);
  warehouseControls->addWidget(new QLabel(QStringLiteral("仓库排序"), warehousePane));
  warehouseControls->addWidget(warehouseSort_);
  warehouseControls->addWidget(warehouseSortDirection_);
  warehouseLayout->addLayout(warehouseControls);
  warehouseTabs_ = new QTabWidget(warehousePane);
  warehouseTable_ = makeTable(warehouseTabs_);
  eliteWarehouseTable_ = makeTable(warehouseTabs_);
  warehouseTabs_->addTab(warehouseTable_, QStringLiteral("普通仓库"));
  warehouseTabs_->addTab(eliteWarehouseTable_, QStringLiteral("精英仓库"));
  warehouseLayout->addWidget(warehouseTabs_, 1);

  for (QTableWidget* table : {backpackTable_, warehouseTable_, eliteWarehouseTable_})
    table->setItemDelegateForColumn(0, new SearchHighlightDelegate(search_, table));

  listSplitter->addWidget(backpackPane);
  listSplitter->addWidget(warehousePane);
  listSplitter->setChildrenCollapsible(false);
  listSplitter->setStretchFactor(0, 1);
  listSplitter->setStretchFactor(1, 1);
  listSplitter->setSizes({330, 330});

  detailTabs_ = new QTabWidget(splitter);
  detailView_ = new QTextBrowser(detailTabs_);
  detailView_->setOpenExternalLinks(false);
  detailView_->setPlaceholderText(QStringLiteral("请选择一只精灵"));
  rawTree_ = new QTreeWidget(detailTabs_);
  rawTree_->setColumnCount(2);
  rawTree_->setHeaderLabels({QStringLiteral("原始字段"), QStringLiteral("值")});
  rawTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  rawTree_->header()->setStretchLastSection(true);
  rawTree_->setAlternatingRowColors(true);
  detailTabs_->addTab(detailView_, QStringLiteral("精灵详情"));
  detailTabs_->addTab(rawTree_, QStringLiteral("原始数据"));
  splitter->addWidget(listSplitter);
  splitter->addWidget(detailTabs_);
  splitter->setChildrenCollapsible(false);
  splitter->setStretchFactor(0, 8);
  splitter->setStretchFactor(1, 4);
  splitter->setSizes({1020, 520});
  root->addWidget(splitter, 1);

  status_ = new QLabel(this);
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  root->addWidget(status_);

  connect(refresh_, &QPushButton::clicked, this, &PetWindow::listRefreshRequested);
  connect(refreshDetails_, &QPushButton::clicked, this,
          &PetWindow::warehouseDetailRefreshRequested);
  connect(pauseDetails_, &QPushButton::clicked, this, [this]() {
    if (detailBatchPaused_)
      emit warehouseDetailResumeRequested();
    else
      emit warehouseDetailPauseRequested();
  });
  connect(cancelDetails_, &QPushButton::clicked, this,
          &PetWindow::warehouseDetailCancelRequested);
  connect(settings_, &QPushButton::clicked, this, &PetWindow::settingsRequested);
  searchDebounce_ = new QTimer(this);
  searchDebounce_->setSingleShot(true);
  searchDebounce_->setInterval(180);
  connect(searchDebounce_, &QTimer::timeout, this, &PetWindow::applyFilter);
  connect(search_, &QLineEdit::textChanged, this, [this]() {
    searchDebounce_->start();
    for (QTableWidget* table : {backpackTable_, warehouseTable_, eliteWarehouseTable_})
      table->viewport()->update();
  });
  for (QComboBox* filter : {attributeFilter_, jobFilter_, eraFilter_})
    connect(filter, &QComboBox::currentIndexChanged, this, &PetWindow::applyFilter);
  for (QComboBox* sort : {backpackSort_, backpackSortDirection_, warehouseSort_,
                          warehouseSortDirection_})
    connect(sort, &QComboBox::currentIndexChanged, this, &PetWindow::applyFilter);
  connect(backpackSort_, &QComboBox::currentIndexChanged, this,
          &PetWindow::updateSortDirectionState);
  connect(warehouseSort_, &QComboBox::currentIndexChanged, this,
          &PetWindow::updateSortDirectionState);
  connect(resetFilters, &QPushButton::clicked, this, [this]() {
    const QSignalBlocker searchBlocker(search_);
    const QSignalBlocker attributeBlocker(attributeFilter_);
    const QSignalBlocker jobBlocker(jobFilter_);
    const QSignalBlocker eraBlocker(eraFilter_);
    search_->clear();
    attributeFilter_->setCurrentIndex(0);
    jobFilter_->setCurrentIndex(0);
    eraFilter_->setCurrentIndex(0);
    backpackPage_ = 0;
    refreshViews();
  });
  connect(backpackTable_, &QTableWidget::cellClicked, this, &PetWindow::selectBackpack);
  connect(warehouseTable_, &QTableWidget::cellClicked, this, &PetWindow::selectWarehouse);
  connect(eliteWarehouseTable_, &QTableWidget::cellClicked, this,
          &PetWindow::selectWarehouse);
  connect(repository_, &PetRepository::dataChanged, this, &PetWindow::rebuild);
  connect(repository_, &PetRepository::detailChanged, this, &PetWindow::updateCurrentDetail);
  connect(repository_, &PetRepository::statusChanged, this, &PetWindow::setStatus);
  connect(imageCache_, &PetImageCache::petImageReady, this, &PetWindow::updateCurrentImage);

  updateSortDirectionState();
  rebuild();
  if (repository_->updatedAt().isValid()) {
    setStatus(QStringLiteral("已读取账号 %1 的本地缓存（%2），正在等待线上刷新。缓存：%3")
                  .arg(repository_->accountKey(),
                       repository_->updatedAt().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                       repository_->cachePath()));
  } else {
    setStatus(QStringLiteral("尚无本地缓存；登录游戏后会自动读取。"));
  }
}

void PetWindow::setStatus(const QString& status) { status_->setText(status); }

void PetWindow::setListRefreshRunning(bool running) {
  refresh_->setEnabled(!running);
  refresh_->setText(running ? QStringLiteral("列表刷新中……")
                            : QStringLiteral("刷新背包/仓库"));
}

void PetWindow::setDetailProgress(bool running, bool paused, int completed, int total,
                                  int succeeded, int failed, qint64 currentInstanceId,
                                  int estimatedSeconds) {
  detailBatchPaused_ = paused;
  refreshDetails_->setEnabled(!running);
  pauseDetails_->setEnabled(running);
  cancelDetails_->setEnabled(running);
  pauseDetails_->setText(paused ? QStringLiteral("继续详情刷新")
                                : QStringLiteral("暂停详情刷新"));
  if (!running && total <= 0) {
    progress_->setText(QStringLiteral("仓库详情刷新：未启动"));
    return;
  }
  const QString current = currentInstanceId > 0 ? QString::number(currentInstanceId)
                                                 : QStringLiteral("等待中");
  progress_->setText(QStringLiteral("仓库详情：%1/%2　成功 %3　失败 %4　当前 %5　预计剩余 %6 秒%7")
                         .arg(completed).arg(total).arg(succeeded).arg(failed)
                         .arg(current).arg(estimatedSeconds)
                         .arg(paused ? QStringLiteral("　【已暂停】") : QString()));
}

QString PetWindow::displayName(const QJsonObject& pet) const {
  const QString custom = pet.value(QStringLiteral("customName")).toString();
  if (!custom.isEmpty())
    return custom;
  const QString name = pet.value(QStringLiteral("n")).toString();
  return name.isEmpty() ? QStringLiteral("未命名") : name;
}

void PetWindow::fillTable(QTableWidget* table, const QList<QJsonObject>& pets,
                          const QString& location) {
  const QSignalBlocker blocker(table);
  table->setUpdatesEnabled(false);
  table->setSortingEnabled(false);
  table->setRowCount(pets.size());
  int row = 0;
  for (const QJsonObject& pet : pets) fillRow(table, row++, pet, location);
  const QFontMetrics metrics(table->font());
  int nameWidth = metrics.horizontalAdvance(QStringLiteral("名称")) + 48;
  int jobWidth = metrics.horizontalAdvance(QStringLiteral("职业")) + 24;
  int powerWidth = metrics.horizontalAdvance(QStringLiteral("战斗力 / 极限战斗力")) + 24;
  for (const QJsonObject& pet : pets) {
    const QJsonObject metadata = PetDetailCatalog::instance().pet(petRaceId(pet));
    nameWidth = qMax(nameWidth, metrics.horizontalAdvance(displayName(pet)) + 48);
    jobWidth = qMax(jobWidth, metrics.horizontalAdvance(PetDetailCatalog::instance().jobs(
                                     metadata.value(QStringLiteral("jobs")).toString())) + 24);
    powerWidth = qMax(powerWidth, metrics.horizontalAdvance(battlePowerTableText(
                                         battlePowerState(pet, PetDetailCatalog::instance()))) + 24);
  }
  table->setColumnWidth(0, qBound(205, nameWidth, 285));
  table->setColumnWidth(2, qBound(105, jobWidth, 175));
  table->setColumnWidth(5, qBound(185, powerWidth, 225));
  table->setUpdatesEnabled(true);
  table->viewport()->update();
}

void PetWindow::fillRow(QTableWidget* table, int row, const QJsonObject& pet,
                        const QString& location) {
  const PetDetailCatalog& catalog = PetDetailCatalog::instance();
  const qint64 id = petInstanceId(pet);
  const int raceId = petRaceId(pet);
  const QJsonObject metadata = catalog.pet(raceId);
  const QString attributesSequence = metadata.value(QStringLiteral("attributes")).toString();
  const BattlePowerState power = battlePowerState(pet, catalog);
  const QStringList values = {
      displayName(pet), catalog.attributes(attributesSequence),
      catalog.jobs(metadata.value(QStringLiteral("jobs")).toString()),
      eraForPet(pet), pet.value(QStringLiteral("lv")).toVariant().toString(),
      battlePowerTableText(power), gridPositionText(pet, location)};
  const QDateTime cachedAt = repository_->detailSavedAt(id);
  for (int column = 0; column < values.size(); ++column) {
    QTableWidgetItem* item = table->item(row, column);
    if (!item) {
      item = new QTableWidgetItem();
      table->setItem(row, column, item);
    }
    item->setText(values.at(column));
    item->setData(Qt::UserRole, id);
    item->setData(Qt::UserRole + 1, QStringLiteral("%1 %2").arg(id).arg(raceId));
    if (column == 0) {
      const QString original = catalog.originalName(raceId);
      item->setToolTip(original.isEmpty() || original == values.at(0)
                           ? values.at(0)
                           : QStringLiteral("皮肤/当前名称：%1\n原名：%2")
                                 .arg(values.at(0), original));
    }
    if (column == 1) item->setIcon(imageCache_->attributeIcon(attributesSequence));
    if (column == 5) {
      if (location == QStringLiteral("warehouse")) {
        item->setToolTip(cachedAt.isValid()
                             ? QStringLiteral("战力来自本地详情缓存：%1")
                                   .arg(cachedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                             : QStringLiteral("尚无该实例ID的本地详情缓存"));
      } else {
        item->setToolTip(QStringLiteral("战力来自本次背包完整数据"));
      }
    }
  }
}

void PetWindow::updateWarehouseRow(qint64 instanceId) {
  const QJsonObject pet = repository_->warehousePet(instanceId);
  if (pet.isEmpty()) return;
  for (QTableWidget* table : {warehouseTable_, eliteWarehouseTable_}) {
    table->setSortingEnabled(false);
    for (int row = 0; row < table->rowCount(); ++row) {
      if (rowId(table, row) == instanceId) {
        fillRow(table, row, pet, QStringLiteral("warehouse"));
        break;
      }
    }
  }
}

void PetWindow::rebuild() {
  refreshViews(true);
  if (currentId_ > 0)
    showDetail(repository_->detailFor(currentId_));
}

void PetWindow::applyFilter() {
  backpackPage_ = 0;
  updateSortDirectionState();
  refreshViews();
}

void PetWindow::updateSortDirectionState() {
  backpackSortDirection_->setEnabled(
      backpackSort_->currentData().toInt() != static_cast<int>(PetSortMode::Default));
  warehouseSortDirection_->setEnabled(
      warehouseSort_->currentData().toInt() != static_cast<int>(PetSortMode::Default));
}

bool PetWindow::matchesCurrentQueryAndFilters(const QJsonObject& pet) const {
  const PetDetailCatalog& catalog = PetDetailCatalog::instance();
  const int raceId = petRaceId(pet);
  const QJsonObject metadata = catalog.pet(raceId);
  const QStringList names = {
      displayName(pet), pet.value(QStringLiteral("n")).toString(),
      catalog.petName(raceId), catalog.originalName(raceId)};
  const QStringList identifiers = {QString::number(petInstanceId(pet)),
                                   QString::number(raceId)};
  if (!petQueryMatches(search_->text(), names, identifiers)) return false;

  const QString selectedAttribute = attributeFilter_->currentText();
  if (attributeFilter_->currentIndex() > 0) {
    const QString attributes =
        catalog.attributes(metadata.value(QStringLiteral("attributes")).toString());
    if (!categoryParts(attributes).contains(selectedAttribute)) return false;
  }

  const QString selectedJob = jobFilter_->currentText();
  if (jobFilter_->currentIndex() > 0) {
    const QString jobs = catalog.jobs(metadata.value(QStringLiteral("jobs")).toString());
    if (!categoryParts(jobs).contains(selectedJob)) return false;
  }

  if (eraFilter_->currentIndex() > 0 && eraForPet(pet) != eraFilter_->currentText())
    return false;
  return true;
}

QList<QJsonObject> PetWindow::filteredAndSorted(const QList<QJsonObject>& pets,
                                                bool backpack) const {
  QList<QJsonObject> result;
  result.reserve(pets.size());
  for (const QJsonObject& pet : pets)
    if (matchesCurrentQueryAndFilters(pet)) result.append(pet);

  const QComboBox* sortBox = backpack ? backpackSort_ : warehouseSort_;
  const QComboBox* directionBox =
      backpack ? backpackSortDirection_ : warehouseSortDirection_;
  const auto mode = static_cast<PetSortMode>(sortBox->currentData().toInt());
  const bool ascending = mode == PetSortMode::Default
                             ? true
                             : directionBox->currentData().toBool();

  auto numericKey = [mode](const QJsonObject& pet, bool* valid) -> qint64 {
    *valid = true;
    switch (mode) {
      case PetSortMode::BattlePower:
        *valid = pet.contains(QStringLiteral("zdl"));
        return pet.value(QStringLiteral("zdl")).toVariant().toLongLong();
      case PetSortMode::ExtremePower:
        *valid = pet.contains(QStringLiteral("xzdl"));
        return pet.value(QStringLiteral("xzdl")).toVariant().toLongLong();
      case PetSortMode::CatalogSequence:
        *valid = petRaceId(pet) > 0;
        return petRaceId(pet);
      case PetSortMode::ObtainedAt: {
        const QJsonValue value = pet.value(QStringLiteral("gd"));
        qint64 timestamp = value.toVariant().toLongLong();
        if (timestamp <= 0 && value.isString())
          timestamp = QDateTime::fromString(value.toString(), Qt::ISODate).toMSecsSinceEpoch();
        *valid = timestamp > 0;
        return timestamp;
      }
      case PetSortMode::Default:
        return pet.value(QStringLiteral("_position")).toInt(INT_MAX);
    }
    return 0;
  };

  std::stable_sort(result.begin(), result.end(), [&](const QJsonObject& left,
                                                     const QJsonObject& right) {
    bool leftValid = false;
    bool rightValid = false;
    const qint64 leftKey = numericKey(left, &leftValid);
    const qint64 rightKey = numericKey(right, &rightValid);
    if (leftValid != rightValid) return leftValid;
    if (leftValid && leftKey != rightKey)
      return ascending ? leftKey < rightKey : leftKey > rightKey;
    const int leftPosition = left.value(QStringLiteral("_position")).toInt(INT_MAX);
    const int rightPosition = right.value(QStringLiteral("_position")).toInt(INT_MAX);
    if (leftPosition != rightPosition) return leftPosition < rightPosition;
    return petInstanceId(left) < petInstanceId(right);
  });
  return result;
}

void PetWindow::rebuildFilterChoices() {
  QSet<QString> attributes;
  QSet<QString> jobs;
  QSet<QString> dualCareerJobs;
  QSet<QString> eras;
  QList<QJsonObject> all = repository_->backpackPets();
  all.append(repository_->warehousePets());
  const PetDetailCatalog& catalog = PetDetailCatalog::instance();
  for (const QJsonObject& pet : all) {
    const QJsonObject metadata = catalog.pet(petRaceId(pet));
    for (const QString& value : categoryParts(catalog.attributes(
             metadata.value(QStringLiteral("attributes")).toString())))
      attributes.insert(value);
    const QStringList petJobs = categoryParts(catalog.jobs(
        metadata.value(QStringLiteral("jobs")).toString()));
    for (const QString& value : petJobs)
      jobs.insert(value);
    if (petJobs.size() > 1)
      for (const QString& value : petJobs) dualCareerJobs.insert(value);
    eras.insert(eraForPet(pet));
  }

  auto refill = [](QComboBox* box, const QStringList& values) {
    const QString selected = box->currentIndex() > 0 ? box->currentText() : QString();
    const QSignalBlocker blocker(box);
    box->clear();
    box->addItem(QStringLiteral("全部"));
    box->addItems(values);
    const int selectedIndex = selected.isEmpty() ? 0 : box->findText(selected);
    box->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
  };

  QStringList orderedAttributes = attributes.values();
  std::sort(orderedAttributes.begin(), orderedAttributes.end(),
            [](const QString& left, const QString& right) {
              const bool leftDivine = left.startsWith(QStringLiteral("神"));
              const bool rightDivine = right.startsWith(QStringLiteral("神"));
              if (leftDivine != rightDivine) return leftDivine;
              return QString::localeAwareCompare(left, right) < 0;
            });

  static const QStringList preferredJobs = {
      QStringLiteral("神速"), QStringLiteral("神平衡"), QStringLiteral("神盾"),
      QStringLiteral("神攻"), QStringLiteral("元素师"), QStringLiteral("神召唤师"),
      QStringLiteral("通灵师"), QStringLiteral("赋能师"), QStringLiteral("幻元师")};
  QStringList orderedJobs = jobs.values();
  std::sort(orderedJobs.begin(), orderedJobs.end(),
            [&](const QString& left, const QString& right) {
              const int leftPreferred = preferredJobs.indexOf(left);
              const int rightPreferred = preferredJobs.indexOf(right);
              if ((leftPreferred >= 0) != (rightPreferred >= 0)) return leftPreferred >= 0;
              if (leftPreferred >= 0 && leftPreferred != rightPreferred)
                return leftPreferred < rightPreferred;
              const bool leftDual = dualCareerJobs.contains(left);
              const bool rightDual = dualCareerJobs.contains(right);
              if (leftDual != rightDual) return leftDual;
              return QString::localeAwareCompare(left, right) < 0;
            });

  QStringList orderedEras = eras.values();
  std::sort(orderedEras.begin(), orderedEras.end(),
            [](const QString& left, const QString& right) {
              if ((left == QStringLiteral("其它")) != (right == QStringLiteral("其它")))
                return right == QStringLiteral("其它");
              return QString::localeAwareCompare(left, right) < 0;
            });
  refill(attributeFilter_, orderedAttributes);
  refill(jobFilter_, orderedJobs);
  refill(eraFilter_, orderedEras);
}

void PetWindow::refreshViews(bool rebuildChoices) {
  if (rebuildChoices) rebuildFilterChoices();
  const QList<QJsonObject> allBackpack = repository_->backpackPets();
  QList<QJsonObject> ordinaryAll;
  QList<QJsonObject> eliteAll;
  for (const QJsonObject& pet : repository_->warehousePets()) {
    if (pet.value(QStringLiteral("_warehouseGroup")).toString() ==
        QStringLiteral("elite"))
      eliteAll.append(pet);
    else
      ordinaryAll.append(pet);
  }

  const QList<QJsonObject> backpack = filteredAndSorted(allBackpack, true);
  const QList<QJsonObject> ordinary = filteredAndSorted(ordinaryAll, false);
  const QList<QJsonObject> elite = filteredAndSorted(eliteAll, false);
  const int pageCount = qMax(1, (backpack.size() + 11) / 12);
  backpackPage_ = qBound(0, backpackPage_, pageCount - 1);
  fillTable(backpackTable_, backpack.mid(backpackPage_ * 12, 12),
            QStringLiteral("backpack"));
  fillTable(warehouseTable_, ordinary, QStringLiteral("warehouse"));
  fillTable(eliteWarehouseTable_, elite, QStringLiteral("warehouse"));
  rebuildPageButtons(pageCount);

  backpackTitle_->setText(
      QStringLiteral("背包：%1 / %2　第 %3/%4 页")
          .arg(backpack.size()).arg(allBackpack.size()).arg(backpackPage_ + 1).arg(pageCount));
  warehouseTabs_->setTabText(
      0, QStringLiteral("普通仓库（%1 / %2）").arg(ordinary.size()).arg(ordinaryAll.size()));
  warehouseTabs_->setTabText(
      1, QStringLiteral("精英仓库（%1 / %2）").arg(elite.size()).arg(eliteAll.size()));
}

void PetWindow::rebuildPageButtons(int pageCount) {
  while (QLayoutItem* item = backpackPages_->takeAt(0)) {
    if (item->widget()) item->widget()->deleteLater();
    delete item;
  }
  backpackPages_->addStretch(1);
  for (int page = 0; page < pageCount; ++page) {
    auto* button = new QPushButton(QString::number(page + 1), this);
    button->setFixedSize(32, 24);
    button->setCheckable(true);
    button->setChecked(page == backpackPage_);
    connect(button, &QPushButton::clicked, this, [this, page]() { setBackpackPage(page); });
    backpackPages_->addWidget(button);
  }
  backpackPages_->addStretch(1);
}

void PetWindow::setBackpackPage(int page) {
  if (page == backpackPage_) return;
  backpackPage_ = qMax(0, page);
  refreshViews();
}

qint64 PetWindow::rowId(QTableWidget* table, int row) {
  QTableWidgetItem* item = table->item(row, 0);
  return item ? item->data(Qt::UserRole).toLongLong() : 0;
}

void PetWindow::selectBackpack(int row, int) {
  currentId_ = rowId(backpackTable_, row);
  showDetail(repository_->detailFor(currentId_));
}

void PetWindow::selectWarehouse(int row, int) {
  auto* table = qobject_cast<QTableWidget*>(sender());
  if (!table)
    return;
  currentId_ = rowId(table, row);
  showDetail(repository_->detailFor(currentId_));
  if (currentId_ > 0) {
    const QDateTime cachedAt = repository_->detailSavedAt(currentId_);
    setStatus(cachedAt.isValid()
                  ? QStringLiteral("已显示实例 %1 的本地缓存（%2），正在获取最新详情……")
                        .arg(currentId_)
                        .arg(cachedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                  : QStringLiteral("实例 %1 尚无详情缓存，正在获取线上详情……")
                        .arg(currentId_));
    emit detailRequested(currentId_);
  }
}

void PetWindow::updateCurrentDetail(qint64 instanceId) {
  updateWarehouseRow(instanceId);
  if (instanceId == currentId_)
    showDetail(repository_->detailFor(instanceId));
}

void PetWindow::updateCurrentImage(const QString& visualKey, const QString&) {
  if (visualKey == currentVisualKey_ && currentId_ > 0)
    showDetail(repository_->detailFor(currentId_));
}

void PetWindow::addJsonValue(const QString& key, const QJsonValue& value,
                             QTreeWidgetItem* parent) {
  auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(rawTree_);
  item->setText(0, friendlyKey(key));
  item->setText(1, valueText(value));
  item->setToolTip(0, key);
  if (value.isObject()) {
    const QJsonObject object = value.toObject();
    for (auto iterator = object.begin(); iterator != object.end(); ++iterator)
      addJsonValue(iterator.key(), iterator.value(), item);
  } else if (value.isArray()) {
    const QJsonArray array = value.toArray();
    for (int index = 0; index < array.size(); ++index)
      addJsonValue(QStringLiteral("[%1]").arg(index), array.at(index), item);
  }
}

void PetWindow::showDetail(const QJsonObject& pet) {
  rawTree_->clear();
  if (pet.isEmpty()) {
    currentVisualKey_.clear();
    detailView_->setHtml(QStringLiteral("<p style='color:#6b7280'>请选择一只精灵</p>"));
    auto* item = new QTreeWidgetItem(rawTree_);
    item->setText(0, QStringLiteral("提示"));
    item->setText(1, QStringLiteral("请选择一只精灵"));
    return;
  }

  const PetDetailCatalog& catalog = PetDetailCatalog::instance();
  const int raceId = petRaceId(pet);
  currentRaceId_ = raceId;
  currentVisualKey_ = petVisualKey(pet);
  const QJsonObject metadata = catalog.pet(raceId);
  QString name = pet.value(QStringLiteral("n")).toString();
  if (name.isEmpty())
    name = catalog.petName(raceId);
  const QString originalName = catalog.originalName(raceId);
  const QString customName = pet.value(QStringLiteral("customName")).toString();
  const QString imagePath = imageCache_->ensurePetImage(
      pet, {name, displayName(pet), catalog.petName(raceId), catalog.originalName(raceId)});
  const QString imageHtml = imagePath.isEmpty()
                                ? QStringLiteral("<div class='image-placeholder'>图片首次显示后<br>自动缓存到本地</div>")
                                : QStringLiteral("<img class='pet-picture' src='%1' width='150'>")
                                      .arg(QUrl::fromLocalFile(imagePath).toString(QUrl::FullyEncoded));
  QString identityRows = detailRow(QStringLiteral("名称"),
                                   QStringLiteral("<b>%1</b>").arg(valueOrDash(name)));
  identityRows += detailRow(QStringLiteral("原名"), valueOrDash(originalName));
  if (!customName.isEmpty())
    identityRows += detailRow(QStringLiteral("自定义昵称"), htmlText(customName));
  identityRows += detailRow(QStringLiteral("属性"),
                            htmlText(catalog.attributes(
                                metadata.value(QStringLiteral("attributes")).toString())));
  identityRows += detailRow(QStringLiteral("职业"),
                            htmlText(catalog.jobs(
                                metadata.value(QStringLiteral("jobs")).toString())));
  identityRows += detailRow(QStringLiteral("时代"), htmlText(eraForPet(pet)));
  const BattlePowerState power = battlePowerState(pet, catalog);
  const QString currentPower = power.hasCurrent
                                   ? QStringLiteral("<b>%1</b>（%2）")
                                         .arg(power.current)
                                         .arg(power.isHighest ? QStringLiteral("已达最高战斗力")
                                                              : QStringLiteral("未达最高战斗力"))
                                   : QStringLiteral("—");
  const QString extremePower = power.hasExtreme
                                   ? QStringLiteral("<b>%1</b>（最高战斗力 %2）")
                                         .arg(power.extreme)
                                         .arg(power.highest)
                                   : QStringLiteral("—");
  identityRows += detailRow(QStringLiteral("战斗力"), currentPower);
  identityRows += detailRow(QStringLiteral("极限战斗力"), extremePower);

  const qint64 instanceId = pet.value(QStringLiteral("id")).toVariant().toLongLong();
  const QString mismatchWarning = pet.value(QStringLiteral("_visualMismatch")).toBool()
                                      ? QStringLiteral("<div class='warning'>检测到形态或皮肤变化，当前显示缓存战力，正在刷新最新详情。</div>")
                                      : QString();
  const QString header = QStringLiteral(
                             "<div class='hero'><div class='pet-name'>%1</div>"
                             "<div class='sub'>实例 %2　种族 %3　等级 %4</div>%5</div>")
                             .arg(valueOrDash(name))
                             .arg(instanceId)
                             .arg(raceId)
                             .arg(pet.value(QStringLiteral("lv")).toInt())
                             .arg(mismatchWarning);
  const QString body = identitySection(identityRows, imageHtml) +
                       talentHtml(pet) + relationshipHtml(pet, repository_) +
                       badgeHtml(pet, catalog) +
                       sacredHtml(pet, catalog) + astrolabeHtml(pet, catalog) +
                       stargodHtml(pet, catalog) + battlePowerAnalysisHtml(pet, catalog);
  const QString html = QStringLiteral(
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
  detailView_->setHtml(html);
  detailView_->verticalScrollBar()->setValue(0);

  const QStringList priority = {QStringLiteral("id"), QStringLiteral("n"),
                                QStringLiteral("customName"), QStringLiteral("r"),
                                QStringLiteral("ri"), QStringLiteral("lv"),
                                QStringLiteral("fr"), QStringLiteral("pr")};
  QSet<QString> inserted;
  for (const QString& key : priority) {
    if (pet.contains(key)) {
      addJsonValue(key, pet.value(key), nullptr);
      inserted.insert(key);
    }
  }
  for (auto iterator = pet.begin(); iterator != pet.end(); ++iterator) {
    if (!inserted.contains(iterator.key()))
      addJsonValue(iterator.key(), iterator.value(), nullptr);
  }
  rawTree_->expandToDepth(0);
}
