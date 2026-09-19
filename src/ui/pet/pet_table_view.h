#pragma once

#include "pet_search.h"

#include <QStyledItemDelegate>

class QLineEdit;
class QTableView;
class QTableWidget;
class QWidget;

// Presentation shared by the pet list tables (backpack, warehouse, elite
// warehouse): selection/row styling, viewport-fitted column widths and the
// search-match highlight on the name columns.
namespace PetTableView {

// Paints the part of a name that matches the current search in bold red.
class SearchHighlightDelegate final : public QStyledItemDelegate {
public:
  SearchHighlightDelegate(const QLineEdit* search, QObject* parent)
      : QStyledItemDelegate(parent), search_(search) {}

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override;

private:
  const QLineEdit* search_ = nullptr;
  mutable QString cachedQueryText_;
  mutable PetSearchQuery cachedQuery_;
};

// Fits all columns into the table's own viewport; the power column keeps room
// for "99999 / 99999（至高）".
void resizeModelViewColumns(QTableView* table);
// Row selection, fixed row height, no editing, and columns refitted on resize.
void configurePetTable(QTableView* table);
// The backpack table (QTableWidget) with its fixed headers.
QTableWidget* makeTable(QWidget* parent, bool backpack = false);
QTableView* makeTableView(QWidget* parent);

}  // namespace PetTableView
