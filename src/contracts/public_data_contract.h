#pragma once
#include <QJsonValue>
#include <QString>

// These are read projections of ONE public dataset, not independent sources.
// A table/id is meaningful only in its projection and official namespace;
// callers must use an explicitly evidenced relation to cross either boundary.
enum class PublicDataViewKind { PetDetail, PetSkills, BattleReplay, Shop, Routine, PetImages, Icons };
enum class PublicValueState { Missing, Present, Empty, InvalidContainer, WrongView };

struct PublicRecordKey {
  PublicDataViewKind view = PublicDataViewKind::PetDetail;
  QString table;
  QString id;
};

struct PublicRecordValue {
  PublicRecordKey key;
  PublicValueState state = PublicValueState::Missing;
  QJsonValue raw = QJsonValue(QJsonValue::Undefined);
  bool exists() const { return state == PublicValueState::Present || state == PublicValueState::Empty; }
};
