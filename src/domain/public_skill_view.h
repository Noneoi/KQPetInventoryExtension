#pragma once

#include "public_data_view.h"

#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

enum class PublicSkillTermSource { Missing, ExactOfficial, ParameterizedOfficial };

struct PublicSkillTerm {
  PublicSkillTermSource source = PublicSkillTermSource::Missing;
  QString requested;
  QString entryName;
  QString description;
  QStringList arguments;
  bool hasOfficialEntry() const { return source != PublicSkillTermSource::Missing; }
};

// Pure reads over one frozen skill-catalog view. No IO, names-as-identities,
// archived supplements, or interpretation of server-owned battle outcomes.
class PublicSkillView final {
public:
  explicit PublicSkillView(QJsonObject root) : data_(PublicDataViewKind::PetSkills, std::move(root)) {}
  static const QList<QPair<QString, QString>>& slotDefinitions();
  static bool hasTag(const QJsonObject& pet, const QString& tag);
  QString slotLabel(const QString& slot, const QJsonObject& pet) const;
  PublicSkillTerm term(const QString& requested) const;

private:
  PublicDataView data_;
};
