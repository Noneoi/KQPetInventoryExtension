#pragma once
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>

// Test-only definitions. These numeric IDs/names are NOT game data.
inline QJsonObject battleReplayTestCatalog() {
  return QJsonDocument::fromJson(R"({"skills":{"999999999":{"action":"3","attackType":"近身","playOrderType":"0"}},
    "buffs":{"99":{"nm":"synthetic normal","hd":"1"},"100":{"nm":"synthetic wait","buffWaitType":"1"},"101":{"nm":"synthetic marker"}},
    "execution":{"1":{"type":"1"},"-1":{"type":"2"}},
    "bufferExecution":{"88":{"actionResId":"0"}}})").object();
}

// Deliberately synthetic, small adversarial protocol fixture; not a game sample.
inline QByteArray battleReplayFixture() {
  return R"({"extra":{"empty":[],"zero":0},"br":{"ver":1,"win":-1,
    "or":{"team":[
      {"id":0,"players":[{"name":"tester","cts":[
        {"id":7,"name":"synthetic A","x":0,"y":0,"hp":100,"maxHp":100,"vigour":130,"shp":50,"bfs":"99,99"},
        {"id":8,"name":"synthetic reserve","x":0,"y":0,"hp":80,"maxHp":80,"ics":true,"is":false}
      ]}]},
      {"id":1,"players":[{"name":"tester","cts":[
        {"id":7,"name":"synthetic B","x":0,"y":0,"hp":200,"maxHp":200,"vigour":60,"shp":70}
      ]}]}
    ]},"fr":[{"pan":"tester","pbn":"tester","pr":{"ml":[],"dl":[]},"rl":[
      {"cti":0,"cpn":"tester","cci":7,"si":999999999,"csn":1,"ses":-1,
       "cn":{"ahp":100,"avg":150,"ashp":0},
       "trl":[{"tti":1,"tpn":"tester","tci":7,"isse":false,
         "tn":{"ahp":180,"avg":60,"ashp":0,"bb_hps":"99_-20_1","ad_bfs":"1_99_2",
          "rl":[{"t":4,"o":2,"changeset":2,"pbid":88,"prob":0,"proa":2,"vc":2,"mp":10,"isse":false}]}}],
       "prl":[{"tti":0,"tpn":"tester","isse":false,"tn":{"ashp":0}}]},
      {"cti":0,"cpn":"tester","cci":7,"si":999999999,"csn":2,"ses":1,
       "cn":{"ahp":90,"avg":160,"ashp":0,"att_shps":"-10",
        "rl":[{"t":3,"ti":0,"pn":"tester","ci":8,"o":2,"changeset":2},
              {"t":777,"o":7,"changeset":2,"unknownPayload":{"x":"preserve"}}]},
       "trl":[{"tti":1,"tpn":"tester","tci":7,"isse":true,"tn":{}}],"prl":[]}
    ]},{"isf":false,"pr":{"ml":[],"dl":[{"teamId":1,"player":"tester"}]},"rl":[]}]}})";
}

inline QByteArray battleReplayStateFixture() {
  return R"({"br":{"win":0,"or":{"team":[
    {"id":0,"players":[{"name":"tester","ppbf":{"pbid":77,"cp":2,"mp":10},"pcbfs":"88,88","cts":[
      {"id":7,"name":"synthetic source","x":0,"y":0,"hp":100,"maxHp":100,"shp":50,"vigour":100,"bfs":"99,99"}]}]},
    {"id":1,"players":[{"name":"tester","cts":[
      {"id":7,"name":"synthetic target","x":0,"y":0,"hp":200,"maxHp":200,"shp":70,"vigour":60}]}]}
  ]},"fr":[{"rl":[{"cti":0,"cpn":"tester","cci":7,"csn":1,"ses":1,"si":999999999,
    "cn":{"bb_shps":"99_10_9","bb_hps":"99_-5_20","bd_bfs":"0_99_9,1_101_20",
      "att_hps":"-10","att_shps":"-7","att_vgs":"5","sb_hps":"-8","sb_shps":"-6",
      "ab_shps":"99_-100_30,99_20_40","ad_bfs":"1_99_30,0_99_40","ahp":83,"avg":150,"ashp":999,
      "rl":[{"t":2,"o":2,"changeset":40,"old_curv":83,"new_curv":77,"old_maxv":100,"new_maxv":150}]},
    "trl":[{"tti":1,"tpn":"tester","tci":7,"isse":true,"issb":true,"ishit":false,
      "tn":{"bb_shps":"99_-3_1,99_2_2","bd_bfs":"1_100_1",
        "att_hps":"-300,150","att_shps":"-11","sb_hps":"-2","sb_shps":"-4",
        "ab_shps":"99_1_8","ahp":180,"avg":60,"ashp":0,
        "rl":[{"t":100,"o":2,"changeset":8}]}}],"prl":[]}
  ]}]}})";
}

