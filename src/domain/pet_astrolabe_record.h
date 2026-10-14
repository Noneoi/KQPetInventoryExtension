#pragma once
#include <QString>

struct PetAstrolabeRecord {
  int id = 0;
  bool lighted = false;
  bool selected = false;
  bool idKnown = false;
  bool lightedKnown = false;
  bool selectedKnown = false;
  bool valid = false;
  bool usableForAnalysis() const { return valid && (!selected || lighted); }
};

// PetAstrolabeTraceSlot.init: zero is a real definition; an omitted third
// token means not twinkled. Preserve conflicting observed flags for display,
// but do not let an unlighted selected node assert known cultivation totals.
PetAstrolabeRecord readPetAstrolabeRecord(const QString& token);
