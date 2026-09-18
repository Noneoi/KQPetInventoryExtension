#pragma once
// NOT a redundant forwarder. src/domain/recommendation_engine.h has the same
// file name, so a bare #include from a target with src/extension on its path
// would silently resolve to the Domain header instead of the adapter that
// actually declares RecommendationEngine. Deleting this file does not fail to
// compile as a missing include; it fails later, as unknown identifiers.
#include "../application/recommendation_adapter.h"
