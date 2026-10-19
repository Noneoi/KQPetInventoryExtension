#include "bridge/game_navigation_script.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc != 3) return 2;
  const auto args = app.arguments();
  QJsonObject result;
  if (args[1] == QStringLiteral("--script")) {
    result.insert(QStringLiteral("allowed"), GameNavigationScript::isAllowedLink(args[2]));
    result.insert(QStringLiteral("script"), GameNavigationScript::build(args[2]));
  } else if (args[1] == QStringLiteral("--result")) {
    result.insert(QStringLiteral("kind"), static_cast<int>(GameNavigationScript::classify(args[2])));
    result.insert(QStringLiteral("message"), GameNavigationScript::message(args[2]));
  } else return 3;
  const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
  std::fwrite(bytes.constData(), 1, bytes.size(), stdout);
  return 0;
}
