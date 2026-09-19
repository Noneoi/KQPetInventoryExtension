# Registered tests. Sections mirror the tests/ folders; test support headers
# are included as "support/...", fixtures as "fixtures/...".
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/tests")

get_target_property(KQ_QT_CORE_RUNTIME Qt6::Core IMPORTED_LOCATION_RELEASE)
if(NOT KQ_QT_CORE_RUNTIME)
  get_target_property(KQ_QT_CORE_RUNTIME Qt6::Core IMPORTED_LOCATION)
endif()
get_filename_component(KQ_QT_RUNTIME_DIR "${KQ_QT_CORE_RUNTIME}" DIRECTORY)

# Test executables that load Qt DLLs find them through PATH.
function(kqpet_use_qt_runtime)
  set_tests_properties(${ARGN} PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${KQ_QT_RUNTIME_DIR}")
endfunction()

# =============================================================================
# Python tools, PowerShell scripts and architecture guards
# =============================================================================
add_test(NAME public_data_updater_smoke COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/python/test_public_data_updater.py")
add_test(NAME public_names_updater_smoke COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/python/test_public_names_updater.py")
add_test(NAME public_icons_updater_smoke COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/python/public_icon_updater_test.py")
add_test(NAME public_routine_updater_smoke COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/python/test_public_routine_updater.py")
add_test(NAME activity_evolution_selector_smoke COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/python/test_activity_evolution_selector.py")
add_test(NAME public_activity_exchange_smoke COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/python/public_activity_exchange_updater_test.py")
add_test(NAME public_updater_output_encoding_smoke COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/python/public_updater_output_encoding_test.py")
add_test(NAME cache_manager_smoke COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/python/test_cache_manager.py")
add_test(NAME power_metadata_smoke COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/python/test_power_metadata.py")

add_test(NAME domain_boundary COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/build/check_domain_boundary.py" --self-test)
add_test(NAME ui_boundary COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/build/check_ui_boundary.py" --self-test)

add_test(NAME release_tools_smoke COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
  -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/release-tools-smoke.ps1"
  -BuildBin "$<TARGET_FILE_DIR:KQPetInventory>")
set_tests_properties(release_tools_smoke PROPERTIES TIMEOUT 60)
add_test(NAME target_tools_smoke COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
  -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/target-tools-smoke.ps1")
set_tests_properties(target_tools_smoke
  PROPERTIES ENVIRONMENT "KQPET_COMPATIBILITY_CHECK=$<TARGET_FILE:KQPetCompatibilityCheck>")

# =============================================================================
# tests/native: compatibility, launcher, bootstrap and release (no Qt)
# =============================================================================
add_executable(KQCompatibilityCoreSmoke tests/native/compatibility_core_smoke.cpp)
target_link_libraries(KQCompatibilityCoreSmoke PRIVATE KQPetCompatibility)
target_compile_options(KQCompatibilityCoreSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(KQCompatibilityCoreSmoke PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_test(NAME compatibility_core_smoke COMMAND KQCompatibilityCoreSmoke)

add_executable(KQDataRootConfigSmoke tests/native/data_root_config_smoke.cpp src/loader/data_root_config.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/cache_tools.rc")
target_compile_definitions(KQDataRootConfigSmoke PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQDataRootConfigSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQDataRootConfigSmoke PRIVATE KQPetReleaseCore)
set_target_properties(KQDataRootConfigSmoke PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_test(NAME data_root_config_smoke COMMAND KQDataRootConfigSmoke)

add_executable(KQLauncherTargetSmoke tests/native/launcher_target_smoke.cpp src/loader/client_target.cpp)
target_link_libraries(KQLauncherTargetSmoke PRIVATE KQPetCompatibility)
target_compile_options(KQLauncherTargetSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME launcher_target_smoke COMMAND KQLauncherTargetSmoke)

add_executable(KQLoaderModuleSmoke tests/native/loader_module_smoke.cpp src/loader/remote_module.cpp)
target_compile_definitions(KQLoaderModuleSmoke PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQLoaderModuleSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(KQLoaderModuleSmoke PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_test(NAME loader_module_smoke COMMAND KQLoaderModuleSmoke)
set_tests_properties(loader_module_smoke PROPERTIES TIMEOUT 12)

add_executable(KQStartupChannelSmoke tests/native/startup_channel_smoke.cpp)
target_link_libraries(KQStartupChannelSmoke PRIVATE KQPetStartup)
target_compile_options(KQStartupChannelSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(KQStartupChannelSmoke PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(KQStartupChannelSmoke PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_test(NAME startup_channel_smoke COMMAND KQStartupChannelSmoke)
set_tests_properties(startup_channel_smoke PROPERTIES TIMEOUT 15)

add_executable(KQReleaseCoreSmoke tests/native/release_core_smoke.cpp)
target_link_libraries(KQReleaseCoreSmoke PRIVATE KQPetReleaseCore shell32)
target_compile_definitions(KQReleaseCoreSmoke PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQReleaseCoreSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(KQReleaseCoreSmoke PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_test(NAME release_core_smoke COMMAND KQReleaseCoreSmoke $<TARGET_FILE:KQPetLauncher> $<TARGET_FILE:KQPetInventory>)
set_tests_properties(release_core_smoke PROPERTIES TIMEOUT 30)

# Child process used by release_tools_smoke; not a test on its own.
add_executable(KQReleaseProbeChild tests/native/release_probe_child.cpp)
target_compile_definitions(KQReleaseProbeChild PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQReleaseProbeChild PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(KQReleaseProbeChild PROPERTIES AUTOMOC OFF AUTORCC OFF)

# =============================================================================
# tests/domain: pure Domain (no qrc, no repository)
# =============================================================================
add_executable(KQDomainCoreSmoke tests/domain/domain_core_smoke.cpp)
target_link_libraries(KQDomainCoreSmoke PRIVATE KQPetDomain Qt6::Core)
target_compile_options(KQDomainCoreSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_options(KQDomainCoreSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/WHOLEARCHIVE:$<TARGET_FILE:KQPetDomain>>)
set_target_properties(KQDomainCoreSmoke PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_test(NAME domain_core_smoke COMMAND KQDomainCoreSmoke)

add_executable(KQPetCultivationRequirementsSmoke tests/domain/pet_cultivation_requirements_smoke.cpp)
target_link_libraries(KQPetCultivationRequirementsSmoke PRIVATE KQPetDomain)
target_compile_options(KQPetCultivationRequirementsSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME pet_cultivation_requirements_smoke COMMAND KQPetCultivationRequirementsSmoke)

add_executable(KQPetPowerCompositionSmoke tests/domain/pet_power_composition_smoke.cpp)
target_link_libraries(KQPetPowerCompositionSmoke PRIVATE KQPetDomain)
target_compile_options(KQPetPowerCompositionSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME pet_power_composition_smoke COMMAND KQPetPowerCompositionSmoke "${CMAKE_CURRENT_SOURCE_DIR}/assets/pet-detail-data.json")

add_executable(KQPowerConsumersSmoke tests/domain/power_consumers_smoke.cpp)
target_link_libraries(KQPowerConsumersSmoke PRIVATE KQPetDomain)
target_compile_options(KQPowerConsumersSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME power_consumers_smoke COMMAND KQPowerConsumersSmoke)

add_executable(KQShopPetParameterizedSmoke tests/domain/shop_pet_parameterized_smoke.cpp)
target_link_libraries(KQShopPetParameterizedSmoke PRIVATE KQPetDomain)
target_compile_options(KQShopPetParameterizedSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME shop_pet_parameterized_smoke COMMAND KQShopPetParameterizedSmoke)

add_executable(KQActivityShopObservationSmoke tests/domain/activity_shop_observation_smoke.cpp)
target_link_libraries(KQActivityShopObservationSmoke PRIVATE KQPetDomain)
target_compile_options(KQActivityShopObservationSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME activity_shop_observation_smoke COMMAND KQActivityShopObservationSmoke)

add_executable(KQPetDetailPreparationSmoke tests/domain/pet_detail_preparation_smoke.cpp)
target_link_libraries(KQPetDetailPreparationSmoke PRIVATE KQPetDomain)
target_compile_options(KQPetDetailPreparationSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME pet_detail_preparation_smoke COMMAND KQPetDetailPreparationSmoke)
set_tests_properties(pet_detail_preparation_smoke PROPERTIES TIMEOUT 30)

kqpet_use_qt_runtime(domain_core_smoke pet_cultivation_requirements_smoke pet_power_composition_smoke
  power_consumers_smoke shop_pet_parameterized_smoke activity_shop_observation_smoke
  pet_detail_preparation_smoke)

# =============================================================================
# tests/storage and tests/diagnostics
# =============================================================================
add_executable(KQStorageQueueSmoke tests/storage/storage_queue_smoke.cpp)
target_link_libraries(KQStorageQueueSmoke PRIVATE KQPetStorage)
target_compile_options(KQStorageQueueSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME storage_queue_smoke COMMAND KQStorageQueueSmoke)
set_tests_properties(storage_queue_smoke PROPERTIES TIMEOUT 20)

add_executable(KQDiagnosticSmoke tests/diagnostics/diagnostic_smoke.cpp)
target_link_libraries(KQDiagnosticSmoke PRIVATE KQPetDiagnostics KQPetStorage Qt6::Core bcrypt)
target_compile_definitions(KQDiagnosticSmoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQDiagnosticSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME diagnostic_smoke COMMAND KQDiagnosticSmoke)
set_tests_properties(diagnostic_smoke PROPERTIES TIMEOUT 20)

add_executable(KQTargetProfileSmoke tests/diagnostics/target_profile_smoke.cpp)
target_compile_definitions(KQTargetProfileSmoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQTargetProfileSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQTargetProfileSmoke PRIVATE KQPetDiagnostics)
add_test(NAME target_profile_smoke COMMAND KQTargetProfileSmoke)

kqpet_use_qt_runtime(storage_queue_smoke diagnostic_smoke target_profile_smoke)

# =============================================================================
# tests/protocol
# =============================================================================
add_executable(KQInboundQueueSmoke tests/protocol/inbound_queue_smoke.cpp)
target_link_libraries(KQInboundQueueSmoke PRIVATE KQPetInbound)
target_compile_options(KQInboundQueueSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME inbound_queue_smoke COMMAND KQInboundQueueSmoke)

add_executable(KQOutboundContractSmoke tests/protocol/outbound_contract_smoke.cpp)
target_link_libraries(KQOutboundContractSmoke PRIVATE KQPetProtocol)
target_compile_options(KQOutboundContractSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME outbound_contract_smoke COMMAND KQOutboundContractSmoke)

add_executable(KQProtocolTransportSmoke tests/protocol/protocol_transport_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQProtocolTransportSmoke PRIVATE KQPetCore)
target_compile_options(KQProtocolTransportSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME protocol_transport_smoke COMMAND KQProtocolTransportSmoke)
set_tests_properties(protocol_transport_smoke PROPERTIES TIMEOUT 20)

add_executable(KQProtocolFixtureSmoke tests/protocol/fixture_smoke.cpp src/extension/resources.qrc)
target_compile_definitions(
  KQProtocolFixtureSmoke PRIVATE
  KQPET_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
)
target_compile_options(KQProtocolFixtureSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQProtocolFixtureSmoke PRIVATE KQPetCore KQPetUi)
add_test(NAME protocol_fixture_smoke COMMAND KQProtocolFixtureSmoke)

kqpet_use_qt_runtime(inbound_queue_smoke outbound_contract_smoke protocol_transport_smoke
  protocol_fixture_smoke)

# =============================================================================
# tests/bridge
# =============================================================================
add_executable(KQInlineHookPolicySmoke tests/bridge/inline_hook_policy_smoke.cpp)
target_compile_options(KQInlineHookPolicySmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQInlineHookPolicySmoke PRIVATE KQPetBridge)
add_test(NAME inline_hook_policy_smoke COMMAND KQInlineHookPolicySmoke)

add_executable(KQInlineHookRuntimeSmoke tests/bridge/inline_hook_runtime_smoke.cpp)
target_compile_definitions(KQInlineHookRuntimeSmoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQInlineHookRuntimeSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQInlineHookRuntimeSmoke PRIVATE KQPetBridge)
add_test(NAME inline_hook_runtime_smoke COMMAND KQInlineHookRuntimeSmoke)
set_tests_properties(inline_hook_runtime_smoke PROPERTIES TIMEOUT 20)

add_executable(KQOriginalWindowLocatorSmoke tests/bridge/original_window_locator_smoke.cpp
  src/bridge/original_window_locator.cpp src/bridge/original_window_locator.h)
target_compile_options(KQOriginalWindowLocatorSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQOriginalWindowLocatorSmoke PRIVATE Qt6::Widgets)
add_test(NAME original_window_locator_smoke COMMAND KQOriginalWindowLocatorSmoke)
set_tests_properties(original_window_locator_smoke
  PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)

kqpet_use_qt_runtime(inline_hook_policy_smoke inline_hook_runtime_smoke original_window_locator_smoke)

# =============================================================================
# tests/application: catalogs, repository, controllers, services, runtime
# =============================================================================
# --- catalogs ---
add_executable(KQPetCatalogSmoke tests/application/catalog_smoke.cpp src/extension/resources.qrc)
target_compile_options(KQPetCatalogSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetCatalogSmoke PRIVATE KQPetCore)
add_test(NAME pet_detail_catalog_smoke COMMAND KQPetCatalogSmoke)

add_executable(KQCatalogIoSmoke tests/application/catalog_io_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQCatalogIoSmoke PRIVATE KQPetCore)
target_compile_options(KQCatalogIoSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME catalog_io_smoke COMMAND KQCatalogIoSmoke)
set_tests_properties(catalog_io_smoke PROPERTIES TIMEOUT 30)

add_executable(KQActivityShopCatalogSmoke tests/application/activity_shop_catalog_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQActivityShopCatalogSmoke PRIVATE KQPetApplicationCore)
target_compile_options(KQActivityShopCatalogSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME activity_shop_catalog_smoke COMMAND KQActivityShopCatalogSmoke)

# --- pet repository, records and refresh/move ---
add_executable(KQPetRepositorySmoke tests/application/repository_smoke.cpp src/extension/resources.qrc)
target_compile_options(KQPetRepositorySmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetRepositorySmoke PRIVATE KQPetCore)
add_test(NAME pet_repository_smoke COMMAND KQPetRepositorySmoke)

add_executable(KQPetRecordRepositorySmoke tests/application/pet_record_repository_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQPetRecordRepositorySmoke PRIVATE KQPetCore)
target_compile_options(KQPetRecordRepositorySmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME pet_record_repository_smoke COMMAND KQPetRecordRepositorySmoke)
set_tests_properties(pet_record_repository_smoke PROPERTIES TIMEOUT 30)

add_executable(KQPetRecordCacheSmoke tests/application/pet_record_cache_smoke.cpp)
target_link_libraries(KQPetRecordCacheSmoke PRIVATE KQPetCore)
target_compile_options(KQPetRecordCacheSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME pet_record_cache_smoke COMMAND KQPetRecordCacheSmoke)
set_tests_properties(pet_record_cache_smoke PROPERTIES TIMEOUT 20)

add_executable(KQPetDerivationCacheSmoke tests/application/pet_derivation_cache_smoke.cpp)
target_link_libraries(KQPetDerivationCacheSmoke PRIVATE KQPetCore)
target_compile_options(KQPetDerivationCacheSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME pet_derivation_cache_smoke COMMAND KQPetDerivationCacheSmoke)
set_tests_properties(pet_derivation_cache_smoke PROPERTIES TIMEOUT 20)

add_executable(KQPetDetailServiceSmoke tests/application/pet_detail_service_smoke.cpp)
target_link_libraries(KQPetDetailServiceSmoke PRIVATE KQPetCore)
target_compile_options(KQPetDetailServiceSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME pet_detail_service_smoke COMMAND KQPetDetailServiceSmoke)
set_tests_properties(pet_detail_service_smoke PROPERTIES TIMEOUT 30)

add_executable(KQPetRefreshSmoke tests/application/refresh_controller_smoke.cpp src/extension/resources.qrc)
target_compile_options(KQPetRefreshSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetRefreshSmoke PRIVATE KQPetCore)
add_test(NAME pet_refresh_controller_smoke COMMAND KQPetRefreshSmoke)

add_executable(KQPetMoveSmoke tests/application/move_controller_smoke.cpp src/extension/resources.qrc)
target_compile_options(KQPetMoveSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetMoveSmoke PRIVATE KQPetCore)
add_test(NAME pet_move_controller_smoke COMMAND KQPetMoveSmoke)

add_executable(KQMoveOperationSmoke tests/application/move_operation_smoke.cpp)
target_link_libraries(KQMoveOperationSmoke PRIVATE KQPetCore)
target_compile_options(KQMoveOperationSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME move_operation_smoke COMMAND KQMoveOperationSmoke)

# --- shop and routine controllers ---
add_executable(KQPetShopSmoke tests/application/shop_exchange_smoke.cpp src/extension/resources.qrc)
target_compile_options(KQPetShopSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetShopSmoke PRIVATE KQPetCore)
add_test(NAME shop_exchange_smoke COMMAND KQPetShopSmoke)

add_executable(KQActivityShopControllerSmoke tests/application/activity_shop_controller_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQActivityShopControllerSmoke PRIVATE KQPetApplicationCore)
target_compile_options(KQActivityShopControllerSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME activity_shop_controller_smoke COMMAND KQActivityShopControllerSmoke)
set_tests_properties(activity_shop_controller_smoke PROPERTIES TIMEOUT 30)

add_executable(KQCultivationMaterialInventorySmoke tests/application/cultivation_material_inventory_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQCultivationMaterialInventorySmoke PRIVATE KQPetApplicationCore)
target_compile_options(KQCultivationMaterialInventorySmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME cultivation_material_inventory_smoke COMMAND KQCultivationMaterialInventorySmoke)
set_tests_properties(cultivation_material_inventory_smoke PROPERTIES TIMEOUT 60)

add_executable(KQRoutineOverviewSmoke tests/application/routine_overview_smoke.cpp src/extension/resources.qrc)
target_compile_options(KQRoutineOverviewSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQRoutineOverviewSmoke PRIVATE KQPetCore)
add_test(NAME routine_overview_smoke COMMAND KQRoutineOverviewSmoke)

add_executable(KQObservationFreshnessSmoke tests/application/observation_freshness_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQObservationFreshnessSmoke PRIVATE KQPetCore)
target_compile_options(KQObservationFreshnessSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME observation_freshness_smoke COMMAND KQObservationFreshnessSmoke)
set_tests_properties(observation_freshness_smoke PROPERTIES TIMEOUT 20)

# --- analysis, recommendation and snapshots ---
add_executable(KQAlgorithmPipelineSmoke tests/application/algorithm_pipeline_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQAlgorithmPipelineSmoke PRIVATE KQPetCore)
target_compile_options(KQAlgorithmPipelineSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME algorithm_pipeline_smoke COMMAND KQAlgorithmPipelineSmoke)

add_executable(KQRecommendationSmoke tests/application/recommendation_smoke.cpp src/extension/resources.qrc)
target_compile_options(KQRecommendationSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQRecommendationSmoke PRIVATE KQPetCore)
add_test(NAME recommendation_smoke COMMAND KQRecommendationSmoke)

add_executable(KQAssetAnalysisSmoke tests/application/asset_analysis_smoke.cpp src/extension/resources.qrc)
target_compile_options(KQAssetAnalysisSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQAssetAnalysisSmoke PRIVATE KQPetApplication)
add_test(NAME asset_analysis_smoke COMMAND KQAssetAnalysisSmoke)

add_executable(KQAnalysisWorkerSmoke tests/application/analysis_worker_smoke.cpp)
target_link_libraries(KQAnalysisWorkerSmoke PRIVATE KQPetAnalysisWorker)
target_compile_options(KQAnalysisWorkerSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME analysis_worker_smoke COMMAND KQAnalysisWorkerSmoke)
set_tests_properties(analysis_worker_smoke PROPERTIES TIMEOUT 20)

add_executable(KQAnalysisCacheIntegrationSmoke tests/application/analysis_cache_integration_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQAnalysisCacheIntegrationSmoke PRIVATE KQPetApplication)
target_compile_options(KQAnalysisCacheIntegrationSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME analysis_cache_integration_smoke COMMAND KQAnalysisCacheIntegrationSmoke)
set_tests_properties(analysis_cache_integration_smoke PROPERTIES TIMEOUT 150)

add_executable(KQSnapshotStorageSmoke tests/application/snapshot_storage_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQSnapshotStorageSmoke PRIVATE KQPetCore)
target_compile_options(KQSnapshotStorageSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME snapshot_storage_smoke COMMAND KQSnapshotStorageSmoke)
set_tests_properties(snapshot_storage_smoke PROPERTIES TIMEOUT 120)

add_executable(KQLocalStargodStatisticsSmoke tests/application/local_stargod_statistics_smoke.cpp)
target_link_libraries(KQLocalStargodStatisticsSmoke PRIVATE KQPetApplication KQPetStorage)
target_compile_options(KQLocalStargodStatisticsSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME local_stargod_statistics_smoke COMMAND KQLocalStargodStatisticsSmoke)

# --- read views, images and runtime services ---
add_executable(KQInventoryProjectionSmoke tests/application/inventory_projection_smoke.cpp)
target_link_libraries(KQInventoryProjectionSmoke PRIVATE KQPetReadViews)
target_compile_options(KQInventoryProjectionSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME inventory_projection_smoke COMMAND KQInventoryProjectionSmoke)

add_executable(KQImageServiceSmoke tests/application/image_service_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQImageServiceSmoke PRIVATE KQPetImages KQPetUi KQPetApplication KQPetStorage Qt6::Test Qt6::Widgets Qt6::Network)
target_compile_options(KQImageServiceSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME image_service_smoke COMMAND KQImageServiceSmoke)
set_tests_properties(image_service_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(KQCacheManagementServiceSmoke tests/application/cache_management_service_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQCacheManagementServiceSmoke PRIVATE KQPetApplication KQPetStorage)
target_compile_options(KQCacheManagementServiceSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME cache_management_service_smoke COMMAND KQCacheManagementServiceSmoke)
set_tests_properties(cache_management_service_smoke PROPERTIES TIMEOUT 60)

add_executable(KQApplicationRuntimeSmoke tests/application/application_runtime_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQApplicationRuntimeSmoke PRIVATE KQPetApplication)
target_compile_options(KQApplicationRuntimeSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME application_runtime_smoke COMMAND KQApplicationRuntimeSmoke)
set_tests_properties(application_runtime_smoke PROPERTIES TIMEOUT 20)

kqpet_use_qt_runtime(
  pet_detail_catalog_smoke catalog_io_smoke activity_shop_catalog_smoke
  pet_repository_smoke pet_record_repository_smoke pet_record_cache_smoke pet_derivation_cache_smoke
  pet_detail_service_smoke pet_refresh_controller_smoke pet_move_controller_smoke move_operation_smoke
  shop_exchange_smoke activity_shop_controller_smoke cultivation_material_inventory_smoke
  routine_overview_smoke observation_freshness_smoke
  algorithm_pipeline_smoke recommendation_smoke asset_analysis_smoke analysis_worker_smoke
  analysis_cache_integration_smoke snapshot_storage_smoke local_stargod_statistics_smoke
  inventory_projection_smoke image_service_smoke cache_management_service_smoke
  application_runtime_smoke)

# =============================================================================
# tests/ui: widget smokes and offscreen previews
# =============================================================================
add_executable(KQPetSearchSmoke tests/ui/search_smoke.cpp)
target_compile_definitions(KQPetSearchSmoke PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetSearchSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetSearchSmoke PRIVATE KQPetUi)
add_test(NAME pet_search_smoke COMMAND KQPetSearchSmoke)

add_executable(KQPetTableModelSmoke tests/ui/pet_table_model_smoke.cpp src/extension/resources.qrc)
target_compile_options(KQPetTableModelSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetTableModelSmoke PRIVATE KQPetUi KQPetApplication Qt6::Test)
add_test(NAME pet_table_model_smoke COMMAND KQPetTableModelSmoke)

add_executable(KQSelectedDetailRefreshSmoke tests/ui/selected_detail_refresh_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQSelectedDetailRefreshSmoke PRIVATE KQPetUi KQPetApplication)
target_compile_options(KQSelectedDetailRefreshSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME selected_detail_refresh_smoke COMMAND KQSelectedDetailRefreshSmoke)
set_tests_properties(selected_detail_refresh_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

kqpet_use_qt_runtime(pet_search_smoke pet_table_model_smoke selected_detail_refresh_smoke)

add_executable(KQWorkbenchUiPreview tests/ui/workbench_ui_preview.cpp src/extension/resources.qrc)
target_link_libraries(KQWorkbenchUiPreview PRIVATE KQPetUi KQPetApplication Qt6::Test)
target_compile_options(KQWorkbenchUiPreview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME workbench_ui_preview_smoke COMMAND KQWorkbenchUiPreview)
set_tests_properties(workbench_ui_preview_smoke PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;KQPET_PREVIEW_SELF_TEST=1"
  TIMEOUT 20)
kqpet_use_qt_runtime(workbench_ui_preview_smoke)

add_executable(KQPetUiPreview tests/ui/ui_preview.cpp src/extension/resources.qrc)
target_compile_options(KQPetUiPreview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetUiPreview PRIVATE KQPetUi KQPetApplication)
kqpet_use_build_info(KQPetUiPreview)
add_test(NAME pet_ui_preview_smoke COMMAND KQPetUiPreview)

add_executable(KQShopUiPreview tests/ui/shop_ui_preview.cpp src/extension/resources.qrc)
target_compile_options(KQShopUiPreview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQShopUiPreview PRIVATE KQPetUi KQPetApplication)
kqpet_use_build_info(KQShopUiPreview)
add_test(NAME shop_ui_preview_smoke COMMAND KQShopUiPreview)

add_executable(KQRoutineUiPreview tests/ui/routine_ui_preview.cpp)
target_compile_options(KQRoutineUiPreview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQRoutineUiPreview PRIVATE KQPetUi KQPetApplication)
kqpet_use_build_info(KQRoutineUiPreview)
add_test(NAME routine_ui_preview_smoke COMMAND KQRoutineUiPreview)

add_executable(KQUiPreferencesSmoke tests/ui/ui_preferences_smoke.cpp)
target_compile_options(KQUiPreferencesSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQUiPreferencesSmoke PRIVATE KQPetUi)
add_test(NAME ui_preferences_smoke COMMAND KQUiPreferencesSmoke)

add_executable(KQAssetAnalysisUiPreview tests/ui/asset_analysis_ui_preview.cpp src/extension/resources.qrc)
target_compile_options(KQAssetAnalysisUiPreview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQAssetAnalysisUiPreview PRIVATE KQPetUi KQPetApplication)
kqpet_use_build_info(KQAssetAnalysisUiPreview)
add_test(NAME asset_analysis_ui_preview_smoke COMMAND KQAssetAnalysisUiPreview)

set_tests_properties(
  pet_ui_preview_smoke shop_ui_preview_smoke routine_ui_preview_smoke
  asset_analysis_ui_preview_smoke ui_preferences_smoke
  PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;KQPET_PREVIEW_SELF_TEST=1;KQPET_PREVIEW_EXIT_MS=3000"
    TIMEOUT 15
)
# The shop preview also runs the thousand-pet project switch check, which needs
# more headroom on hosted CI runners (about 7s locally, over 15s on CI).
set_tests_properties(shop_ui_preview_smoke PROPERTIES TIMEOUT 45)
kqpet_use_qt_runtime(pet_ui_preview_smoke shop_ui_preview_smoke routine_ui_preview_smoke
  asset_analysis_ui_preview_smoke ui_preferences_smoke)

# =============================================================================
# tests/performance (see scripts/run-performance.ps1 for the full matrix)
# =============================================================================
add_executable(KQAssetAnalysisPerformance
  tests/performance/asset_analysis_performance.cpp
  tests/performance/performance_dataset.cpp
  tests/performance/performance_dataset.h
  src/extension/resources.qrc
)
target_compile_options(KQAssetAnalysisPerformance PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQAssetAnalysisPerformance PRIVATE KQPetUi KQPetApplication)
add_test(NAME asset_analysis_performance COMMAND KQAssetAnalysisPerformance)
set_tests_properties(asset_analysis_performance
  PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20)
kqpet_use_qt_runtime(asset_analysis_performance)
