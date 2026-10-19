#pragma once
#include <QTableWidget>
#include <memory>

// Preserve the cell and its model index. Qt notifies views only for roles that
// changed; ordinary count/freshness updates do not destroy selection/editors.
inline QTableWidgetItem* updateTableItem(QTableWidget* table, int row, int column, QTableWidgetItem* value) {
  auto* existing = table->item(row, column);
  if (!existing) { table->setItem(row, column, value); return value; }
  std::unique_ptr<QTableWidgetItem> owned(value);
  for (int role : {Qt::DisplayRole, Qt::DecorationRole, Qt::ToolTipRole, Qt::TextAlignmentRole,
                   Qt::BackgroundRole, Qt::ForegroundRole, Qt::FontRole, Qt::CheckStateRole})
    if (existing->data(role) != value->data(role)) existing->setData(role, value->data(role));
  if (existing->flags() != value->flags()) existing->setFlags(value->flags());
  return existing;
}
