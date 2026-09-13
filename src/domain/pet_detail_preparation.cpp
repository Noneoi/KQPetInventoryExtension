#include "pet_detail_preparation.h"
#include "pet_era.h"
#include "asset_derivation.h"
#include "checked_json_numbers.h"
#include "pet_identity.h"
#include "pet_metadata_view.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <climits>
#include <limits>

namespace {
DetailField field(QString label, QString text, DetailKnowledge state = DetailKnowledge::Known) {
  DetailField value; value.label = std::move(label); value.text = std::move(text); value.state = state; return value;
}
bool number(const QJsonValue& value, qint64* result, qint64 minimum = 0, qint64 maximum = INT_MAX) {
  return DomainNumeric::checkedInteger(value, result, minimum, maximum);
}
DetailField numeric(QString label, const QJsonValue& value, qint64 minimum = 0) {
  qint64 n = 0;
  return number(value, &n, minimum) ? field(std::move(label), QString::number(n))
      : field(std::move(label), QStringLiteral("待确认"), value.isUndefined() ? DetailKnowledge::Unknown : DetailKnowledge::Invalid);
}
DetailField boolean(QString label, const QJsonValue& value, QString yes, QString no) {
  return value.isBool() ? field(std::move(label), value.toBool() ? std::move(yes) : std::move(no))
      : field(std::move(label), QStringLiteral("待确认"), value.isUndefined() ? DetailKnowledge::Unknown : DetailKnowledge::Invalid);
}
QVector<QString> parts(const QString& text, QChar separator, int maximum = 16) {
  QVector<QString> result; int start = 0;
  for (int i = 0; i <= text.size(); ++i) if (i == text.size() || text[i] == separator) {
    result.append(text.mid(start, i - start)); start = i + 1;
    if (result.size() > maximum) return {};
  }
  return result;
}
QJsonValue part(const QVector<QString>& values, int index) {
  return index >= 0 && index < values.size() ? QJsonValue(values[index]) : QJsonValue(QJsonValue::Undefined);
}
quint64 textBytes(const QString& text) { return 64 + quint64(qMax(text.size(), text.capacity())) * sizeof(QChar); }
quint64 fieldBytes(const DetailField& value) { return sizeof(value) + textBytes(value.label) + textBytes(value.text); }
quint64 entryBytes(const DetailEntry& value) {
  quint64 bytes = sizeof(value) + textBytes(value.name) + textBytes(value.group) + textBytes(value.problem);
  for (const auto& item : value.fields) bytes += fieldBytes(item);
  return bytes;
}
QString category(int id, const QString& name) {
  static const QSet<int> defenses{10,15,16,17,18,19,21,26,34,35,37,38,39,40,46,47,48,49,54,55,57,60,63,64,70,72,73,74,76,85,86};
  static const QSet<int> functions{11,20,22,29,33,45,67,68,81,84,88};
  for (const auto& token : {QStringLiteral("如有神助"),QStringLiteral("顺应天命"),QStringLiteral("天煞孤星"),QStringLiteral("气贯星河")})
    if (name.contains(token)) return QStringLiteral("功能性");
  if (name.contains(QStringLiteral("乘胜追击"))) return QStringLiteral("进攻");
  if (name.contains(QStringLiteral("福虎佑灵")) || defenses.contains(id)) return QStringLiteral("防御");
  if (functions.contains(id)) return QStringLiteral("功能性");
  for (const auto& token : {QStringLiteral("盾"),QStringLiteral("防"),QStringLiteral("护"),QStringLiteral("躯"),QStringLiteral("霸体"),QStringLiteral("闪")})
    if (name.contains(token)) return QStringLiteral("防御");
  for (const auto& token : {QStringLiteral("人品"),QStringLiteral("命中"),QStringLiteral("幸运"),QStringLiteral("追击"),QStringLiteral("连打")})
    if (name.contains(token)) return QStringLiteral("功能性");
  return QStringLiteral("进攻");
}
qint64 relationId(const QJsonValue& value) {
  qint64 id = 0;
  const auto scalar = value.isObject() ? value.toObject().value(QStringLiteral("id")) : value;
  return number(scalar, &id, 1, std::numeric_limits<qint64>::max()) ? id : 0;
}
DetailEntry invalidEntry(const QString& name, const QString& reason) {
  DetailEntry value; value.name = name; value.state = DetailKnowledge::Invalid; value.problem = reason; return value;
}
}

