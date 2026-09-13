#pragma once

#include "pet_repository.h"

#include <QJsonDocument>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

inline bool waitForRepositoryPersistence(PetRepository* repository, int timeoutMs = 2000) {
  QElapsedTimer timer;
  timer.start();
  while (repository->pendingPersistenceCount() && timer.elapsed() < timeoutMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
  return repository->pendingPersistenceCount() == 0;
}

inline bool waitForRepositoryIdle(PetRepository* repository, int timeoutMs = 5000) {
  QElapsedTimer timer;
  timer.start();
  const auto busy = [repository] {
    return repository->pendingPersistenceCount() || repository->pendingReadCount() || repository->cacheLoading();
  };
  while (busy() && timer.elapsed() < timeoutMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
  return !busy();
}

// Synthetic transport boundary for offline tests only. This explicitly
// asserts provenance/ordering that the production host has not yet proved.
inline InboundEnvelope verifiedFixtureEnvelope(PetRepository* repository,
                                              const QJsonObject& packet) {
  if (packet.value(QStringLiteral("_cmd")).toString() == QStringLiteral("21_1")) {
    SessionSourceEvidence source;
    source.account = packet.value(QStringLiteral("info")).toObject()
                         .value(QStringLiteral("n")).toVariant().toString();
    source.sourceId = QStringLiteral("offline-fixture-transport");
    source.sourceEpoch = repository->sessionGeneration() + 1;
    source.identityVerified = true;
    source.orderingVerified = true;
    source.evidenceReference = QStringLiteral("tests/protocol_test_support.h synthetic transport");
    repository->setSessionSourceEvidence(source);
  }
  InboundEnvelope envelope;
  envelope.receiveSequence = repository->lastInboundSequence() + 1;
  envelope.receivedMonotonicMs = static_cast<qint64>(envelope.receiveSequence);
  envelope.capturedSessionEpoch = repository->sessionGeneration();
  envelope.source = repository->sessionContext().source;
  envelope.orderedObservation = true;
  envelope.orderEvidenceToken = QStringLiteral("fixture-barrier-%1").arg(envelope.receiveSequence);
  envelope.method = QStringLiteral("recivedata");
  envelope.payload = QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact));
  return envelope;
}

inline void deliverVerifiedFixture(PetRepository* repository, const QJsonObject& packet) {
  repository->handleEnvelope(verifiedFixtureEnvelope(repository, packet));
}
