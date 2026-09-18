#pragma once
#include <QString>

// The detail browsers are QTextBrowsers, so every renderer emits one small
// self-contained document. The shared base style below is what keeps those
// documents looking like one product; each renderer appends only the rules
// that are actually specific to it.
namespace kqpet::html {

inline QString escaped(const QString& text) { return text.toHtmlEscaped(); }

// An unknown number is shown as a dash rather than as 0, because "not read
// yet" and "really zero" mean different things to the player.
inline QString number(bool known, int value) {
  return known ? QString::number(value) : QStringLiteral("—");
}

inline QString document(const QString& style, const QString& body) {
  return QStringLiteral("<html><head><style>"
      "body{font-family:'Microsoft YaHei UI','Segoe UI';font-size:12px;color:#233044;background:#fff;margin:12px;}"
      "h2{font-size:18px;margin:0 0 6px;}p{margin:6px 0;}"
      "a{color:#2462a3;text-decoration:none;}.muted{color:#64748b;}%1"
      "</style></head><body>%2</body></html>").arg(style, body);
}

}  // namespace kqpet::html