quint64 preparedPetDetailRetainedBytes(const PreparedPetDetail& detail) {
  quint64 bytes = sizeof(detail) + 512 + textBytes(detail.version.facts.record.account) + quint64(detail.version.related.capacity()) * sizeof(DetailRelatedRevision);
  const auto& identity = detail.identity;
  for (const auto& text : {identity.name,identity.originalName,identity.customName,identity.attributes,identity.jobs,identity.era,identity.visualKey}) bytes += textBytes(text);
  bytes += fieldBytes(identity.level);
  for (const auto& text : identity.imageCandidateNames) bytes += textBytes(text);
  for (const auto& item : detail.talent) bytes += fieldBytes(item);
  for (const auto& item : detail.sacred) bytes += fieldBytes(item);
  bytes += quint64(detail.cultivationRequirements.items.capacity()) * sizeof(PetCultivationRequirement);
  for (const auto& item : detail.cultivationRequirements.items) {
    bytes += textBytes(item.key) + textBytes(item.category) + textBytes(item.name) + textBytes(item.status) +
        quint64(item.materials.capacity()) * sizeof(PetCultivationMaterial);
    for (const auto& material : item.materials) bytes += textBytes(material.name) + textBytes(material.scope);
  }
  for (const auto& page : detail.pages) {
    bytes += sizeof(page) + textBytes(page.problem);
    for (const auto& entry : page.entries) bytes += entryBytes(entry);
  }
  // battlePower's implicitly shared strings/lists retain the original facts lease.
  return bytes;
}
bool detailRelatedSummaryValid(const DetailRelatedSummary& summary, QString* error) {
  static const QSet<QString> allowed{QStringLiteral("id"),QStringLiteral("r"),QStringLiteral("ri"),QStringLiteral("fr"),
      QStringLiteral("n"),QStringLiteral("customName"),QStringLiteral("lv"),QStringLiteral("zdl"),QStringLiteral("xzdl"),
      QStringLiteral("rt"),QStringLiteral("_metaOriginalName"),QStringLiteral("_metaAttributes"),QStringLiteral("_metaJobs"),
      QStringLiteral("_metaEra"),QStringLiteral("_metaRaceId")};
  const auto fail = [&] { if (error) *error = QStringLiteral("invalid related identity, revision or non-summary fields"); return false; };
  if (summary.instanceId <= 0 || (summary.present && summary.revision == 0) || (!summary.present && !summary.fields.isEmpty())) return fail();
  if (summary.fields.contains(QStringLiteral("id")) && relationId(summary.fields.value(QStringLiteral("id"))) != summary.instanceId) return fail();
  for (auto it = summary.fields.begin(); it != summary.fields.end(); ++it)
    if (!allowed.contains(it.key()) || it.value().isArray() || it.value().isObject() ||
        (it.value().isString() && it.value().toString().size() > 32768)) return fail();
  return true;
}

