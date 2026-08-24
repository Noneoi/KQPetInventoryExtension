#pragma once

#include <QString>

class PetRepository;

class AssetAnalysisSettings final {
public:
  explicit AssetAnalysisSettings(PetRepository* repository);

  bool loadAutoSnapshot(const QString& account) const;
  bool saveAutoSnapshot(const QString& account, bool enabled) const;

private:
  QString settingsPath() const;

  PetRepository* repository_ = nullptr;
};
