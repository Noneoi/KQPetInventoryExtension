#include "cultivation_material_delegate.h"
#include "asset_analysis_model.h"

#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QStyle>
#include <QTableView>
#include <QTextDocument>
#include <cmath>

namespace {
void prepare(QTextDocument& document, const QStyleOptionViewItem& option,
             const QModelIndex& index, int width) {
  document.setDocumentMargin(0);
  document.setDefaultFont(option.font);
  auto textOption = document.defaultTextOption();
  textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
  document.setDefaultTextOption(textOption);
  document.setDefaultStyleSheet(QStringLiteral("td.category { color: %1; }").arg(
      option.state & QStyle::State_Selected ? option.palette.color(QPalette::HighlightedText).name() : QStringLiteral("#526b86")));
  document.setHtml(index.data(AssetAnalysisModel::MaterialsHtmlRole).toString());
  document.setTextWidth(qMax(40,width - 16));
}
}

CultivationMaterialDelegate::CultivationMaterialDelegate(QTableView* view)
    : QStyledItemDelegate(view), view_(view) {}

void CultivationMaterialDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                       const QModelIndex& index) const {
  auto styled = option;
  initStyleOption(&styled,index);
  styled.text.clear();
  view_->style()->drawControl(QStyle::CE_ItemViewItem,&styled,painter,view_);
  QTextDocument document;
  prepare(document,styled,index,option.rect.width());
  QAbstractTextDocumentLayout::PaintContext context;
  context.palette = styled.palette;
  if (styled.state & QStyle::State_Selected) {
    context.palette.setColor(QPalette::Text,styled.palette.color(QPalette::HighlightedText));
  }
  painter->save();
  painter->setClipRect(option.rect);
  painter->translate(option.rect.topLeft() + QPoint(8,5));
  document.documentLayout()->draw(painter,context);
  painter->restore();
}

QSize CultivationMaterialDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
  auto styled = option;
  initStyleOption(&styled,index);
  const int width = view_->columnWidth(index.column());
  QTextDocument document;
  prepare(document,styled,index,width);
  return {width,static_cast<int>(std::ceil(document.size().height())) + 10};
}
