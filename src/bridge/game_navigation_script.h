#pragma once

#include <QString>

namespace GameNavigationScript {

bool isAllowedLink(const QString& link);
// QCefView executes this as a function body, not as an expression. Keep the
// outer return so reportJavascriptResult receives the result of our IIFE.
QString build(const QString& link);

enum class Result { Submitted, NotReady, Failed, Unconfirmed };
Result classify(const QString& value);
QString message(const QString& value);

}  // namespace GameNavigationScript
