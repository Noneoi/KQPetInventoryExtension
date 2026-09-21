# Qt Core layers of the in-process extension, lowest first:
#   Domain -> Storage -> Diagnostics -> Protocol -> ReadViews/Application
# Every include is written relative to src/ (for example "domain/pet_era.h"),
# so these targets no longer publish per-directory include paths.

# --- Domain: pure QtCore values and rules (see src/domain/README.md) ---------
add_library(KQPetDomain STATIC
  src/domain/account_resource_view.cpp
  src/domain/activity_shop_observation.cpp
  src/domain/asset_derivation.cpp
  src/domain/asset_snapshot_comparator.cpp
  src/domain/catalog_types.cpp
  src/domain/checked_json_numbers.cpp
  src/domain/compiled_shop_catalog.cpp
  src/domain/local_stargod_count.cpp src/domain/local_stargod_count.h
  src/domain/pet_analysis_facts.cpp
  src/domain/pet_cultivation_requirements.cpp src/domain/pet_cultivation_requirements.h
  src/domain/pet_detail_preparation.cpp
  src/domain/pet_era.cpp
  src/domain/pet_identity.cpp
  src/domain/pet_metadata_view.cpp
  src/domain/pet_move_policy.cpp
  src/domain/pet_power_calculator.cpp
  src/domain/prepared_shop_conditions.cpp
  src/domain/recommendation_engine.cpp
  src/domain/shop_actionability.cpp
  src/domain/shop_limit_facts.cpp
  src/domain/shop_pet_eligibility.cpp)
