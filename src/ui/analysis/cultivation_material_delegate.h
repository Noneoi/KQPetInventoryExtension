#pragma once

#include <QStyledItemDelegate>

class QTableView;

class CultivationMaterialDelegate final : public QStyledItemDelegate {
public:
  explicit CultivationMaterialDelegate(QTableView* view);
  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
  QTableView* view_;
};