inline QJsonObject battleReplayBattlefieldCatalog() {
  auto result = battleReplayTestCatalog();
  result["pets"] = QJsonObject{{"9001", QJsonObject{{"name", "synthetic form A"}, {"maxPower", 100}}},
    {"9002", QJsonObject{{"name", "synthetic form B"}, {"maxPower", 150}}}};
  auto pets = result["pets"].toObject();
  for (const auto& id : {QStringLiteral("9001"), QStringLiteral("9002")}) {
    auto pet = pets[id].toObject();
    pet["identity"] = QJsonObject{{"status", "native"}, {"canonicalName", pet["name"]}};
    pets[id] = pet;
  }
  result["pets"] = pets;
  auto skills = result["skills"].toObject();
  skills["999999998"] = QJsonObject{{"action", "3"}, {"attackType", "远程"}, {"skillBaseType", "召唤技"}};
  result["skills"] = skills;
  return result;
}

inline QByteArray battleReplayBattlefieldFixture() {
  return R"({"br":{"win":0,"or":{"team":[
    {"id":0,"players":[{"name":"tester","x":0,"y":0,"cts":[
      {"id":7,"name":"synthetic source","raceId":9001,"faceId":9001,"nskill":11,"uskills":21,"suskill":31,"attr":"1","x":0,"y":0,"hp":100,"maxHp":100,"shp":20,"vigour":120},
      {"id":8,"name":"synthetic summon","raceId":9001,"x":0,"y":0,"hp":50,"maxHp":50,"ics":true,"is":false},
      {"id":9,"name":"synthetic clone","raceId":9001,"x":1,"y":0,"hp":50,"maxHp":50,"ias":true,"ibs":false}]},
      {"name":"reserve-player","x":1,"y":0,"cts":[{"id":7,"name":"synthetic other formation","x":0,"y":0,"hp":80,"maxHp":80}]}]},
    {"id":1,"players":[{"name":"foe","x":0,"y":0,"cts":[{"id":7,"name":"synthetic foe","x":0,"y":0,"hp":200,"maxHp":200}]}]}
  ]},"fr":[{"pan":"tester","pbn":"foe","rl":[
    {"cti":0,"cpn":"tester","cci":7,"si":999999999,"csn":1,"ses":1,
     "cn":{"rl":[{"t":1,"o":1,"changeset":1,"or":9001,"of":9001,"cr":9002,"cf":9003,"cn":12,"cu":"0,999","scu":45,"ca":"2,3"},
       {"t":5,"o":2,"changeset":1,"ti":1,"pn":"deliberately-wrong-host","ci":9},
       {"t":3,"o":2,"changeset":1,"ti":0,"pn":"tester","ci":8},
       {"t":6,"o":2,"changeset":2,"ca":"4"}]},
     "trl":[{"tti":1,"tpn":"foe","tci":7,"isse":false,"tn":{"bb_hps":"99_-200_1","ab_hps":"99_200_2","ahp":200}}]},
    {"cti":0,"cpn":"tester","cci":7,"si":999999998,"csn":2,"ses":1,"cn":{},
     "trl":[{"tti":0,"tpn":"tester","tci":8,"isse":true,"tn":{}}]}
  ]},{"pan":"tester","pbn":"foe","pr":{"ml":[{"teamId":0,"player":"tester","x_to":2,"y_to":1}],"dl":[]},"rl":[]},
     {"pr":{"ml":[],"dl":[{"teamId":0,"player":"tester"}]},"rl":[]}]}})";
}
