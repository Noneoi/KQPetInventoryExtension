#include "application/catalog/public_dataset_store.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>

// The application and command-line updater share this exact publication code.
// No catalog file is written through a second, less strict CLI path.
int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  const auto args = application.arguments();
  if (args.size() < 3) {
    std::fprintf(stderr, "Usage: KQPublicDatasetCheck --apply-candidate <dataRoot> <attempt> | --migrate <dataRoot> | --rollback <dataRoot> | --restore-version <dataRoot> <revision> | --inspect <dataRoot>\n");
    return 2;
  }
  PublicDatasetStore store(args.at(2));
  PublicDatasetResult result;
  if (args.at(1) == QStringLiteral("--inspect") && args.size() == 3) {
    QString error;
    auto report = store.inspect(&error);
    if (!error.isEmpty()) report.insert("error", error);
    const auto output = QJsonDocument(report).toJson(QJsonDocument::Compact);
    std::fwrite(output.constData(), 1, size_t(output.size()), stdout); std::fputc('\n', stdout);
    return error.isEmpty() ? 0 : 1;
  }
  if (args.at(1) == QStringLiteral("--apply-candidate") && args.size() == 4)
    result = store.prepareCandidate(args.at(3));
  else if (args.at(1) == QStringLiteral("--migrate") && args.size() == 3)
    result = store.prepareMigration();
  else if (args.at(1) == QStringLiteral("--rollback") && args.size() == 3)
    result = store.prepareRollback();
  else if (args.at(1) == QStringLiteral("--restore-version") && args.size() == 4)
    result = store.prepareRestoreRevision(args.at(3));
  else {
    std::fprintf(stderr, "Unknown command or invalid argument count\n"); return 2;
  }
  if (result.ok() && result.prepared) result = store.commitPrepared(result.prepared);
  QJsonObject report{{"status", PublicDatasetStore::statusName(result.status)}, {"error", result.error},
      {"revision", result.snapshot ? result.snapshot->revision : QString{}},
      {"attempt", result.prepared ? result.prepared->attempt : result.receipt.value("attempt").toString()},
      {"diskCommitted", result.status == PublicDatasetStatus::DiskCommitted},
      {"sessionApplied", false}, {"receipt", result.receipt}};
  const auto output = QJsonDocument(report).toJson(QJsonDocument::Compact);
  std::fwrite(output.constData(), 1, size_t(output.size()), stdout); std::fputc('\n', stdout);
  return result.ok() ? 0 : 1;
}
