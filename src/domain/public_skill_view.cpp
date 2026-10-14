#include "public_skill_view.h"

#include <QJsonValue>
#include <QRegularExpression>
#include <cmath>
#include <limits>

namespace {
bool integer(const QJsonValue& value, int* result, int minimum, int maximum) {
  if (!value.isDouble()) return false;
  const double number = value.toDouble();
  if (!std::isfinite(number) || number != std::floor(number) || number < minimum || number > maximum) return false;
  *result = int(number);
  return true;
}

QString parameterText(QString text, const QStringList& arguments) {
  // EntryArgLike defines ARG/ARG1 as the first argument and ARGn as the nth.
  // Match complete placeholders: a missing ARG2 must not become firstArg+"2"
  // through the client's literal-prefix replacement. Its explicit unspecified
  // wording is used for absent arguments; the original catalog stays intact.
  static const QRegularExpression placeholder(QStringLiteral("ARG(\\d*)"));
  QString result;
  qsizetype cursor = 0;
  auto matches = placeholder.globalMatch(text);
  while (matches.hasNext()) {
    const auto match = matches.next();
    result += text.mid(cursor, match.capturedStart() - cursor);
    const auto digits = match.captured(1);
    bool valid = true;
    const int position = digits.isEmpty() ? 1 : digits.toInt(&valid);
    valid = valid && position > 0 && (digits.isEmpty() || QString::number(position) == digits);
    result += valid && position <= arguments.size() ? arguments[position - 1] : QStringLiteral("指定数值");
    cursor = match.capturedEnd();
  }
  result += text.mid(cursor);
  return result;
}
}

const QList<QPair<QString, QString>>& PublicSkillView::slotDefinitions() {
  static const QList<QPair<QString, QString>> values{
      {QStringLiteral("normal"), QStringLiteral("普通技")},
      {QStringLiteral("ultimate"), QStringLiteral("超杀技")},
      {QStringLiteral("super"), QStringLiteral("超必杀技")},
      {QStringLiteral("hero"), QStringLiteral("英雄技")},
      {QStringLiteral("large"), QStringLiteral("大招")},
      {QStringLiteral("dragonJob"), QStringLiteral("龙职技")},
      {QStringLiteral("yuanLi"), QStringLiteral("源力技")},
      {QStringLiteral("psychics"), QStringLiteral("通灵技")},
      {QStringLiteral("fate"), QStringLiteral("天命 / 传说 / 神运技能")},
      {QStringLiteral("transform"), QStringLiteral("转化类技能")},
  };
  return values;
}

bool PublicSkillView::hasTag(const QJsonObject& pet, const QString& tag) {
  // PetDictionaryDataItem.findSign: exact comma-separated tokens. Do not turn
  // "不是灵初" or a similarly named skin into an official era/mechanism tag.
  return pet.value(QStringLiteral("signs")).toString().split(QLatin1Char(','), Qt::KeepEmptyParts).contains(tag);
}

QString PublicSkillView::slotLabel(const QString& slot, const QJsonObject& pet) const {
  if (slot == QStringLiteral("fate")) {
    if (hasTag(pet, QStringLiteral("灵初"))) return QStringLiteral("灵初天命技");
    if (hasTag(pet, QStringLiteral("神运"))) return QStringLiteral("神运技能");
    if (hasTag(pet, QStringLiteral("天启"))) return QStringLiteral("天启 / 传说技");
    if (hasTag(pet, QStringLiteral("传说"))) return QStringLiteral("传说技");
  } else if (slot == QStringLiteral("transform")) {
    int id = 0;
    if (integer(pet.value(QStringLiteral("slots")).toObject().value(slot), &id, 1, std::numeric_limits<int>::max())) {
      const auto transform = data_.object(QStringLiteral("transformSkills"), QString::number(id));
      int type = -1;
      // Reviewed TransformSkillType enum order: 0 ELEMENT_SKILL,
      // 1 ENERGIZE_SKILL, 2 SELF_ELEMENT_SKILL. Missing/unknown type stays generic.
      if (integer(transform.value(QStringLiteral("type")), &type, 0, 2)) {
        if (type == 0) return QStringLiteral("元素技");
        if (type == 1) return QStringLiteral("赋能技");
        return QStringLiteral("幻元技");
      }
    }
  } else if (slot == QStringLiteral("yuanLi")) {
    if (hasTag(pet, QStringLiteral("寰力量"))) return QStringLiteral("寰技");
    if (hasTag(pet, QStringLiteral("异形族"))) return QStringLiteral("异形技");
  }
  for (const auto& entry : slotDefinitions()) if (entry.first == slot) return entry.second;
  return QStringLiteral("技能");
}

PublicSkillTerm PublicSkillView::term(const QString& requested) const {
  PublicSkillTerm result;
  result.requested = requested;
  const auto exact = data_.record({PublicDataViewKind::PetSkills, QStringLiteral("entries"), requested});
  if (exact.exists()) {
    // An explicit empty or invalid current value must never borrow an older
    // base term, suffix term, Buff name, or archived explanation.
    if (!exact.raw.isString()) return result;
    result.source = PublicSkillTermSource::ExactOfficial;
    result.entryName = requested;
    result.description = parameterText(exact.raw.toString(), {});
    return result;
  }
  // Official EntryConfigParser/EntryArgLike uses the middle dot only. Roman
  // suffixes and '-' are ordinary name characters unless an exact row exists.
  const QStringList parts = requested.split(QChar(0x00b7), Qt::KeepEmptyParts);
  const auto base = data_.record({PublicDataViewKind::PetSkills, QStringLiteral("entries"), parts.front()});
  if (parts.size() > 1 && !parts.front().isEmpty() && base.raw.isString()) {
    result.source = PublicSkillTermSource::ParameterizedOfficial;
    result.entryName = parts.front();
    result.arguments = parts.mid(1);
    result.description = parameterText(base.raw.toString(), result.arguments);
  }
  return result;
}
