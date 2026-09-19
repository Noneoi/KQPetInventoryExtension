#include "pet_table_view.h"

#include "pet_table_model.h"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHeaderView>
#include <QLineEdit>
#include <QPainter>
#include <QStyle>
#include <QTableView>
#include <QTableWidget>
#include <QTextLayout>

#include <vector>

namespace PetTableView {

namespace {

class PetTableWidthFitter final : public QObject {
public:
  explicit PetTableWidthFitter(QTableView* table) : QObject(table), table_(table) {
    table->viewport()->installEventFilter(this);
  }
protected:
  bool eventFilter(QObject*, QEvent* event) override {
    if (event->type() == QEvent::Resize || event->type() == QEvent::Show)
      resizeModelViewColumns(table_);
    return false;
  }
private:
  QTableView* table_;
};

}  // namespace

void SearchHighlightDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const {
  if (index.column() > PetTableModel::OriginalNameColumn || !search_ || search_->text().trimmed().isEmpty()) {
    QStyledItemDelegate::paint(painter, option, index);
    return;
  }
  QStyleOptionViewItem styled(option);
  initStyleOption(&styled, index);
  const QString text = styled.text;
  if (cachedQueryText_ != search_->text()) {
    cachedQueryText_ = search_->text();
    cachedQuery_ = preparePetSearchQuery(cachedQueryText_);
  }
  const QVariant cached = index.data(PetTableModel::NameSearchIndexRole);
  const QPair<int, int> range = cached.canConvert<PetSearchText>()
      ? petQueryHighlightRange(cachedQuery_, cached.value<PetSearchText>())
      : petQueryHighlightRange(search_->text(), text);
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

void resizeModelViewColumns(QTableView* table) {
  if (!table || !table->model()) return;
  const int count = table->model()->columnCount();
  const int available = qMax(0, table->viewport()->width());
  if (!available || count <= 0) return;
  // Fit the entire table to its own viewport. Long names keep their full text
  // in the tooltip, while every field and heading stays on screen.
  const int backpackWeights[]{176,176,76,110,48,42,128,56,78};
  const int warehouseWeights[]{192,192,76,110,48,42,128,78};
  // A column count this function does not know about still has to be laid out:
  // an even split is not pretty, but it beats leaving default widths behind.
  const std::vector<int> evenWeights(std::size_t(count), 100);
  const int* weights = count == 9 ? backpackWeights
      : count == 8 ? warehouseWeights : evenWeights.data();
  int totalWeight = 0;
  for (int column=0; column<count; ++column) totalWeight += weights[column];
  const int powerColumn = count == 8 || count == 9 ? int(PetTableModel::BattlePowerColumn) : 0;
  const int powerWidth = qMin(qMax(available*weights[powerColumn]/totalWeight,
      table->fontMetrics().horizontalAdvance(QStringLiteral("99999 / 99999（至高）")) + 12),
      available/3);
  const int otherWidth = available-powerWidth;
  const int otherWeight = totalWeight-weights[powerColumn];
  int remaining = available;
  for (int column=0; column<count; ++column) {
    const int width = column == count-1 ? remaining : column == powerColumn ? powerWidth
        : otherWidth*weights[column]/otherWeight;
    table->setColumnWidth(column, width);
    remaining -= width;
  }
}

void configurePetTable(QTableView* table) {
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setStyleSheet(QStringLiteral(
      "QTableView::item:selected { background-color:#2563eb; color:#ffffff; }"
      "QTableView::item:selected:!active { background-color:#3b82f6; color:#ffffff; }"
      "QHeaderView::section { padding:4px 2px; }"));
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setAlternatingRowColors(true);
  table->setWordWrap(false);
  table->setTextElideMode(Qt::ElideRight);
  table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  table->setIconSize(QSize(20, 20));
  table->verticalHeader()->setVisible(false);
  table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
  table->verticalHeader()->setDefaultSectionSize(28);
  table->horizontalHeader()->setStretchLastSection(false);
  table->horizontalHeader()->setMinimumSectionSize(1);
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
  table->setMinimumWidth(0);
  new PetTableWidthFitter(table);
  resizeModelViewColumns(table);
}

QTableWidget* makeTable(QWidget* parent, bool backpack) {
  auto* table = new QTableWidget(parent);
  QStringList headers = {QStringLiteral("显示名称"), QStringLiteral("精灵原名"), QStringLiteral("属性"),
                         QStringLiteral("职业"), QStringLiteral("时代"),
                         QStringLiteral("等级"),
                         QStringLiteral("战斗力 /\n极限战斗力")};
  if (backpack) headers.append(QStringLiteral("是否\n出战"));
  headers.append(QStringLiteral("位置"));
  table->setColumnCount(headers.size());
  table->setHorizontalHeaderLabels(headers);
  configurePetTable(table);
  return table;
}

QTableView* makeTableView(QWidget* parent) {
  return new QTableView(parent);
}

}  // namespace PetTableView
