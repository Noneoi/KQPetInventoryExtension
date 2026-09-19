#include "ui/common/ui_preferences.h"

#include <QComboBox>
#include <QSettings>
#include <QSplitter>
#include <QStringList>
#include <QTabWidget>

#include <memory>

namespace UiPreferences {
namespace {

std::unique_ptr<QSettings>& settings() {
  static std::unique_ptr<QSettings> instance;
  return instance;
}

}  // namespace

void setFilePath(const QString& path) {
  auto& current = settings();
  if (current) current->sync();
  current.reset(path.isEmpty() ? nullptr : new QSettings(path, QSettings::IniFormat));
}

bool enabled() { return settings() != nullptr; }

QVariant value(const QString& key, const QVariant& fallback) {
  return settings() ? settings()->value(key, fallback) : fallback;
}

void setValue(const QString& key, const QVariant& value) {
  if (settings()) settings()->setValue(key, value);
}

void sync() {
  if (settings()) settings()->sync();
}

QList<int> intList(const QString& key) {
  QList<int> result;
  for (const QString& part : value(key).toString().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
    bool valid = false;
    const int number = part.trimmed().toInt(&valid);
    if (!valid || number < 0) return {};
    result.append(number);
  }
  return result;
}

void setIntList(const QString& key, const QList<int>& values) {
  QStringList parts;
  for (int number : values) parts.append(QString::number(number));
  setValue(key, parts.join(QLatin1Char(',')));
}

void bindComboBox(QComboBox* box, const QString& key) {
  if (!box || !enabled()) return;
  const QVariant saved = value(key);
  if (saved.isValid()) {
    // Item data is stable across wording changes; text is the fallback for
    // combo boxes that carry no data.
    int index = -1;
    for (int item = 0; item < box->count() && index < 0; ++item) {
      const QVariant data = box->itemData(item);
      if (data.isValid() ? data.toString() == saved.toString() : box->itemText(item) == saved.toString())
        index = item;
    }
    if (index >= 0) box->setCurrentIndex(index);
  }
  QObject::connect(box, &QComboBox::currentIndexChanged, box, [box, key](int index) {
    if (index < 0) return;
    const QVariant data = box->itemData(index);
    setValue(key, data.isValid() ? data.toString() : box->itemText(index));
  });
}

void bindTabWidget(QTabWidget* tabs, const QString& key) {
  if (!tabs || !enabled()) return;
  bool valid = false;
  const int saved = value(key).toInt(&valid);
  if (valid && saved >= 0 && saved < tabs->count()) tabs->setCurrentIndex(saved);
  QObject::connect(tabs, &QTabWidget::currentChanged, tabs, [key](int index) {
    if (index >= 0) setValue(key, index);
  });
}

void bindSplitter(QSplitter* splitter, const QString& key) {
  if (!splitter || !enabled()) return;
  const QList<int> saved = intList(key);
  if (saved.size() == splitter->count()) {
    int total = 0;
    for (int size : saved) total += size;
    if (total > 0) splitter->setSizes(saved);
  }
  // splitterMoved fires only for user drags, never for programmatic setSizes.
  QObject::connect(splitter, &QSplitter::splitterMoved, splitter, [splitter, key](int, int) {
    setIntList(key, splitter->sizes());
  });
}

}  // namespace UiPreferences
