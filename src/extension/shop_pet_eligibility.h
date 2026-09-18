#pragma once
// NOT a redundant forwarder — see recommendation_engine.h in this directory.
// src/domain/shop_pet_eligibility.h shares this file name and declares
// different symbols, so removing this header silently redirects the include
// there and analyzeShopPetEligibility stops resolving.
#include "../application/shop_legacy_adapters.h"
