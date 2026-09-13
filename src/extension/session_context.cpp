#include "session_context.h"

bool SessionSourceEvidence::verified() const {
  return identityVerified && !account.isEmpty() && !sourceId.isEmpty() &&
         sourceEpoch != 0 && !evidenceReference.isEmpty();
}

bool SessionSourceEvidence::sameSource(const SessionSourceEvidence& other) const {
  return verified() && other.verified() && account == other.account &&
         sourceId == other.sourceId && sourceEpoch == other.sourceEpoch;
}

bool SessionContext::canPersist() const {
  return state == SessionConnectionState::Active && source.verified() &&
         source.account == account;
}

bool SessionContext::accepts(const InboundEnvelope& envelope) const {
  return canPersist() && envelope.capturedSessionEpoch == epoch &&
         source.sameSource(envelope.source);
}
