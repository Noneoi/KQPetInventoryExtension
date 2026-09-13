#pragma once

#include <QString>
#include <QStringList>
#include <QPair>
#include <QMetaType>

struct PetSearchText {
  QString text;
  QString initials;
  QList<int> positions;
};
Q_DECLARE_METATYPE(PetSearchText)

struct PetSearchQuery {
  QString needle;
  QString lowerNeedle;
  bool initialsAllowed = false;
};

struct PetSearchIndex {
  QList<PetSearchText> names;
  QStringList identifiers;
};

PetSearchText preparePetSearchText(const QString& text);
PetSearchQuery preparePetSearchQuery(const QString& query);
PetSearchIndex preparePetSearchIndex(const QStringList& names,
                                    const QStringList& identifiers = {});
bool petQueryMatches(const PetSearchQuery& query, const PetSearchIndex& index);
QPair<int, int> petQueryHighlightRange(const PetSearchQuery& query,
                                      const PetSearchText& text);

QString petPinyinInitials(const QString& text);
bool petQueryMatches(const QString& query, const QStringList& names,
                     const QStringList& identifiers = {});
QPair<int, int> petQueryHighlightRange(const QString& query, const QString& text);
