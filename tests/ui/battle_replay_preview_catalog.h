#pragma once
#include "ui/battle/battle_replay_page.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>

// Standalone/test composition only. The production QWidget never reads or
// parses a resource catalog. All fixtures reuse one immutable initial read.
inline BattleReplayCatalogHandle readPreviewBattleReplayCatalog() {
  const auto arguments = QCoreApplication::arguments();
  const int externalIndex = arguments.indexOf(QStringLiteral("--catalog-json"));
  if (externalIndex >= 0 && externalIndex + 1 >= arguments.size()) return {};
  const bool embedded = externalIndex < 0;
  QFile file(embedded ? QStringLiteral(":/kqpet/battle-replay-catalog.json") : arguments.at(externalIndex + 1));
  if (!file.open(QIODevice::ReadOnly)) return {};
  if (file.size() <= 0 || file.size() > 256 * 1024 * 1024) return {};
  const auto bytes = file.readAll();
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(bytes, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject() || document.object().isEmpty()) return {};
  auto snapshot = std::make_shared<BattleReplayCatalogSnapshot>();
  snapshot->root = document.object();
  static quint64 revision = 0;
  snapshot->revision = ++revision;
  snapshot->sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  snapshot->sourcePath = file.fileName();
  snapshot->sourceLabel = embedded ? QStringLiteral("独立预览内置资料") : QStringLiteral("独立预览外部资料");
  snapshot->loadedAt = QDateTime::currentDateTimeUtc();
  snapshot->embedded = embedded;
  return snapshot;
}

inline BattleReplayCatalogHandle previewBattleReplayCatalog() {
  static const auto snapshot = readPreviewBattleReplayCatalog();
  return snapshot;
}

inline BattleReplayPage* createPreviewBattleReplayPage(QWidget* parent) {
  auto* page = new BattleReplayPage(parent, previewBattleReplayCatalog());
  QObject::connect(page, &BattleReplayPage::catalogReloadRequested, page, [page] {
    page->setCatalogState({}, QStringLiteral("正在读取预览内置资料"), true);
    const auto snapshot = readPreviewBattleReplayCatalog();
    page->setCatalogState(snapshot, snapshot ? QStringLiteral("已重新读取预览内置资料")
      : QStringLiteral("预览资料读取失败，保留已有资料"), false);
  });
  return page;
}
