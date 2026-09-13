#include "inventory_projection.h"
#include <QCoreApplication>
#include <QThread>
#include <cstdio>
#include <thread>

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  InventoryProjection projection;
  int resets = 0, details = 0, sessions = 0, metadataChanges = 0;
  bool ownerThread = true;
  QObject::connect(&projection, &InventoryProjection::dataChanged, [&] {
    ++resets; ownerThread = ownerThread && QThread::currentThread() == app.thread();
  });
  QObject::connect(&projection, &InventoryProjection::detailChanged, [&] { ++details; });
  QObject::connect(&projection, &InventoryProjection::accountSessionChanged, [&] { ++sessions; });
  QObject::connect(&projection, &InventoryProjection::metadataChanged, [&](quint64) {
    ++metadataChanges; ownerThread = ownerThread && QThread::currentThread() == app.thread();
  });
  auto first = std::make_shared<InventoryViewSnapshot>();
  first->publication = 1; first->account = QStringLiteral("A"); first->sessionEpoch = 1;
  first->membershipChanged = true;
  first->backpack.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("1")}});
  std::thread publisher([&] { projection.publish(first); }); publisher.join();
  QCoreApplication::processEvents();
  bool ok = ownerThread && resets == 1 && sessions == 1 && !projection.backpackPet(1).isEmpty();
  for (quint64 revision = 2; revision <= 1001; ++revision) {
    auto update = std::make_shared<InventoryViewSnapshot>(*first);
    update->publication = revision; update->membershipChanged = false;
    update->details.insert(1, QJsonObject{{QStringLiteral("lv"), int(revision)}});
    update->changedDetails.insert(1);
    projection.publish(update);
  }
  QCoreApplication::processEvents();
  ok = ok && resets == 1 && details == 1 && projection.detailFor(1).value(QStringLiteral("lv")).toInt() == 1001;
  projection.publish(first); QCoreApplication::processEvents();
  ok = ok && projection.snapshot()->publication == 1001;
  auto other = std::make_shared<InventoryViewSnapshot>();
  other->publication = 1002; other->account = QStringLiteral("B"); other->sessionEpoch = 2;
  projection.publish(other); QCoreApplication::processEvents();
  ok = ok && projection.accountKey() == QStringLiteral("B") && projection.backpackPet(1).isEmpty() &&
       projection.detailFor(1).isEmpty() && sessions == 2;
  const int oldResets = resets;
  auto metadata = std::make_shared<PetDetailCatalogSnapshot>(); metadata->revision = 1;
  auto metadataOnly = std::make_shared<InventoryViewSnapshot>(*other);
  metadataOnly->publication = 1003; metadataOnly->metadata = metadata;
  projection.publish(metadataOnly);
  auto newestMetadata = std::make_shared<PetDetailCatalogSnapshot>(); newestMetadata->revision = 2;
  metadataOnly = std::make_shared<InventoryViewSnapshot>(*metadataOnly);
  metadataOnly->publication = 1004; metadataOnly->metadata = newestMetadata;
  projection.publish(metadataOnly); QCoreApplication::processEvents();
  ok = ok && metadataChanges == 1 && projection.metadataSnapshot() == newestMetadata && resets == oldResets && ownerThread;
  InventoryProjection burst;
  auto many = std::make_shared<InventoryViewSnapshot>();
  many->publication = 2000; many->account = "burst"; many->sessionEpoch = 1; many->membershipChanged = true;
  for (qint64 id = 1; id <= 1000; ++id) {
    many->warehouse.append(QJsonObject{{"id", QString::number(id)}}); many->changedDetails.insert(id);
  }
  int delivered = 0, firstTurn = 0;
  QObject::connect(&burst, &InventoryProjection::detailChanged, [&] { ++delivered; });
  burst.publish(many);
  QMetaObject::invokeMethod(&burst, [&] { firstTurn = delivered; }, Qt::QueuedConnection);
  QCoreApplication::processEvents();
  ok = ok && firstTurn > 0 && firstTurn <= 32;
  for (int round = 0; round < 40 && delivered < 1000; ++round) QCoreApplication::processEvents();
  ok = ok && delivered == 1000;
  burst.publish(std::make_shared<InventoryViewSnapshot>(*many)); // Repeated publication is ignored.
  auto switched = std::make_shared<InventoryViewSnapshot>();
  switched->publication = 2001; switched->account = "new burst account"; switched->sessionEpoch = 2;
  burst.publish(switched); QCoreApplication::processEvents();
  ok = ok && burst.accountKey() == switched->account && burst.detailFor(1).isEmpty();
  auto* transient = new InventoryProjection;
  QObject::connect(transient, &InventoryProjection::accountSessionChanged, [transient] { delete transient; });
  transient->publish(many); QCoreApplication::processEvents();
  InventoryProjection trust;
  int trustNotifications = 0;
  QObject::connect(&trust, &InventoryProjection::dataChanged, [&] { ++trustNotifications; });
  auto verified = std::make_shared<InventoryViewSnapshot>();
  verified->publication = 3000; verified->account = "trust"; verified->sessionEpoch = 1;
  verified->sourceVerified = true; verified->sessionState = SessionConnectionState::Active;
  trust.publish(verified); QCoreApplication::processEvents();
  ok = ok && trust.currentSourceVerified();
  auto uncertain = std::make_shared<InventoryViewSnapshot>(*verified);
  uncertain->publication = 3001; uncertain->sessionState = SessionConnectionState::Uncertain;
  trust.publish(uncertain); QCoreApplication::processEvents();
  ok = ok && !trust.currentSourceVerified() && trustNotifications == 2;
  std::puts(ok ? "PASS: GUI ownership, bounded snapshot coalescing, detail-only update and account isolation"
               : "FAIL: inventory projection contract");
  return ok ? 0 : 1;
}
