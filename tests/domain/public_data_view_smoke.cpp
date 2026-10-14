#include "domain/public_data_view.h"
#include "domain/pet_astrolabe_record.h"
#include <QJsonArray>
#include <cstdio>

namespace { bool check(bool ok, const char* text) { if (!ok) std::fprintf(stderr,"FAIL: %s\n",text); return ok; } }
int main() {
  bool ok = true;
  QJsonObject root{{"skills",QJsonObject{{"17",QJsonObject{{"name","skill"},{"description",""}}}}},
                   {"buffs",QJsonObject{{"17",QJsonObject{{"name","buff"},{"unknownRaw",QJsonArray{1,2}}}}}},
                   {"entries",QJsonObject{{"effect", ""},{"effect II","separate text"}}},
                   {"ordered",QJsonArray{17}}};
  PublicDataView view(PublicDataViewKind::PetSkills,root);
  const auto skill = view.record({PublicDataViewKind::PetSkills,"skills","17"});
  ok &= check(skill.exists() && skill.raw.toObject().value("description").isString(),"empty official text lost");
  ok &= check(view.object("buffs","17").value("name").toString()=="buff" &&
      view.object("skills","17").value("name").toString()=="skill","same number crossed definition types");
  ok &= check(view.record({PublicDataViewKind::BattleReplay,"skills","17"}).state==PublicValueState::WrongView,
      "H5 projection silently answered Flash projection lookup");
  ok &= check(view.record({PublicDataViewKind::PetSkills,"entries","effect"}).state==PublicValueState::Empty &&
      view.record({PublicDataViewKind::PetSkills,"entries","effect I"}).state==PublicValueState::Missing,
      "missing term borrowed a similarly named term or lost legitimate empty text");
  ok &= check(view.record({PublicDataViewKind::PetSkills,"ordered","17"}).state==PublicValueState::InvalidContainer,
      "ordered array was treated as ID-keyed table");
  ok &= check(view.object("skills","017").isEmpty(),"numeric identity was silently coerced");
  root.insert("skills",QJsonObject{});
  ok &= check(view.object("skills","17").value("name").toString()=="skill" &&
      view.object("buffs","17").value("unknownRaw").toArray().size()==2,"snapshot or retained raw fields mutated");

  for (const auto& token : {QStringLiteral("0:1"),QStringLiteral("0:1:0")}) {
    const auto value = readPetAstrolabeRecord(token);
    ok &= check(value.usableForAnalysis() && value.id==0 && value.lighted && !value.selected && value.selectedKnown,
        "official node zero or omitted twinkle flag was rejected");
  }
  for (const auto& token : {QStringLiteral("-1:1"),QStringLiteral("1:2"),QStringLiteral("1:1:"),
       QStringLiteral("1:1:1:0"),QStringLiteral("2147483648:1"),QStringLiteral("1 :1"),QStringLiteral("1")})
    ok &= check(!readPetAstrolabeRecord(token).valid,"malformed astrolabe record accepted");
  const auto conflict = readPetAstrolabeRecord(QStringLiteral("1:0:1"));
  ok &= check(conflict.valid && conflict.selected && !conflict.lighted && !conflict.usableForAnalysis(),
      "conflicting raw flags were erased or asserted as a known cultivation result");
  return ok ? 0 : 1;
}
