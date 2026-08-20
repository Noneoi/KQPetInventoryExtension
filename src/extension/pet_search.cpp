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

QChar pinyinInitial(QChar character) {
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

struct InitialsWithPositions {
  QString initials;
  QList<int> positions;
};

InitialsWithPositions initialsWithPositions(const QString& text) {
  InitialsWithPositions result;
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

QString petPinyinInitials(const QString& text) {
  return initialsWithPositions(text).initials;
}

bool petQueryMatches(const QString& query, const QStringList& names,
                     const QStringList& identifiers) {
  const QString needle = query.trimmed();
  if (needle.isEmpty()) return true;
  for (const QString& name : names)
    if (name.contains(needle, Qt::CaseInsensitive)) return true;
  for (const QString& identifier : identifiers)
    if (identifier.contains(needle, Qt::CaseInsensitive)) return true;

  static const QRegularExpression lettersOnly(QStringLiteral("^[A-Za-z]+$"));
  if (!lettersOnly.match(needle).hasMatch()) return false;
  const QString lowerNeedle = needle.toLower();
  for (const QString& name : names)
    if (petPinyinInitials(name).contains(lowerNeedle)) return true;
  return false;
}

QPair<int, int> petQueryHighlightRange(const QString& query, const QString& text) {
  const QString needle = query.trimmed();
  if (needle.isEmpty() || text.isEmpty()) return {-1, 0};
  const int direct = text.indexOf(needle, 0, Qt::CaseInsensitive);
  if (direct >= 0) return {direct, needle.size()};

  static const QRegularExpression lettersOnly(QStringLiteral("^[A-Za-z]+$"));
  if (!lettersOnly.match(needle).hasMatch()) return {-1, 0};
  const InitialsWithPositions mapped = initialsWithPositions(text);
  const int initialIndex = mapped.initials.indexOf(needle.toLower());
  if (initialIndex < 0 || initialIndex + needle.size() > mapped.positions.size())
    return {-1, 0};
  const int begin = mapped.positions.at(initialIndex);
  const int end = mapped.positions.at(initialIndex + needle.size() - 1) + 1;
  return {begin, end - begin};
}
