# Pure value domain

This directory is an actual QtCore-only implementation boundary. Its public
headers include only other Domain headers, explicit QtCore value/measurement
headers and standard C++ facilities. It has no repository, controller, catalog
singleton, resource/file/network access, QWidget/QImage, Win32 adapter or
implicit business date. `QElapsedTimer` measures the existing cooperative work
budget; supplied catalog dates, facts and metadata determine business results.

The complete implementation set is:

```text
account_resource_view.cpp
asset_derivation.cpp
asset_snapshot_comparator.cpp
catalog_types.cpp
checked_json_numbers.cpp
compiled_shop_catalog.cpp
pet_identity.cpp
pet_move_policy.cpp
pet_power_calculator.cpp
prepared_shop_conditions.cpp
recommendation_engine.cpp
shop_actionability.cpp
shop_limit_facts.cpp
shop_pet_eligibility.cpp
```

Application keeps `AssetAnalyzer` input capture and controller-based summaries,
and the old global-metadata entry points in `recommendation_adapter.*` and
`shop_legacy_adapters.*`. The old extension headers forward to those adapters
where compatibility requires them; Domain never includes the compatibility
headers. Domain callers use `PreparedRecommendationEngine`, `RecommendationSession`
and `AssetDerivation`. The worker includes this directory's recommendation
header explicitly. Legacy `RecommendationEngine::generate` remains an
Application call and cannot be linked using Domain alone.

Catalog holders continue to own publication outside Domain. Their Good identity
and quota methods delegate the same Domain implementations used by the prepared
pipeline. PacketContracts keeps its original numeric API and delegates to
DomainNumeric; callers retain the exact signed-string, JSON precision and
overflow rules. Battle-power value types are shared directly with the UI view
model, without importing that display model back into Domain.

`tests/domain_core_smoke.cpp` runs without QCoreApplication or registered RCC.
Link it against Domain and QtCore with `/WHOLEARCHIVE` on MSVC, so unresolved
references in an otherwise unused Domain object cannot hide in a static archive.
It exercises numeric boundaries, frozen catalog data, strict quotas, duplicate
and malformed costs, pure recommendations/cancellation, unknown cultivation,
snapshot comparability and move restrictions/sequence rules. Existing broader
application and algorithm tests remain necessary; this is a boundary regression,
not a replacement for the full behavioral suite.

Run `python tools/check_domain_boundary.py --self-test` for include/API checks.
The script deliberately ignores comments and literals and includes negative
fixtures. It is an architectural guard, not a full C++ parser or a claim that
all possible dependency tricks can be recognized. Pair it with the standalone
whole-archive link and review of new public dependencies.