target_link_libraries(KQPetDomain PUBLIC Qt6::Core)
target_compile_options(KQPetDomain PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(KQPetDomain PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_library(KQPet::Domain ALIAS KQPetDomain)

# --- Storage: single I/O thread, frozen write contexts, atomic commits -------
add_library(KQPetStorage STATIC
  src/storage/storage_service.cpp src/storage/storage_service.h
  src/storage/storage_write_context.cpp src/storage/storage_write_context.h
  src/storage/storage_types.h
  src/storage/diagnostic_store.cpp src/storage/diagnostic_store.h)
target_link_libraries(KQPetStorage PUBLIC Qt6::Core PRIVATE bcrypt)
target_compile_options(KQPetStorage PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# --- Diagnostics: build info, logger, target profile guard -------------------
add_library(KQPetDiagnostics STATIC
  src/diagnostics/build_info.cpp
  src/diagnostics/build_info.h
  src/diagnostics/diagnostic_logger.cpp
  src/diagnostics/diagnostic_logger.h
  src/diagnostics/target_compatibility_guard.cpp
  src/diagnostics/target_compatibility_guard.h
  src/diagnostics/target_profile.h
  src/diagnostics/target_profile_registry.cpp
  src/diagnostics/target_profile_registry.h
)
target_include_directories(KQPetDiagnostics PUBLIC "${KQPET_GENERATED_INCLUDE_DIR}")
target_compile_definitions(KQPetDiagnostics PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetDiagnostics PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetDiagnostics
  PUBLIC Qt6::Core KQPetCompatibility PRIVATE version PUBLIC KQPetStorage PRIVATE bcrypt)

# --- Protocol: packet contracts, session, inbound queue, outbound transport --
add_library(KQPetProtocol STATIC
  src/protocol/packet_contract.cpp
  src/protocol/packet_contract.h
  src/protocol/session_context.cpp
  src/protocol/session_context.h
)
target_compile_options(KQPetProtocol PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetProtocol PUBLIC KQPetDomain Qt6::Core)

add_library(KQPetInbound STATIC src/protocol/inbound_queue.cpp src/protocol/inbound_queue.h)
target_link_libraries(KQPetInbound PUBLIC KQPetProtocol Qt6::Core)
target_compile_options(KQPetInbound PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_library(KQPetTransport STATIC src/protocol/protocol_transport.cpp src/protocol/protocol_transport.h)
target_link_libraries(KQPetTransport PUBLIC KQPetProtocol Qt6::Core)
target_compile_options(KQPetTransport PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# --- Read views: the GUI's read-only ports and their thread-safe projections -
add_library(KQPetReadViews STATIC
  src/application/views/inventory_read_view.h
  src/application/views/inventory_projection.h src/application/views/inventory_projection.cpp
  src/application/views/analysis_read_view.h
  src/application/views/analysis_projection.h src/application/views/analysis_projection.cpp)
target_link_libraries(KQPetReadViews PUBLIC KQPetDomain Qt6::Core)
target_compile_options(KQPetReadViews PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# --- Application core: repository, catalogs, controllers, caches ------------
add_library(KQPetApplicationCore STATIC
  src/application/catalog/catalog_io_service.cpp
  src/application/catalog/catalog_io_service.h
  src/application/catalog/pet_detail_catalog.cpp
  src/application/catalog/pet_detail_catalog.h
  src/application/catalog/pet_skill_catalog.cpp
  src/application/catalog/pet_skill_catalog.h
  src/application/catalog/routine_overview_catalog.cpp
  src/application/catalog/routine_overview_catalog.h
  src/application/catalog/shop_exchange_catalog.cpp
  src/application/catalog/shop_exchange_catalog.h
  src/application/common/observation_freshness.cpp
  src/application/common/observation_freshness.h
  src/application/common/read_only_request_tracker.cpp
  src/application/common/read_only_request_tracker.h
  src/application/pet/move_operation.cpp
  src/application/pet/move_operation.h
  src/application/pet/pet_derivation_cache.cpp
  src/application/pet/pet_derivation_cache.h
  src/application/pet/pet_detail_preparation_service.cpp
  src/application/pet/pet_detail_preparation_service.h
  src/application/pet/pet_record_cache.cpp
  src/application/pet/pet_record_cache.h
  src/application/pet/pet_refresh_controller.cpp
  src/application/pet/pet_refresh_controller_details.cpp
  src/application/pet/pet_refresh_controller_move.cpp
  src/application/pet/pet_refresh_controller.h
  src/application/pet/pet_repository.cpp
  src/application/pet/pet_repository_packets.cpp
  src/application/pet/pet_repository_storage.cpp
  src/application/pet/pet_repository_internal.h
  src/application/pet/pet_repository.h
  src/application/shop/shop_exchange_controller.cpp
  src/application/shop/shop_exchange_controller_activities.cpp
  src/application/shop/shop_exchange_controller_materials.cpp
  src/application/shop/shop_exchange_controller_internal.h
  src/application/shop/shop_exchange_controller.h
  src/application/shop/shop_legacy_adapters.cpp
  src/application/shop/shop_legacy_adapters.h
  src/application/routine/routine_overview_controller.cpp
  src/application/routine/routine_overview_controller.h
  src/application/analysis/account_analysis_state.h
  src/application/analysis/asset_analysis_settings.cpp
  src/application/analysis/asset_analysis_settings.h
  src/application/analysis/asset_analyzer.cpp
  src/application/analysis/asset_analyzer.h
  src/application/analysis/asset_snapshot_store.cpp
  src/application/analysis/asset_snapshot_store.h
  src/application/analysis/recommendation_adapter.cpp
  src/application/analysis/recommendation_adapter.h
)
target_compile_definitions(KQPetApplicationCore PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetApplicationCore PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetApplicationCore PUBLIC KQPetDomain KQPetDiagnostics KQPetProtocol KQPetStorage KQPetReadViews KQPetTransport Qt6::Core)
# Existing build/test names remain an aggregate application alias; the actual
# Domain target above has no dependency on this implementation boundary.
add_library(KQPetCore INTERFACE)
target_link_libraries(KQPetCore INTERFACE KQPetApplicationCore)
add_library(KQPet::Core ALIAS KQPetCore)

# --- Analysis worker: cooperative compute thread for the Domain pipeline ----
add_library(KQPetAnalysisWorker STATIC
  src/application/analysis/analysis_worker.cpp src/application/analysis/analysis_worker.h)
target_link_libraries(KQPetAnalysisWorker PUBLIC KQPetDomain Qt6::Core)
target_compile_options(KQPetAnalysisWorker PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# --- Images: request scheduling, decoding and persistence (QtGui/Network) ---
add_library(KQPetImages STATIC src/application/images/image_service.cpp src/application/images/image_service.h)
target_link_libraries(KQPetImages PUBLIC Qt6::Core Qt6::Gui Qt6::Network PRIVATE KQPetStorage)
target_compile_options(KQPetImages PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# --- Application: runtime composition, publishers and account services ------
add_library(KQPetApplication STATIC
  src/application/runtime/application_runtime.cpp src/application/runtime/application_runtime.h
  src/application/runtime/runtime_types.h
  src/application/runtime/data_update_service.cpp src/application/runtime/data_update_service.h
  src/application/runtime/cache_management_service.cpp src/application/runtime/cache_management_service.h
  src/application/views/inventory_publisher.cpp src/application/views/inventory_publisher.h
  src/application/views/analysis_publisher.cpp src/application/views/analysis_publisher.h
  src/application/analysis/local_stargod_statistics_service.cpp
  src/application/analysis/local_stargod_statistics_service.h
  src/application/analysis/asset_analysis_controller.cpp
  src/application/analysis/asset_analysis_controller.h)
target_link_libraries(KQPetApplication
  PUBLIC KQPetApplicationCore KQPetReadViews KQPetAnalysisWorker PRIVATE KQPetImages)
target_compile_options(KQPetApplication PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
