#pragma once

#include <QRegularExpression>
#include <QString>

namespace GameNavigation {
inline bool isAllowedLink(const QString& link) {
  // Services are limited to opening a panel with bounded numeric arguments.
  // Arbitrary service methods, URLs and executable expressions are excluded.
  static const QRegularExpression allowed(QStringLiteral(
      "\\A(?:btnNewAct_[A-Za-z][A-Za-z0-9]{1,100}(?:_[A-Za-z0-9]{1,64}){1,8}|"
      "btnService_[A-Za-z][A-Za-z0-9]{1,100}_(?:show|open)[A-Z][A-Za-z0-9]{0,60}(?:_-?[0-9]{1,10}){0,6})\\z"));
  return link.size() <= 256 && allowed.match(link).hasMatch();
}
}  // namespace GameNavigation