struct PetDetailPreparation::Impl {
  FrozenDetailInputs input;
  DetailPreparationLimits limits;
  PetMetadataView metadata;
  DetailSection requested;
  int requestedPage = 0;
  std::unique_ptr<PreparedPetDetail> result = std::make_unique<PreparedPetDetail>();
  DetailPreparationProgress progress;
  QString failure;
  bool initialized = false, complete = false;
  int page = 0, sourceIndex = 0, tokenStart = 0, tokenPosition = 0, relationGroup = 0;
  bool sectionInitialized = false;
  int activatedCount = 0, selectedCount = 0;
  bool activatedCountKnown = false, selectedCountKnown = false;
  bool astrolabeBreakthroughApplicable = false;
  QString encoded;
  QJsonArray array;
  struct RelationGroup { QString label; QJsonArray values; int fallbackRace = 0; QString identityProblem; };
  QVector<RelationGroup> groups;
  QSet<qint64> seen;
  struct PendingRelation { QJsonObject embedded; qint64 id = 0; int race = 0; QString label; QString problem; };
  QVector<PendingRelation> pendingRelations;
  QHash<qint64, DetailRelatedSummary> summaries;
  QVector<qint64> needs;
  Impl(FrozenDetailInputs value, DetailSection section, int pageNumber, DetailPreparationLimits requestedLimits)
      : input(std::move(value)), limits(requestedLimits), metadata(input.metadata), requested(section), requestedPage(pageNumber) {}
  QJsonValue value(const char* name) const {
    const QString key = QString::fromLatin1(name);
    if (input.brief.contains(key)) return input.brief.value(key);
    if (input.raw && input.raw->brief.contains(key)) return input.raw->brief.value(key);
    return input.raw ? input.raw->object().value(key) : QJsonValue(QJsonValue::Undefined);
  }
  bool initialize() {
    if (!input.raw || !input.facts || !input.metadata || input.summaryRevision == 0 ||
        !(input.raw->key == input.facts->key.record) || input.metadata->revision != input.facts->key.metadataRevision ||
        input.metadata->contentDigest != input.facts->key.metadataDigest || input.facts->key.analysisVersion != AssetAnalysisVersion::kCurrentAnalysis ||
        (input.raw->complete && !input.raw->payload) || requestedPage < 0 || requestedPage > 1000000 ||
        (requested == DetailSection::CarryRelations && requestedPage != 0) || limits.maximumExpandedCarryItems <= 0 || input.brief.size() > 64) {
      failure = QStringLiteral("detail inputs do not share a valid record/metadata/summary version"); return false;
    }
    if ((input.raw->complete && relationId(input.raw->object()) != input.raw->key.instanceId) ||
        (input.brief.contains(QStringLiteral("id")) && relationId(input.brief) != input.raw->key.instanceId)) {
      failure = QStringLiteral("detail raw/brief instance identity mismatch"); return false;
    }
    QJsonObject identity = AssetDerivation::identityFields(input.raw->brief);
    for (auto it = input.brief.begin(); it != input.brief.end(); ++it) {
      if (it.value().isString() && it.value().toString().size() > limits.maximumTextUnits) { failure = QStringLiteral("summary text exceeds its supported size"); return false; }
      if (it.value().isArray() || it.value().isObject()) { failure = QStringLiteral("summary contains a raw subtree"); return false; }
      identity.insert(it.key(), it.value());
    }
    for (const auto& name : {"n","customName","r","ri","fr","lv","rt","_metaOriginalName","_metaAttributes","_metaJobs","_metaEra","_metaRaceId"})
      if (!identity.contains(QString::fromLatin1(name)) && !value(name).isUndefined()) identity.insert(QString::fromLatin1(name), value(name));
    auto& out = *result;
    astrolabeBreakthroughApplicable = eraHasAstrolabeBreakthrough(resolvePetEra(
        identity, input.metadata->root.value(QStringLiteral("pets")).toObject()));
    out.version.facts = input.facts->key; out.version.summaryRevision = input.summaryRevision;
    out.sourceVerified = input.sourceVerified && input.raw->sourceKnown && input.facts->facts.asset.observationVerified;
    out.visualMismatch = value("_visualMismatch").toBool() || (input.raw->complete && petRaceId(input.raw->object()) > 0 && petRaceId(input.raw->object()) != input.facts->facts.asset.raceId);
    out.detailKnown = input.raw->complete && input.facts->facts.asset.detailAvailable && !out.visualMismatch;
    out.battlePower = out.detailKnown ? input.facts->facts.battlePower : PetBattlePowerState{};
    if (out.detailKnown) out.cultivationRequirements = calculatePetCultivationRequirements(
        input.raw->object(), input.metadata->root, out.battlePower);
    auto& id = out.identity; id.instanceId = input.raw->key.instanceId; id.raceId = input.facts->facts.asset.raceId;
    id.name = value("n").toString(); if (id.name.isEmpty()) id.name = metadata.petName(id.raceId);
    if (id.name.isEmpty()) id.name = QStringLiteral("未知精灵");
    id.originalName = metadata.resolvedOriginalName(identity); id.customName = value("customName").toString();
    id.attributes = metadata.resolvedAttributes(identity); id.jobs = metadata.resolvedJobs(identity); id.era = metadata.resolvedEra(identity);
    id.level = numeric(QStringLiteral("等级"), value("lv")); id.visualKey = petVisualKey(identity);
    id.imageCandidateNames = {id.name,id.customName,id.originalName,metadata.petName(id.raceId)}; id.imageCandidateNames.removeAll(QString()); id.imageCandidateNames.removeDuplicates();
    prepareTalent(); prepareSacred();
    const QVector<DetailSection> sections = requested == DetailSection::Overview
        ? QVector<DetailSection>{DetailSection::Badges,DetailSection::Astrolabe,DetailSection::EquippedStargods,DetailSection::StargodBackpack,DetailSection::SummonRelations,DetailSection::CarryRelations}
        : QVector<DetailSection>{requested};
    for (auto section : sections) {
      DetailPage p; p.section = section; p.pageIndex = requested == DetailSection::Overview ? 0 : requestedPage;
      if (requested == DetailSection::CarryRelations) p.pageSize = limits.maximumExpandedCarryItems;
      out.pages.append(p);
    }
    initialized = true; return true;
  }
  void prepareTalent() {
    auto& fields = result->talent;
    if (!result->detailKnown) { fields.append(field(QStringLiteral("状态"),QStringLiteral("详情待确认"),DetailKnowledge::Unknown)); return; }
    static const QStringList levels{QStringLiteral("一无是处"),QStringLiteral("十分常见"),QStringLiteral("百里挑一"),QStringLiteral("千载难逢"),QStringLiteral("万众瞩目"),QStringLiteral("王者无敌"),QStringLiteral("超凡入圣")};
    qint64 level = 0;
    fields.append(number(value("gt"), &level) ? field(QStringLiteral("评价"),QStringLiteral("%1（%2级）").arg(level < levels.size() ? levels[int(level)] : QStringLiteral("等级 %1").arg(level)).arg(level)) : numeric(QStringLiteral("评价"),value("gt")));
    static const QStringList names{QStringLiteral("生命"),QStringLiteral("物攻"),QStringLiteral("物防"),QStringLiteral("魔攻"),QStringLiteral("魔防"),QStringLiteral("超攻"),QStringLiteral("超防"),QStringLiteral("速度"),QStringLiteral("超物攻"),QStringLiteral("超物防"),QStringLiteral("超魔攻"),QStringLiteral("超魔防")};
    const auto ip = value("ip"), gps = value("gps");
    const auto talents = ip.isString() && ip.toString().size() <= limits.maximumTextUnits ? parts(ip.toString(),QLatin1Char('#'),12) : QVector<QString>{};
    const auto energies = gps.isString() && gps.toString().size() <= limits.maximumTextUnits ? parts(gps.toString(),QLatin1Char('#'),12) : QVector<QString>{};
    QStringList single, dual, unpowered, other;
    // ip/gps share the original 12-position property ordering. Hidden legacy
    // properties must never shift the six properties retained in this view.
    for (const int i : {0,7,8,9,10,11}) {
      const auto f = numeric(names[i],part(talents,i)); qint64 energy = 0;
      const QString text = names[i] + QStringLiteral(" ") + f.text;
      // Official PetGift.setStars treats an explicitly empty gps as no star
      // energy (1). A missing field/segment is still unknown. ip already holds
      // the final, energy-adjusted value; do not multiply it a second time.
      const bool energyKnown = (gps.isString() && gps.toString().isEmpty()) ||
          number(part(energies,i),&energy,0,3);
      if (energyKnown && energy == 2) single.append(text);
      else if (energyKnown && energy == 3) dual.append(text);
      else if (energyKnown) unpowered.append(text);
      else other.append(text + QStringLiteral("（星能待确认）"));
    }
    fields.append(field(QStringLiteral("单星能"),single.isEmpty() ? QStringLiteral("—") : single.join(QStringLiteral("　"))));
    fields.append(field(QStringLiteral("双星能"),dual.isEmpty() ? QStringLiteral("—") : dual.join(QStringLiteral("　"))));
    if (!unpowered.isEmpty()) fields.append(field(QStringLiteral("无星能"),unpowered.join(QStringLiteral("　"))));
    if (!other.isEmpty()) fields.append(field(QStringLiteral("其他 / 待确认"),other.join(QStringLiteral("　")),DetailKnowledge::Unknown));
    const auto current = numeric({},value("czdlv").toObject().value(QStringLiteral("iv")));
    const auto maximum = numeric({},value("mzdlv").toObject().value(QStringLiteral("iv")));
    fields.append(field(QStringLiteral("天赋战斗力/满天赋战斗力"),current.text + QStringLiteral("/") + maximum.text,
                        current.state == DetailKnowledge::Known && maximum.state == DetailKnowledge::Known ? DetailKnowledge::Known : DetailKnowledge::Unknown));
  }
  void prepareSacred() {
    auto& fields = result->sacred; const auto raw = value("shenjue");
    if (!result->detailKnown || raw.isUndefined()) { fields.append(field(QStringLiteral("状态"),QStringLiteral("待确认"),DetailKnowledge::Unknown)); return; }
    if (!raw.isString() || raw.toString().size() > limits.maximumTextUnits) { fields.append(field(QStringLiteral("状态"),QStringLiteral("神源兽字段无效"),DetailKnowledge::Invalid)); return; }
    if (raw.toString().isEmpty()) { fields.append(field(QStringLiteral("状态"),QStringLiteral("未装备神源兽"))); return; }
    const auto halves = parts(raw.toString(),QLatin1Char('|'),2); const auto define = parts(part(halves,0).toString(),QLatin1Char('#'),3);
    const auto levels = halves.size() > 1 ? parts(halves[1],QLatin1Char(':'),2) : QVector<QString>{};
    qint64 id = 0, starPlan = 0, stagePlan = 0, star = 0, stage = 0;
    fields.append(number(part(define,0),&id,1) ? field(QStringLiteral("名称"),metadata.sacredEquipmentName(int(id))) : field(QStringLiteral("名称"),QStringLiteral("待确认"),DetailKnowledge::Invalid));
    const int maxStar = number(part(define,1),&starPlan,1,std::numeric_limits<int>::max()) ? metadata.sacredMaxStar(int(starPlan)) : 0;
    const int maxStage = number(part(define,2),&stagePlan,1,std::numeric_limits<int>::max()) ? metadata.sacredMaxStage(int(stagePlan)) : 0;
    auto stars = numeric(QStringLiteral("星级"),part(levels,0));
    if (number(part(levels,0),&star) && maxStar > 0 && star <= maxStar) stars.text = QStringLiteral("%1/%2 星（%3）").arg(star).arg(maxStar).arg(star == maxStar ? QStringLiteral("满星") : QStringLiteral("未满星"));
    else if (stars.state == DetailKnowledge::Known) { stars.text += QStringLiteral(" 星（上限待确认）"); stars.state = DetailKnowledge::Unknown; }
    auto stages = numeric(QStringLiteral("阶级"),part(levels,1));
    if (number(part(levels,1),&stage) && maxStage > 0 && stage <= maxStage) stages.text = QStringLiteral("%1/%2 阶（%3）").arg(stage).arg(maxStage).arg(stage == maxStage ? QStringLiteral("满阶") : QStringLiteral("未满阶"));
    else if (stages.state == DetailKnowledge::Known) { stages.text += QStringLiteral(" 阶（上限待确认）"); stages.state = DetailKnowledge::Unknown; }
    fields.append(stars); fields.append(stages);
  }
  void beginSection() {
    sectionInitialized = true; sourceIndex = tokenStart = tokenPosition = relationGroup = 0; encoded.clear(); array = {}; groups.clear(); seen.clear();
    auto& p = result->pages[page];
    if (p.section == DetailSection::Astrolabe) {
      activatedCount = selectedCount = 0; activatedCountKnown = selectedCountKnown = false;
      DetailEntry status; status.name = QStringLiteral("星轮状态");
      if (astrolabeBreakthroughApplicable)
        status.fields.append(boolean(QStringLiteral("突破"),result->detailKnown ? value("astrolabebr") : QJsonValue(QJsonValue::Undefined),QStringLiteral("已突破"),QStringLiteral("未突破")));
      status.fields.append(field(QStringLiteral("点亮数量"),QStringLiteral("待确认"),DetailKnowledge::Unknown));
      status.fields.append(field(QStringLiteral("选中数量"),QStringLiteral("待确认"),DetailKnowledge::Unknown));
      append(std::move(status));
    }
    if (!result->detailKnown) { p.problem = QStringLiteral("详情待确认"); return; }
    const char* key = p.section == DetailSection::Badges ? "badge" : p.section == DetailSection::Astrolabe ? "astrolabe" : p.section == DetailSection::EquippedStargods ? "sgs" : "sgsp";
    if (p.section == DetailSection::SummonRelations || p.section == DetailSection::CarryRelations) {
      p.state = DetailKnowledge::Known;
      bool hadRelationFields = false;
      const auto appendArray = [&](const char* source, const QString& label) {
        const auto v = value(source);
        hadRelationFields |= !v.isUndefined();
        if (v.isArray()) groups.append({label,v.toArray(),0});
        else if (!v.isUndefined()) { groups.append({label,QJsonArray{v},0}); p.state = DetailKnowledge::Invalid; }
      };
      const auto appendOne = [&](const char* embeddedKey, const char* primary, const char* fallback, const QString& label, int race = 0) {
        const auto object = embeddedKey ? value(embeddedKey) : QJsonValue(QJsonValue::Undefined);
        QJsonValue reference = value(primary);
        hadRelationFields |= !object.isUndefined() || !reference.isUndefined() || race > 0;
        qint64 sentinel = -1;
        const bool zero = number(reference,&sentinel,0,std::numeric_limits<qint64>::max()) && sentinel == 0;
        if ((reference.isUndefined() || zero) && fallback && !value(fallback).isUndefined()) reference = value(fallback);
        const qint64 embeddedId = relationId(object), referenceId = relationId(reference);
        const QString conflict = embeddedId > 0 && referenceId > 0 && embeddedId != referenceId
            ? QStringLiteral("嵌入对象与关系字段的实例 ID 冲突：%1 / %2").arg(embeddedId).arg(referenceId) : QString();
        if (object.isObject() && !object.toObject().isEmpty()) {
          QJsonObject small = object.toObject();
          if (!small.contains(QStringLiteral("id")) && !reference.isUndefined()) small.insert(QStringLiteral("id"), reference);
          groups.append({label,QJsonArray{small},race,conflict});
        } else if (number(reference,&sentinel,0,std::numeric_limits<qint64>::max()) && sentinel == 0) {
          if (race > 0) groups.append({label,QJsonArray{QJsonObject{{QStringLiteral("ri"),race}}},race});
        } else if (!reference.isUndefined()) groups.append({label,QJsonArray{reference},race});
        else if (race > 0) groups.append({label,QJsonArray{QJsonObject{{QStringLiteral("ri"),race}}},race});
        else if (!object.isUndefined() && !object.isObject()) groups.append({label,QJsonArray{object},race});
      };
      if (p.section == DetailSection::SummonRelations) {
        appendOne("sppl","sepi","sdpi",QStringLiteral("被召唤精灵")); appendArray("asps",QStringLiteral("召唤关联列表"));
        qint64 race = 0; number(value("srri"),&race,1); appendOne(nullptr,"srpi",nullptr,QStringLiteral("召唤者"),int(race));
      } else {
        appendOne("cppl","cepi",nullptr,QStringLiteral("正在被携带"));
        appendArray("crpis",QStringLiteral("携带者 / 神使"));
        const auto candidates = value("acps");
        hadRelationFields |= !candidates.isUndefined();
        p.carryCandidatesKnown = candidates.isArray();
        p.hasCarryCandidates = !candidates.isUndefined() && (!candidates.isArray() || !candidates.toArray().isEmpty());
        p.carryCandidatesExpanded = requested == DetailSection::CarryRelations;
        if (p.carryCandidatesExpanded) appendArray("acps",QStringLiteral("可以携带"));
      }
      if (groups.isEmpty() && !hadRelationFields) { p.state = DetailKnowledge::Unknown; p.problem = QStringLiteral("没有返回关系字段"); }
      return;
    }
    const auto v = value(key);
    if (v.isUndefined()) { p.problem = QStringLiteral("详情未返回此字段"); return; }
    if (p.section == DetailSection::StargodBackpack) {
      if (!v.isArray()) { p.state = DetailKnowledge::Invalid; p.problem = QStringLiteral("背包字段不是数组"); return; }
      array = v.toArray(); p.state = result->battlePower.stargodBackpackKnown ? DetailKnowledge::Known : DetailKnowledge::Unknown;
      if (p.state != DetailKnowledge::Known) p.problem = QStringLiteral("背包完整性待确认；逐项显示原始观察");
    } else if (v.isString()) { encoded = v.toString(); p.state = DetailKnowledge::Known; if (p.section == DetailSection::Astrolabe) activatedCountKnown = selectedCountKnown = true; }
    else { p.state = DetailKnowledge::Invalid; p.problem = QStringLiteral("编码字段类型无效"); }
  }
  bool wanted(quint64 index) const {
    const auto& p = result->pages[page];
    return requested == DetailSection::CarryRelations || index / 64 == quint64(p.pageIndex);
  }
  void append(DetailEntry entry) {
    auto& p = result->pages[page];
    if (entry.state == DetailKnowledge::Invalid) p.state = DetailKnowledge::Invalid;
    if (wanted(p.totalItems++)) p.entries.append(std::move(entry));
  }
  DetailEntry badge(const QString& token) {
    const auto halves = parts(token,QLatin1Char('#'),2); const auto job = parts(part(halves,0).toString(),QLatin1Char(':'),2);
    qint64 id = 0; if (!number(part(job,0),&id,1)) return invalidEntry(QStringLiteral("元魂"),QStringLiteral("元魂定义无效"));
    DetailEntry e; e.name = metadata.badgeName(int(id)); e.fields.append(numeric(QStringLiteral("等级"),part(job,1)));
    if (halves.size() > 1) { const auto exclusive = parts(halves[1],QLatin1Char(':'),2); qint64 exclusiveId = 0, awake = 0;
      if (number(part(exclusive,0),&exclusiveId,1)) e.fields.append(field(QStringLiteral("专属元魂"),metadata.badgeName(int(exclusiveId))));
      else e.fields.append(field(QStringLiteral("专属元魂"),QStringLiteral("待确认"),DetailKnowledge::Invalid));
      e.fields.append(number(part(exclusive,1),&awake,0,1) ? field(QStringLiteral("觉醒"),awake ? QStringLiteral("已觉醒") : QStringLiteral("未觉醒")) : field(QStringLiteral("觉醒"),QStringLiteral("待确认"),DetailKnowledge::Invalid));
    }
    return e;
  }
  DetailEntry astrolabe(const QString& token) {
    const auto f = parts(token,QLatin1Char(':'),3); qint64 id = 0, activated = 0, selected = 0;
    if (f.size() != 3 || !number(part(f,0),&id,1)) { activatedCountKnown = selectedCountKnown = false; return invalidEntry(QStringLiteral("星轮"),QStringLiteral("星轮字段无效")); }
    DetailEntry e; e.name = metadata.astrolabeName(int(id)); const auto definition = metadata.astrolabe(int(id));
    const bool a = number(part(f,1),&activated,0,1), s = number(part(f,2),&selected,0,1);
    activatedCountKnown &= a; selectedCountKnown &= s;
    if (a && activated) ++activatedCount; if (s && selected) ++selectedCount;
    e.activated = a && activated; e.selected = s && selected;
    e.fields.append(field(QStringLiteral("点亮"),a ? (activated ? QStringLiteral("已点亮") : QStringLiteral("未点亮")) : QStringLiteral("待确认"),a ? DetailKnowledge::Known : DetailKnowledge::Invalid));
    e.fields.append(field(QStringLiteral("装备"),s ? (selected ? QStringLiteral("已装备") : QStringLiteral("未装备")) : QStringLiteral("待确认"),s ? DetailKnowledge::Known : DetailKnowledge::Invalid));
    if (definition.value(QStringLiteral("exclusive")).toBool() && !e.activated) {
      const QString costs = definition.value(QStringLiteral("lightUpCost")).toString();
      if (costs.size() <= limits.maximumTextUnits) for (const auto& cost : parts(costs,QLatin1Char('#'),128)) {
        const auto c = parts(cost,QLatin1Char(':'),3); qint64 type = 0, item = 0, count = 0;
        if (number(part(c,0),&type) && number(part(c,1),&item,1) && number(part(c,2),&count,1)) e.fields.append(field(QStringLiteral("点亮需要"),metadata.materialCostText(int(type),int(item),int(count))));
        else if (!cost.isEmpty()) e.fields.append(field(QStringLiteral("点亮需要"),QStringLiteral("待确认"),DetailKnowledge::Invalid));
      }
    }
    return e;
  }
  DetailEntry star(const QJsonValue& raw, bool equipped) {
    qint64 id = 0, level = 0, source = 0; QVector<QString> f;
    if (equipped) {
      f = parts(raw.toString(),QLatin1Char(':'),4);
      // Official PetStarGodSlot.valueOf preserves every slot, including the
      // bare empty/disabled sentinels and the optional fourth protocol field.
      if (f.size() == 1 && number(part(f,0),&id,-2,0)) {
        if (id == -1) id = 0;
        f = {QString::number(id),QStringLiteral("1")};
      }
      if (f.size() < 2 || f.size() > 4 || !number(part(f,0),&id,-2) || id == -1)
        return invalidEntry(QStringLiteral("星神栏位"),QStringLiteral("星神编码无效"));
    }
    else if (!number(raw,&id,1)) return invalidEntry(QStringLiteral("背包条目"),QStringLiteral("协议需要正整数星神 ID；此原始条目无法解释"));
    if (equipped) { number(part(f,1),&level); if (f.size() >= 3) number(part(f,2),&source); }
    const auto definition = metadata.stargod(int(id)); const auto original = metadata.stargod(int(source));
    DetailEntry e; e.changeable = id == -2 || definition.value(QStringLiteral("changeable")).toBool() || original.value(QStringLiteral("changeable")).toBool();
    e.emptySlot = equipped && (id == 0 || id == -2);
    e.imageDefineId = id > 0 ? int(id) : 0;
    e.name = id == 0 ? QStringLiteral("未装备") : id == -2 ? QStringLiteral("固定万变栏位") : definition.value(QStringLiteral("name")).toString(QStringLiteral("星神 %1").arg(id));
    // Official PetStarGodSlot.quality uses the equipped mapped definition;
    // the original changeable item is only the slot's source, not its color.
    qint64 quality = 0; const auto q = definition.value(QStringLiteral("quality"));
    if (number(q,&quality,1,6)) e.quality = int(quality);
    e.group = e.changeable ? QStringLiteral("固定万变") : category(int(id),e.name);
    e.fields.append(numeric(QStringLiteral("定义 ID"),id < 0 ? QJsonValue(QString::number(id)) : QJsonValue(id),-2));
    if (equipped) e.fields.append(numeric(QStringLiteral("等级"),part(f,1)));
    if (id != 0) e.fields.append(number(q,&quality,1,6) ? field(QStringLiteral("品质"),QString::number(quality)) : field(QStringLiteral("品质"),QStringLiteral("待确认"),DetailKnowledge::Unknown));
    if (e.changeable) e.fields.append(field(QStringLiteral("万变来源"),original.value(QStringLiteral("name")).toString(QStringLiteral("万变星神"))));
    if (e.changeable && !equipped) e.fields.append(field(QStringLiteral("计数"),QStringLiteral("万变星神，不计入普通栏位")));
    return e;
  }
  void flushRelations() {
    auto& p = result->pages[page];
    for (const auto& pending : pendingRelations) {
      if (!pending.problem.isEmpty()) {
        auto invalid = invalidEntry(pending.embedded.value(QStringLiteral("n")).toString(QStringLiteral("关系条目")),pending.problem);
        invalid.group = pending.label; p.entries.append(std::move(invalid)); p.state = DetailKnowledge::Invalid; continue;
      }
      const auto summary = summaries.value(pending.id); QJsonObject related = summary.fields;
      static const QStringList keys{QStringLiteral("id"),QStringLiteral("r"),QStringLiteral("ri"),QStringLiteral("fr"),QStringLiteral("n"),QStringLiteral("customName"),QStringLiteral("lv"),QStringLiteral("zdl"),QStringLiteral("xzdl"),QStringLiteral("rt"),QStringLiteral("_metaOriginalName"),QStringLiteral("_metaAttributes"),QStringLiteral("_metaJobs"),QStringLiteral("_metaEra"),QStringLiteral("_metaRaceId")};
      for (const auto& key : keys) if (pending.embedded.contains(key)) related.insert(key,pending.embedded.value(key));
      const int race = petRaceId(related) > 0 ? petRaceId(related) : pending.race;
      if (petRaceId(related) <= 0 && race > 0) related.insert(QStringLiteral("ri"),race);
      DetailEntry e; e.group = pending.label; e.name = related.value(QStringLiteral("customName")).toString();
      if (e.name.isEmpty()) e.name = related.value(QStringLiteral("n")).toString(); if (e.name.isEmpty()) e.name = metadata.petName(race); if (e.name.isEmpty()) e.name = QStringLiteral("未知精灵");
      const bool badId = pending.id <= 0 && pending.embedded.contains(QStringLiteral("id"));
      if (badId) { e.state = DetailKnowledge::Invalid; e.problem = QStringLiteral("嵌入实例 ID 无效"); p.state = DetailKnowledge::Invalid; }
      // The expanded carry list shows names only. Keep the same background
      // identity resolution without retaining unused per-candidate power data.
      if (requested == DetailSection::CarryRelations && pending.label == QStringLiteral("可以携带")) {
        p.entries.append(std::move(e)); continue;
      }
      e.fields.append(pending.id > 0 ? field(QStringLiteral("实例 ID"),QString::number(pending.id)) : field(QStringLiteral("实例 ID"),QStringLiteral("待确认"),badId ? DetailKnowledge::Invalid : DetailKnowledge::Unknown));
      e.fields.append(field(QStringLiteral("时代"),metadata.resolvedEra(related)));
      e.fields.append(numeric(QStringLiteral("等级"),related.value(QStringLiteral("lv"))));
      e.fields.append(numeric(QStringLiteral("当前战力"),related.value(QStringLiteral("zdl"))));
      e.fields.append(numeric(QStringLiteral("极限战力"),related.value(QStringLiteral("xzdl"))));
      p.entries.append(std::move(e));
    }
    pendingRelations.clear();
    if (requested == DetailSection::CarryRelations) summaries.clear();
  }
  bool processRelation() {
    while (relationGroup < groups.size() && sourceIndex >= groups[relationGroup].values.size()) { ++relationGroup; sourceIndex = 0; seen.clear(); }
    if (relationGroup >= groups.size()) return false;
    const auto& group = groups[relationGroup]; const auto raw = group.values[sourceIndex++]; const auto embedded = raw.toObject(); const qint64 id = relationId(raw);
    if (id > 0 && seen.contains(id)) return true;
    if (id > 0) seen.insert(id);
    auto& p = result->pages[page]; const bool visible = wanted(p.totalItems++);
    if (!visible) return true;
    if (!group.identityProblem.isEmpty()) { pendingRelations.append({embedded,0,0,group.label,group.identityProblem}); return true; }
    if (id == 0 && embedded.isEmpty()) { pendingRelations.append({{},0,0,group.label,QStringLiteral("实例 ID 无效，不能关联另一只精灵")}); return true; }
    pendingRelations.append({embedded,id,petRaceId(embedded) > 0 ? petRaceId(embedded) : group.fallbackRace,group.label});
    if (id > 0 && !summaries.contains(id) && !needs.contains(id)) needs.append(id);
    return true;
  }
  DetailStep unit() {
    if (!initialized) return initialize() ? DetailStep::More : DetailStep::Failed;
    if (page >= result->pages.size()) { complete = true; return DetailStep::Complete; }
    if (!sectionInitialized) beginSection();
    auto& p = result->pages[page]; bool more = false;
    if (p.section == DetailSection::SummonRelations || p.section == DetailSection::CarryRelations) {
      if (pendingRelations.size() >= 32 || (relationGroup >= groups.size() && !pendingRelations.isEmpty())) {
        if (!needs.isEmpty()) return DetailStep::NeedSummaries; flushRelations();
      }
      more = processRelation();
      if (!more && !pendingRelations.isEmpty()) { if (!needs.isEmpty()) return DetailStep::NeedSummaries; flushRelations(); }
    } else if (p.section == DetailSection::StargodBackpack) {
      if (sourceIndex < array.size()) { append(star(array[sourceIndex++],false)); more = true; }
    } else if (!encoded.isEmpty() && tokenStart < encoded.size()) {
      const QChar separator = p.section == DetailSection::Badges ? QLatin1Char('|') : QLatin1Char('#');
      const int end = qMin(int(encoded.size()), tokenPosition + 4096);
      while (tokenPosition < end && encoded[tokenPosition] != separator && !(p.section == DetailSection::Astrolabe && encoded[tokenPosition] == QLatin1Char('|'))) ++tokenPosition;
      if (tokenPosition < encoded.size() && tokenPosition == end) return DetailStep::More;
      const int length = tokenPosition - tokenStart;
      if (length > limits.maximumTextUnits) { if (p.section == DetailSection::Astrolabe) activatedCountKnown = selectedCountKnown = false; append(invalidEntry(QStringLiteral("编码条目"),QStringLiteral("条目过长；原文保留在原始数据页"))); }
      else { const QString token = encoded.mid(tokenStart,length); append(p.section == DetailSection::Badges ? badge(token) : p.section == DetailSection::Astrolabe ? astrolabe(token) : star(token,true)); }
      tokenStart = ++tokenPosition; more = true;
    }
    if (requested == DetailSection::CarryRelations && p.totalItems > quint64(limits.maximumExpandedCarryItems)) {
      failure = QStringLiteral("可携带列表超过支持的完整展开数量（%1 项）").arg(limits.maximumExpandedCarryItems);
      return DetailStep::Failed;
    }
    if (!more) {
      if (p.section == DetailSection::Astrolabe && p.pageIndex == 0 && !p.entries.isEmpty()) {
        for (auto& statusField : p.entries[0].fields) {
          if (statusField.label == QStringLiteral("点亮数量"))
            statusField = field(QStringLiteral("点亮数量"),activatedCountKnown ? QString::number(activatedCount) : QStringLiteral("待确认"),activatedCountKnown ? DetailKnowledge::Known : DetailKnowledge::Unknown);
          else if (statusField.label == QStringLiteral("选中数量"))
            statusField = field(QStringLiteral("选中数量"),selectedCountKnown ? QString::number(selectedCount) : QStringLiteral("待确认"),selectedCountKnown ? DetailKnowledge::Known : DetailKnowledge::Unknown);
        }
      }
      ++page; sectionInitialized = false;
    }
    return DetailStep::More;
  }
};

