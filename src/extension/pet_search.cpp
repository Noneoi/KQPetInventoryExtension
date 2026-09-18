#include "pet_search.h"

#include <QHash>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

QChar initialForGbkCode(int code) {
  struct Boundary { int code; char initial; };
  static constexpr Boundary boundaries[] = {
      {45217, 'a'}, {45253, 'b'}, {45761, 'c'}, {46318, 'd'},
      {46826, 'e'}, {47010, 'f'}, {47297, 'g'}, {47614, 'h'},
      {48119, 'j'}, {49062, 'k'}, {49324, 'l'}, {49896, 'm'},
      {50371, 'n'}, {50614, 'o'}, {50622, 'p'}, {50906, 'q'},
      {51387, 'r'}, {51446, 's'}, {52218, 't'}, {52698, 'w'},
      {52980, 'x'}, {53689, 'y'}, {54481, 'z'}};
  QChar result;
  for (const Boundary& boundary : boundaries) {
    if (code < boundary.code) break;
    result = QLatin1Char(boundary.initial);
  }
  return code <= 55289 ? result : QChar();
}

QChar computePinyinInitial(QChar character) {
  // GBK range boundaries are only an approximation and misclassify a few
  // commonly used name characters. Keep explicit corrections deterministic
  // so searches and highlighted ranges use the same initial.
  static const QHash<QChar, QChar> overrides = {
      {QStringLiteral("黛").at(0), QLatin1Char('d')},
  };
  const auto corrected = overrides.constFind(character);
  if (corrected != overrides.constEnd()) return corrected.value();
#ifdef Q_OS_WIN
  const wchar_t source = character.unicode();
  char bytes[4] = {};
  const int count = WideCharToMultiByte(936, 0, &source, 1, bytes, 4,
                                        nullptr, nullptr);
  if (count == 2) {
    const int code = static_cast<unsigned char>(bytes[0]) * 256 +
                     static_cast<unsigned char>(bytes[1]);
    return initialForGbkCode(code);
  }
#else
  Q_UNUSED(character);
#endif
  return {};
}

// Every keystroke re-derives the initials of every visible name, and the same
// few hundred characters keep coming back. The cache is thread-local so it
// needs no lock and cannot be shared across threads by accident.
QChar pinyinInitial(QChar character) {
  thread_local QHash<char16_t, QChar> memo;
  const auto found = memo.constFind(character.unicode());
  if (found != memo.constEnd()) return found.value();
  const QChar initial = computePinyinInitial(character);
  memo.insert(character.unicode(), initial);
  return initial;
}

PetSearchText initialsWithPositions(const QString& text) {
  PetSearchText result;
  result.text = text;
  result.initials.reserve(text.size());
  result.positions.reserve(text.size());
  for (int index = 0; index < text.size(); ++index) {
    const QChar character = text.at(index);
    if (character.isLetterOrNumber() && character.unicode() < 128) {
      result.initials.append(character.toLower());
      result.positions.append(index);
      continue;
    }
    const QChar initial = pinyinInitial(character);
    if (!initial.isNull()) {
      result.initials.append(initial);
      result.positions.append(index);
    }
  }
  return result;
}

}  // namespace

PetSearchText preparePetSearchText(const QString& text) { return initialsWithPositions(text); }

PetSearchQuery preparePetSearchQuery(const QString& query) {
  PetSearchQuery result;
  result.needle = query.trimmed();
  result.lowerNeedle = result.needle.toLower();
  result.initialsAllowed = !result.needle.isEmpty();
  for (const QChar letter : result.needle)
    if (!((letter >= QLatin1Char('a') && letter <= QLatin1Char('z')) ||
          (letter >= QLatin1Char('A') && letter <= QLatin1Char('Z'))))
      result.initialsAllowed = false;
  return result;
}

PetSearchIndex preparePetSearchIndex(const QStringList& names, const QStringList& identifiers) {
  PetSearchIndex result;
  QStringList uniqueNames = names;
  uniqueNames.removeDuplicates();
  for (const QString& name : uniqueNames)
    if (!name.isEmpty()) result.names.append(preparePetSearchText(name));
  result.identifiers = identifiers;
  return result;
}

bool petQueryMatches(const PetSearchQuery& query, const PetSearchIndex& index) {
  if (query.needle.isEmpty()) return true;
  for (const PetSearchText& name : index.names)
    if (name.text.contains(query.needle, Qt::CaseInsensitive)) return true;
  for (const QString& identifier : index.identifiers)
    if (identifier.contains(query.needle, Qt::CaseInsensitive)) return true;
  if (!query.initialsAllowed) return false;
  for (const PetSearchText& name : index.names)
    if (name.initials.contains(query.lowerNeedle)) return true;
  return false;
}

QPair<int, int> petQueryHighlightRange(const PetSearchQuery& query, const PetSearchText& text) {
  if (query.needle.isEmpty() || text.text.isEmpty()) return {-1, 0};
  const int direct = text.text.indexOf(query.needle, 0, Qt::CaseInsensitive);
  if (direct >= 0) return {direct, query.needle.size()};
  if (!query.initialsAllowed) return {-1, 0};
  const int initialIndex = text.initials.indexOf(query.lowerNeedle);
  if (initialIndex < 0 || initialIndex + query.needle.size() > text.positions.size())
    return {-1, 0};
  const int begin = text.positions.at(initialIndex);
  const int end = text.positions.at(initialIndex + query.needle.size() - 1) + 1;
  return {begin, end - begin};
}

QString petPinyinInitials(const QString& text) { return preparePetSearchText(text).initials; }

bool petQueryMatches(const QString& query, const QStringList& names, const QStringList& identifiers) {
  return petQueryMatches(preparePetSearchQuery(query), preparePetSearchIndex(names, identifiers));
}

QPair<int, int> petQueryHighlightRange(const QString& query, const QString& text) {
  return petQueryHighlightRange(preparePetSearchQuery(query), preparePetSearchText(text));
}
