#include "local_stargod_count.h"
#include "checked_json_numbers.h"
#include <QJsonArray>
#include <QStringList>
#include <limits>

namespace {
bool integer(const QJsonValue& value, int* out, int minimum = 0) {
  qint64 number = 0;
  if (!DomainNumeric::checkedInteger(value,&number,minimum,std::numeric_limits<int>::max())) return false;
  *out = int(number); return true;
}
struct Definition { bool known = false, changeable = false, red = false; };
Definition definition(const QJsonObject& all, int id) {
  const auto value = all.value(QString::number(id)).toObject(); int quality = 0;
  if (!integer(value.value(QStringLiteral("quality")),&quality) || quality > 6 ||
      !value.value(QStringLiteral("changeable")).isBool()) return {};
  return {true,value.value(QStringLiteral("changeable")).toBool(),quality == 6};
}
}

LocalStargodPetCounts countLocalStargods(const QJsonObject& pet, const QJsonObject& stargods) {
  LocalStargodPetCounts result;
  const auto equipped = pet.value(QStringLiteral("sgs"));
  result.equippedKnown = equipped.isString();
  if (result.equippedKnown && !equipped.toString().isEmpty()) {
    for (const auto& slot : equipped.toString().split(QLatin1Char('#'),Qt::KeepEmptyParts)) {
      const auto fields = slot.split(QLatin1Char(':')); int id = 0, source = 0, level = 0;
      const bool valid = fields.size() >= 1 && fields.size() <= 4 && integer(fields.value(0),&id,-2) &&
          (fields.size() == 1 ? id <= 0 : integer(fields.value(1),&level)) &&
          (fields.size() < 3 || integer(fields.value(2),&source,-2));
      if (!valid) { result.equippedKnown = false; ++result.unknownEntries; continue; }
      source = qMax(0,source);
      if (id <= 0 && !source) continue;
      const auto face = definition(stargods,id), base = definition(stargods,source);
      // A mapped changeable slot is a single physical source/base star. Its
      // selected appearance must not also be counted as an ordinary red star.
      Definition held;
      if (source > 0) {
        if (!base.known) { result.equippedKnown = false; ++result.unknownEntries; continue; }
        if (base.changeable) held = base;
        else if (id == -2 || (face.known && face.changeable)) {
          result.equippedKnown = false; ++result.unknownEntries; continue;
        } else held = face;
      } else held = face;
      if (!held.known) { result.equippedKnown = false; ++result.unknownEntries; continue; }
      if (held.red) held.changeable ? ++result.changeableEquipped : ++result.ordinaryEquipped;
    }
  }
  const auto backpack = pet.value(QStringLiteral("sgsp"));
  result.backpackKnown = backpack.isArray();
  if (result.backpackKnown) {
    for (const auto& entry : backpack.toArray()) {
      int id = 0;
      const auto star = integer(entry,&id,1) ? definition(stargods,id) : Definition{};
      if (!star.known) { result.backpackKnown = false; ++result.unknownEntries; continue; }
      if (star.red) star.changeable ? ++result.changeableBackpack : ++result.ordinaryBackpack;
    }
  }
  return result;
}