PetDetailPreparation::PetDetailPreparation(FrozenDetailInputs inputs, DetailSection section, int pageIndex, DetailPreparationLimits limits)
    : impl_(std::make_unique<Impl>(std::move(inputs),section,pageIndex,limits)) {}
PetDetailPreparation::~PetDetailPreparation() = default;
DetailStep PetDetailPreparation::step(const std::atomic_bool& cancelled) {
  if (cancelled.load()) return DetailStep::Cancelled;
  if (!impl_->failure.isEmpty()) return DetailStep::Failed;
  if (impl_->complete) return DetailStep::Complete;
  QElapsedTimer timer; timer.start(); DetailStep result = DetailStep::More;
  for (int work = 0; work < qMax(1,impl_->limits.sliceItems); ++work) {
    if (cancelled.load()) { result = DetailStep::Cancelled; break; }
    result = impl_->unit(); ++impl_->progress.workItems;
    if (result != DetailStep::More || timer.elapsed() >= qMax(1,impl_->limits.sliceMilliseconds)) break;
  }
  const qint64 span = timer.nsecsElapsed(); impl_->progress.activeNanoseconds += span; impl_->progress.maximumSliceNanoseconds = qMax(span,impl_->progress.maximumSliceNanoseconds);
  impl_->progress.resultBytes = preparedPetDetailRetainedBytes(*impl_->result);
  impl_->progress.retainedWorkingBytes = impl_->progress.resultBytes + quint64(impl_->seen.size()) * 64 + quint64(impl_->summaries.size()) * 4096 + 8192;
  const quint64 resultLimit = impl_->requested == DetailSection::CarryRelations ? impl_->limits.expandedCarryResultBytes : impl_->limits.resultBytes;
  if (impl_->progress.resultBytes > resultLimit || impl_->progress.retainedWorkingBytes > impl_->limits.workingBytes) { impl_->failure = QStringLiteral("detail preparation exceeds its bounded working/result capacity"); return DetailStep::Failed; }
  return result;
}
QVector<qint64> PetDetailPreparation::requestedSummaries() const { return impl_->needs; }
bool PetDetailPreparation::provideSummaries(QVector<DetailRelatedSummary> summaries) {
  if (summaries.size() > 32 || summaries.size() != impl_->needs.size()) return false;
  QSet<qint64> seen;
  for (const auto& summary : summaries) if (!impl_->needs.contains(summary.instanceId) || seen.contains(summary.instanceId) || !detailRelatedSummaryValid(summary,&impl_->failure)) return false; else seen.insert(summary.instanceId);
  for (const auto& summary : summaries) {
    impl_->summaries.insert(summary.instanceId,summary);
    impl_->result->version.related.append({summary.instanceId,summary.revision,summary.present});
    impl_->result->version.relatedSummaryRevision = qMax(impl_->result->version.relatedSummaryRevision,summary.revision);
  }
  impl_->needs.clear(); return true;
}
std::unique_ptr<PreparedPetDetail> PetDetailPreparation::takeResult() { return impl_->complete ? std::move(impl_->result) : nullptr; }
DetailPreparationProgress PetDetailPreparation::progress() const { return impl_->progress; }
QString PetDetailPreparation::error() const { return impl_->failure; }
