#pragma once

#include "domain/routine_opportunity_rules.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

inline QJsonObject withOfficialRoutineRuleFixture(QJsonObject root) {
  const QString path = QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath())
      .filePath(QStringLiteral("../fixtures/routine-official-rules/parsed-rules.json"));
  QFile source(path);
  if (!source.open(QIODevice::ReadOnly)) return {};
  const auto fixture = QJsonDocument::fromJson(source.readAll()).object();
  if (!fixture.contains(QStringLiteral("opportunityRules"))) return {};
  root.insert(QStringLiteral("opportunityRules"), fixture.value(QStringLiteral("opportunityRules")));
  root.insert(QStringLiteral("source"), fixture.value(QStringLiteral("source")));
  return root;
}

// A deliberately synthetic value change for consumer tests. Do not use this
// helper to manufacture public source evidence or to publish an actual catalog.
inline QJsonObject withSyntheticRoutineRuleValue(QJsonObject root, const QString& group,
    const QString& field, int value) {
  auto rules = root.value(QStringLiteral("opportunityRules")).toObject();
  auto current = rules.value(group).toObject();
  current.insert(field, value); rules.insert(group, current);
  root.insert(QStringLiteral("opportunityRules"), rules);
  const QHash<QString, QString> labels{{QStringLiteral("starAdventure"), QStringLiteral("starRules")},
      {QStringLiteral("nationalCompetition"), QStringLiteral("competitionRules")}};
  auto source = root.value(QStringLiteral("source")).toObject();
  auto resources = source.value(QStringLiteral("resources")).toObject();
  const auto label = labels.value(group);
  auto binding = resources.value(label).toObject();
  binding.insert(QStringLiteral("rulesSha256"), QString::fromLatin1(QCryptographicHash::hash(
      RoutineOpportunityRules::canonical(current), QCryptographicHash::Sha256).toHex()));
  resources.insert(label, binding); source.insert(QStringLiteral("resources"), resources);
  root.insert(QStringLiteral("source"), source);
  return root;
}
