#include "packet_contract.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <cstdio>

namespace {
bool check(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}
QString json(const QJsonObject& object) {
  return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}
}

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  bool ok = true;
  struct Example { QString extension; QString command; QString parameters; };
  const QList<Example> approved{
      {QStringLiteral("PJXExtension"), QStringLiteral("2_1_10"), QStringLiteral("{}")},
      {QStringLiteral("PJXExtension"), QStringLiteral("2_1_S"), QStringLiteral("null")},
      {QStringLiteral("PJXExtension"), QStringLiteral("2_1_R"), QStringLiteral("{\"pi\":123}")},
      {QStringLiteral("PJXExtension"), QStringLiteral("2_1_11"), QStringLiteral("{\"pps\":\"1#2\",\"ppt\":0}")},
      {QStringLiteral("PJXExtension"), QStringLiteral("2_2_10"), QStringLiteral("{}")},
      {QStringLiteral("TimelinessActExtension"), QStringLiteral("1008_20260313_es_0"), QStringLiteral("{}")},
      {QStringLiteral("TimelinessActExtension"), QStringLiteral("1008_20260313_es_2"), QStringLiteral("{\"i\":10}")},
      {QStringLiteral("SimpleActExtension"), QStringLiteral("1019_0"), QStringLiteral("{\"ai\":5747}")},
      {QStringLiteral("TimelinessActExtension"), QStringLiteral("1008_20251231_cvgv2_0"), QStringLiteral("null")},
      {QStringLiteral("TimelinessActExtension"), QStringLiteral("1008_20250627_hdc_0"), QStringLiteral("null")},
      {QStringLiteral("null"), QStringLiteral("1039_3_0"), QStringLiteral("null")},
      {QStringLiteral("null"), QStringLiteral("1039_4_0"), QStringLiteral("null")},
      {QStringLiteral("MaterialExtension"), QStringLiteral("3_11"), QStringLiteral("{}")},
      {QStringLiteral("PJXExtension"), QStringLiteral("2_32_0"), QStringLiteral("null")},
      {QStringLiteral("TimelinessActExtension"), QStringLiteral("1008_20170623_dt_0"), QStringLiteral("null")},
      {QStringLiteral("null"), QStringLiteral("1037_0"), QStringLiteral("{\"ids\":\"lights\"}")},
      {QStringLiteral("TimelinessActExtension"), QStringLiteral("1008_20220603_swa_0_0"), QStringLiteral("null")},
      {QStringLiteral("TimelinessActExtension"), QStringLiteral("1008_20190531_gbt_1"), QStringLiteral("null")},
      {QStringLiteral("PJXExtension"), QStringLiteral("2_36_1"), QStringLiteral("null")},
      {QStringLiteral("XiaoMoEvolveExtension"), QStringLiteral("110_123_0"), QStringLiteral("null")},
      {QStringLiteral("TimelinessActExtension"), QStringLiteral("1008_20260522_nf_0"), QStringLiteral("{\"un\":-1}")}};
  QSet<QString> covered;
  for (const Example& example : approved) {
    QString error;
    covered.insert(example.command);
    ok &= check(PacketContracts::validateOutbound(example.extension, example.command,
                                                  example.parameters, &error) && error.isEmpty(),
                "documented command/extension/parameter shape was rejected");
    ok &= check(!PacketContracts::validateOutbound(example.extension + QStringLiteral("Extra"),
                                                   example.command, example.parameters),
                "a registered command authorized the wrong extension");
    ok &= check(!PacketContracts::validateOutbound(example.extension, example.command,
                                                   QStringLiteral("{\"extra\":1}")),
                "extra parameters were accepted");
    ok &= check(!PacketContracts::validateOutbound(example.extension, example.command,
                                                   QStringLiteral("[]")) &&
                !PacketContracts::validateOutbound(example.extension, example.command,
                                                   QStringLiteral("prefix{}suffix")),
                "wrong top-level type or malformed JSON was accepted");
  }
  for (const PacketContract& contract : PacketContracts::all()) {
    if (contract.activeSendAllowed)
      ok &= check(covered.contains(contract.command), "active command has no frozen outbound example");
    else
      ok &= check(!PacketContracts::validateOutbound(contract.extension, contract.command,
                                                     QStringLiteral("null")),
                  "passive-only command was actively authorized");
  }
  ok &= check(approved.size() == 21 && covered.size() == 21,
              "the active protocol whitelist changed without updating its contract test");
  const auto* sourceInventory = PacketContracts::find(QStringLiteral("2_32_0"));
  ok &= check(sourceInventory && sourceInventory->access == PacketAccess::Read &&
      sourceInventory->snapshot == PacketSnapshotSemantics::Complete &&
      !PacketContracts::validateOutbound(QStringLiteral("PJXExtension"), QStringLiteral("2_32_0"), QStringLiteral("{}")) &&
      !PacketContracts::validateOutbound(QStringLiteral("PJXExtension"), QStringLiteral("2_32_10"), QStringLiteral("null")),
      "source inventory lost its exact read-only contract or admitted the ambiguous equipment write command");
  for (const QString& command : {QStringLiteral("16_6_0"), QStringLiteral("100_13_0"),
                                 QStringLiteral("1019_1"),QStringLiteral("1008_20260313_es_3"),
                                 QStringLiteral("100_2_0"), QStringLiteral("execute"), QString()})
    ok &= check(!PacketContracts::validateOutbound(QStringLiteral("null"), command,
                                                   QStringLiteral("null")),
                "unregistered/removed protocol was authorized");
  ok &= check(!PacketContracts::validateOutbound(QStringLiteral("SimpleActExtension"),QStringLiteral("1019_0"),QStringLiteral("{\"ai\":5747,\"bi\":1}")) &&
      !PacketContracts::validateOutbound(QStringLiteral("TimelinessActExtension"),QStringLiteral("1008_20260313_es_2"),QStringLiteral("{\"i\":0}")),
      "activity read accepted an action parameter or invalid source identifier");

  for (const QJsonValue& value : QList<QJsonValue>{QJsonValue(QJsonValue::Null), false,
       QStringLiteral("123"), -1, 0, 1.25, 9007199254740992.0, QJsonArray{123}})
    ok &= check(!PacketContracts::validateOutbound(QStringLiteral("PJXExtension"),
        QStringLiteral("2_1_R"), json({{QStringLiteral("pi"), value}})),
        "invalid instance-id type, fractional value or unsafe numeric precision was accepted");
  ok &= check(PacketContracts::validateOutbound(QStringLiteral("PJXExtension"),
      QStringLiteral("2_1_R"), QStringLiteral("{\"pi\":9007199254740991}")),
      "largest exact JSON integer was rejected");
  for (const QJsonObject& value : QList<QJsonObject>{
       {{QStringLiteral("pps"), QStringLiteral("1")}},
       {{QStringLiteral("pps"), QJsonArray{1}}, {QStringLiteral("ppt"), 0}},
       {{QStringLiteral("pps"), QStringLiteral("1")}, {QStringLiteral("ppt"), false}},
       {{QStringLiteral("pps"), QStringLiteral("1")}, {QStringLiteral("ppt"), QStringLiteral("0")}},
       {{QStringLiteral("pps"), QStringLiteral("1")}, {QStringLiteral("ppt"), 1}},
       {{QStringLiteral("pps"), QStringLiteral("1")}, {QStringLiteral("ppt"), 0}, {QStringLiteral("extra"), 0}}})
    ok &= check(!PacketContracts::validateOutbound(QStringLiteral("PJXExtension"),
        QStringLiteral("2_1_11"), json(value)), "invalid write parameter schema was accepted");
  ok &= check(!PacketContracts::validateOutbound(QStringLiteral("null"), QStringLiteral("1037_0"),
      QStringLiteral("{\"ids\":\"all\"}")) &&
      !PacketContracts::validateOutbound(QStringLiteral("TimelinessActExtension"),
      QStringLiteral("1008_20260522_nf_0"), QStringLiteral("{\"un\":\"-1\"}")),
      "wrong literal or numeric-string farm target was accepted");

  QStringList twelve;
  for (int id = 1; id <= 12; ++id) twelve.append(QString::number(id));
  const QString maximumSequence = twelve.join(QLatin1Char('#'));
  ok &= check(PacketContracts::validateFlash(QStringLiteral("batchpet"), maximumSequence) &&
      PacketContracts::validateFlash(QStringLiteral("batchpet"), QStringLiteral("9223372036854775807")),
      "valid 12-slot sequence or exact int64 string was rejected");
  for (const QString& bad : {QString(), QStringLiteral("0"), QStringLiteral("-1"),
       QStringLiteral("1#1"), QStringLiteral("1##2"), QStringLiteral("#1"), QStringLiteral("1#"),
       QStringLiteral("1.5"), QStringLiteral("1e2"), QStringLiteral("01"), QStringLiteral("+1"),
       QStringLiteral("1 2"), QStringLiteral("9223372036854775808"), maximumSequence + QStringLiteral("#13"),
       QStringLiteral("1');alert(1);//"), QStringLiteral("1\n2"), QStringLiteral("1|2")}) {
    ok &= check(!PacketContracts::validateFlash(QStringLiteral("batchpet"), bad),
                "unsafe, empty, duplicate or over-capacity Flash sequence was accepted");
    ok &= check(!PacketContracts::validateOutbound(QStringLiteral("PJXExtension"),
        QStringLiteral("2_1_11"), json({{QStringLiteral("pps"), bad}, {QStringLiteral("ppt"), 0}})),
        "socket write bypassed the Flash sequence rules");
  }
  for (const QString& method : {QStringLiteral("BatchPet"), QStringLiteral("senddata"),
                                QStringLiteral("batchpet.call"), QStringLiteral("execute"), QString()})
    ok &= check(!PacketContracts::validateFlash(method, QStringLiteral("1#2")),
                "Flash method outside the fixed whitelist was authorized");
  if (!ok) return 1;
  std::puts("PASS: fixed outbound commands, exact parameter shapes and bounded Flash sequences");
  return 0;
}
