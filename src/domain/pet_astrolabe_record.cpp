#include "pet_astrolabe_record.h"
#include "checked_json_numbers.h"
#include <limits>

PetAstrolabeRecord readPetAstrolabeRecord(const QString& token) {
  PetAstrolabeRecord result;
  if (token.size() > 64) return result;
  const auto fields = token.split(QLatin1Char(':'));
  if (fields.size() < 2 || fields.size() > 3) return result;
  qint64 id = 0, light = 0, selected = 0;
  result.idKnown = DomainNumeric::checkedInteger(fields[0], &id, 0, std::numeric_limits<int>::max());
  result.lightedKnown = DomainNumeric::checkedInteger(fields[1], &light, 0, 1);
  result.selectedKnown = fields.size() == 2 || DomainNumeric::checkedInteger(fields[2], &selected, 0, 1);
  result.id = result.idKnown ? int(id) : 0;
  result.lighted = result.lightedKnown && light == 1;
  result.selected = result.selectedKnown && selected == 1;
  result.valid = result.idKnown && result.lightedKnown && result.selectedKnown;
  return result;
}
