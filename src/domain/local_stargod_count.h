#pragma once
#include <QJsonObject>

struct LocalStargodPetCounts {
  qint64 ordinaryEquipped = 0, ordinaryBackpack = 0;
  qint64 changeableEquipped = 0, changeableBackpack = 0;
  qint64 unknownEntries = 0;
  bool equippedKnown = false, backpackKnown = false;
  bool complete() const { return equippedKnown && backpackKnown; }
};

// Counts physical held pieces, retaining repeated backpack IDs and ordinary
// types. Reads only this pet's fields; nested related pets are never traversed.
LocalStargodPetCounts countLocalStargods(const QJsonObject& pet, const QJsonObject& stargods);
