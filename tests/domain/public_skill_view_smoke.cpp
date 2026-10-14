#include "domain/public_skill_view.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <cstdio>

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  bool ok = true;
  auto require = [&](bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    ok = condition && ok;
  };
  QJsonObject entries{{QStringLiteral("护盾"),QStringLiteral("精确护盾说明")},
      {QStringLiteral("伤害降低"),QStringLiteral("降低ARG")},
      {QStringLiteral("伤害降低·30%"),QStringLiteral("精确30%说明")},
      {QStringLiteral("二参"),QStringLiteral("ARG1 / ARG2 / ARG")},
      {QStringLiteral("多位参数"),QStringLiteral("ARG1 / ARG10 / ARG")},
      {QStringLiteral("官方空"),QString()},
      {QStringLiteral("官方空·参数"),QString()},
      {QStringLiteral("背景词"),QStringLiteral("提及[护盾Ⅱ]的另一条文字，不能借用")}};
  QJsonObject root{{QStringLiteral("entries"),entries},
      {QStringLiteral("transformSkills"),QJsonObject{
        {QStringLiteral("11"),QJsonObject{{"name",QStringLiteral("名字含幻元却是元素")},{"type",0}}},
        {QStringLiteral("12"),QJsonObject{{"type",1}}},
        {QStringLiteral("13"),QJsonObject{{"type",2}}},
        {QStringLiteral("14"),QJsonObject{{"name",QStringLiteral("幻元技")}}},
        {QStringLiteral("15"),QJsonObject{{"type",3}}},
        {QStringLiteral("16"),QJsonObject{{"type",false}}},
        {QStringLiteral("17"),QJsonObject{{"type",QStringLiteral("2")}}}}},
      {QStringLiteral("mechanisms"),QJsonObject{{QStringLiteral("护盾Ⅱ"),QStringLiteral("历史借用文字")}}}};
  const QJsonObject original = root;
  PublicSkillView view(root);
  require(view.term(QStringLiteral("护盾")).description == QStringLiteral("精确护盾说明"), "exact current entry was not used");
  for (const QString term : {QStringLiteral("护盾Ⅱ"),QStringLiteral("护盾-Ⅱ"),QStringLiteral("旧护盾"),QStringLiteral("护盾加强")})
    require(!view.term(term).hasOfficialEntry(), "a suffix, substring, or historical note became an alias");
  require(view.term(QStringLiteral("伤害降低·30%")).description == QStringLiteral("精确30%说明"), "exact parameter name lost to its base");
  auto parameter = view.term(QStringLiteral("伤害降低·50%"));
  require(parameter.source == PublicSkillTermSource::ParameterizedOfficial && parameter.description == QStringLiteral("降低50%"), "official middle-dot parameter was not resolved");
  require(view.term(QStringLiteral("二参·甲·乙")).description == QStringLiteral("甲 / 乙 / 甲"), "numbered official arguments were not kept");
  require(view.term(QStringLiteral("二参·甲")).description == QStringLiteral("甲 / 指定数值 / 甲"), "missing second argument became first argument plus a digit");
  require(view.term(QStringLiteral("多位参数·甲")).description == QStringLiteral("甲 / 指定数值 / 甲"), "ARG1 stole the prefix of missing ARG10");
  require(view.term(QStringLiteral("多位参数·甲·乙·丙·丁·戊·己·庚·辛·壬·癸")).description == QStringLiteral("甲 / 癸 / 甲"), "two-digit parameter did not resolve exactly");
  require(view.term(QStringLiteral("二参·ARG2·乙")).description == QStringLiteral("ARG2 / 乙 / ARG2"), "argument text was recursively interpreted as another placeholder");
  require(view.term(QStringLiteral("伤害降低")).description == QStringLiteral("降低指定数值"), "missing argument was invented");
  require(view.term(QStringLiteral("官方空")).hasOfficialEntry() && view.term(QStringLiteral("官方空")).description.isEmpty(), "legitimate empty entry was not preserved");
  require(view.term(QStringLiteral("官方空·参数")).source == PublicSkillTermSource::ExactOfficial && view.term(QStringLiteral("官方空·参数")).description.isEmpty(), "empty exact value fell through to base");
  entries.insert(QStringLiteral("伤害降低·50%"),QJsonValue(QJsonValue::Null));
  root.insert(QStringLiteral("entries"),entries);
  require(!PublicSkillView(root).term(QStringLiteral("伤害降低·50%")).hasOfficialEntry(), "invalid exact value borrowed its base");
  require(view.term(QStringLiteral("伤害降低·50%")).description == QStringLiteral("降低50%"), "existing value snapshot changed with another root");

  QJsonObject pet{{QStringLiteral("signs"),QStringLiteral("不是灵初,前神运后,寰力量假的")},
                 {QStringLiteral("name"),QStringLiteral("[灵初]名字不能代替标签")}};
  require(!PublicSkillView::hasTag(pet,QStringLiteral("灵初")), "partial tag matched");
  require(view.slotLabel(QStringLiteral("fate"),pet) == QStringLiteral("天命 / 传说 / 神运技能"), "pet name or partial era tag supplied a category");
  require(view.slotLabel(QStringLiteral("yuanLi"),pet) == QStringLiteral("源力技"), "partial Huan tag supplied a category");
  pet.insert(QStringLiteral("signs"),QStringLiteral("传说,神运,灵初"));
  require(view.slotLabel(QStringLiteral("fate"),pet) == QStringLiteral("灵初天命技"), "complete cumulative tags did not select latest category");
  pet.insert(QStringLiteral("signs"),QStringLiteral("异形族"));
  require(view.slotLabel(QStringLiteral("yuanLi"),pet) == QStringLiteral("异形技"), "exact YiXingZu tag ignored");
  const QString labels[] = {QStringLiteral("元素技"),QStringLiteral("赋能技"),QStringLiteral("幻元技"),QStringLiteral("转化类技能")};
  for (int id = 11; id <= 17; ++id) {
    pet.insert(QStringLiteral("slots"),QJsonObject{{QStringLiteral("transform"),id}});
    require(view.slotLabel(QStringLiteral("transform"),pet) == labels[id <= 13 ? id-11 : 3], "transform category did not use exact numeric official type");
  }
  pet.insert(QStringLiteral("slots"),QJsonObject{{QStringLiteral("transform"),999}});
  require(view.slotLabel(QStringLiteral("transform"),pet) == QStringLiteral("转化类技能"), "unknown transform borrowed a different row");
  require(PublicSkillView::slotDefinitions().size() == 10, "existing ten slots were lost");
  require(original.value(QStringLiteral("entries")).toObject().value(QStringLiteral("官方空")).toString().isEmpty(), "source record mutated");
  return ok ? 0 : 1;
}
