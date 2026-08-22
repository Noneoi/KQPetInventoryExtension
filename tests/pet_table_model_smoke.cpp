#include "pet_filter_proxy_model.h"
#include "pet_table_model.h"

#include <QCoreApplication>
#include <QJsonObject>

#include <cstdio>

namespace {

bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  const QJsonObject qiankun{{QStringLiteral("id"), 1001},
                            {QStringLiteral("r"), 7516},
                            {QStringLiteral("n"), QStringLiteral("[灵初]五行御玄·乾坤")},
                            {QStringLiteral("rt"), 26},
                            {QStringLiteral("lv"), 120},
                            {QStringLiteral("zdl"), 30000},
                            {QStringLiteral("xzdl"), 30000},
                            {QStringLiteral("_position"), 2},
                            {QStringLiteral("_warehouseGroup"), QStringLiteral("normal")}};
  const QJsonObject annihilation{{QStringLiteral("id"), 1002},
                                 {QStringLiteral("r"), 7135},
                                 {QStringLiteral("n"), QStringLiteral("逆时空·湮灭神女")},
                                 {QStringLiteral("lv"), 100},
                                 {QStringLiteral("zdl"), 18000},
                                 {QStringLiteral("xzdl"), 24000},
                                 {QStringLiteral("_position"), 1},
                                 {QStringLiteral("_warehouseGroup"), QStringLiteral("elite")}};
  const QJsonObject unknown{{QStringLiteral("id"), 1003},
                            {QStringLiteral("r"), 990003},
                            {QStringLiteral("n"), QStringLiteral("未来测试精灵")},
                            {QStringLiteral("_position"), 3},
                            {QStringLiteral("_warehouseGroup"), QStringLiteral("normal")}};

  PetTableModel model(PetTableModel::Location::Warehouse);
  model.setPets({qiankun, annihilation, unknown});
  bool ok = true;
  ok &= require(model.rowCount() == 3 && model.columnCount() == 7 &&
                    model.headerData(5, Qt::Horizontal).toString() ==
                        QStringLiteral("战斗力 / 极限战斗力") &&
                    model.index(0, 0).data(PetTableModel::InstanceIdRole).toLongLong() ==
                        1001 &&
                    model.index(1, 6).data().toString() == QStringLiteral("精英"),
                "table model did not expose the stable warehouse columns and roles");

  PetFilterProxyModel proxy;
  proxy.setSourceModel(&model);
  proxy.setQuery(QStringLiteral("乾坤"));
  ok &= require(proxy.rowCount() == 1 &&
                    proxy.index(0, 0).data(PetTableModel::InstanceIdRole).toLongLong() == 1001,
                "proxy query did not retain the matching pet");
  proxy.setQuery({});
  proxy.setJobFilter(QStringLiteral("神召唤师"));
  ok &= require(proxy.rowCount() == 1 &&
                    proxy.index(0, 0).data(PetTableModel::InstanceIdRole).toLongLong() == 1001,
                "proxy profession filter did not use resolved dual-category metadata");
  proxy.setJobFilter({});
  proxy.setSortMode(PetFilterProxyModel::SortMode::BattlePower, false);
  ok &= require(proxy.rowCount() == 3 &&
                    proxy.index(0, 0).data(PetTableModel::InstanceIdRole).toLongLong() == 1001 &&
                    proxy.index(1, 0).data(PetTableModel::InstanceIdRole).toLongLong() == 1002 &&
                    proxy.index(2, 0).data(PetTableModel::InstanceIdRole).toLongLong() == 1003,
                "proxy descending sort did not keep missing values last");

  QJsonObject updated = annihilation;
  updated.insert(QStringLiteral("customName"), QStringLiteral("已更新昵称"));
  ok &= require(model.updatePet(updated) &&
                    model.index(model.rowForInstanceId(1002), 0).data().toString() ==
                        QStringLiteral("已更新昵称"),
                "incremental model update did not emit the new row state");

  if (!ok) return 1;
  std::fprintf(stdout, "PASS: pet table model and filter proxy\n");
  return 0;
}
