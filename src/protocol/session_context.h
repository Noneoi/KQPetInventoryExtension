#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <memory>

// These levels describe evidence, not how many local retries have occurred.
enum class PacketCorrelationStrength {
  PassiveObserved, CommandObserved, EntityCorrelated, ServerCorrelated
};
enum class SessionConnectionState {
  Disconnected, Authenticating, Active, Uncertain, Closing
};

struct SessionSourceEvidence {
  QString account;
  QString sourceId;
  quint64 sourceEpoch = 0;
  bool identityVerified = false;
  // A reviewed host ordering barrier must establish this; a local counter or
  // elapsed delay is not such a barrier. No production adapter currently does.
  bool orderingVerified = false;
  QString evidenceReference;

  bool verified() const;
  bool sameSource(const SessionSourceEvidence& other) const;
};

struct InboundEnvelope {
  // Shared queue reservation survives queued copies until Core has consumed
  // the envelope. It contains no business state or thread-affine objects.
  std::shared_ptr<void> retention;
  quint64 receiveSequence = 0;
  qint64 receivedMonotonicMs = 0;
  quint64 capturedSessionEpoch = 0;
  SessionSourceEvidence source;
  // Proof supplied at capture that this observation follows the applicable
  // host send/order barrier, not merely the extension's request generation.
  bool orderedObservation = false;
  QString orderEvidenceToken;
  QString method;
  QString payload;
};

struct SessionContext {
  QString account;
  quint64 epoch = 0;
  SessionConnectionState state = SessionConnectionState::Disconnected;
  SessionSourceEvidence source;
  QString uncertaintyReason;
  bool canPersist() const;
  bool accepts(const InboundEnvelope& envelope) const;
};

struct InventoryObservation {
  quint64 revision = 0;
  quint64 receiveSequence = 0;
  quint64 sessionEpoch = 0;
  QDateTime observedAt;
  bool complete = false;
  bool sourceVerified = false;
  // Host-proved order (verified source + ordering barrier). No production
  // adapter supplies this today.
  bool ordered = false;
  // Observed through the current, never-interrupted read stream of an
  // unverified session. This is the v1.3-level basis for a move: the lists
  // were freshly read, in order, by this extension in this session. It is not
  // proof that no stale packet could interleave around an account switch.
  bool continuousRead = false;

  void revokeWriteAuthority() { ordered = false; continuousRead = false; }
};

Q_DECLARE_METATYPE(PacketCorrelationStrength)
Q_DECLARE_METATYPE(SessionConnectionState)
Q_DECLARE_METATYPE(SessionSourceEvidence)
Q_DECLARE_METATYPE(InboundEnvelope)
