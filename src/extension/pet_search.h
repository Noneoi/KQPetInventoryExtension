#pragma once

#include <QString>
#include <QStringList>
#include <QPair>

QString petPinyinInitials(const QString& text);
bool petQueryMatches(const QString& query, const QStringList& names,
                     const QStringList& identifiers = {});
QPair<int, int> petQueryHighlightRange(const QString& query, const QString& text);
