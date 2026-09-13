cmake_minimum_required(VERSION 3.24)

project(KQPetInventoryExtension VERSION 2.0.0 LANGUAGES C CXX)

# CMake 4.4 on the local Chinese MSVC toolchain decodes UTF-8 /showIncludes
# detection as the system code page. Ninja then records zero header deps.
# Correct only this identified mojibake; other compiler locales keep detection.
if(MSVC AND CMAKE_CL_SHOWINCLUDES_PREFIX MATCHES "娉ㄦ剰")
  set(CMAKE_CL_SHOWINCLUDES_PREFIX "注意: 包含文件:  ")
endif()

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This project only supports 64-bit Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

find_package(Qt6 6.6.3 EXACT REQUIRED COMPONENTS Core Gui Widgets Network Test)

# KQPetInventoryExtension's project version is the only authoritative version
# source.  Every binary/UI surface consumes this generated header instead of
# maintaining its own version literal.
find_package(Git QUIET)
set(KQPET_GIT_COMMIT "unknown")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD" KQPET_GIT_HEAD LIMIT 1024)
  string(STRIP "${KQPET_GIT_HEAD}" KQPET_GIT_HEAD)
  if(KQPET_GIT_HEAD MATCHES "^ref: (.+)$")
    set(KQPET_GIT_REF "${CMAKE_MATCH_1}")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/${KQPET_GIT_REF}")
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                   "${CMAKE_CURRENT_SOURCE_DIR}/.git/${KQPET_GIT_REF}")
    endif()
  endif()
endif()
if(Git_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE KQPET_GIT_RESULT
    OUTPUT_VARIABLE KQPET_GIT_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(KQPET_GIT_RESULT EQUAL 0 AND NOT KQPET_GIT_OUTPUT STREQUAL "")
    set(KQPET_GIT_COMMIT "${KQPET_GIT_OUTPUT}")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=normal
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      RESULT_VARIABLE KQPET_GIT_STATUS_RESULT
      OUTPUT_VARIABLE KQPET_GIT_STATUS_OUTPUT
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    if(KQPET_GIT_STATUS_RESULT EQUAL 0 AND NOT KQPET_GIT_STATUS_OUTPUT STREQUAL "")
      string(APPEND KQPET_GIT_COMMIT "-dirty")
    endif()
  endif()
endif()
string(TIMESTAMP KQPET_BUILD_TIME_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
set(KQPET_TARGET_ARCH "x64")
set(KQPET_QT_VERSION "${Qt6_VERSION}")
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/assets/pet-image-urls.json" KQPET_IMAGE_URLS_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/assets/attribute-icons.png" KQPET_ATTRIBUTE_IMAGE_SHA256)
file(GLOB KQPET_STARGOD_ICON_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/stargod-icons/*.png")
list(SORT KQPET_STARGOD_ICON_FILES)
set(KQPET_STARGOD_IMAGE_HASHES "")
foreach(icon IN LISTS KQPET_STARGOD_ICON_FILES)
  file(SHA256 "${icon}" icon_hash)
  get_filename_component(icon_name "${icon}" NAME)
  string(APPEND KQPET_STARGOD_IMAGE_HASHES "${icon_name}:${icon_hash};")
endforeach()
string(SHA256 KQPET_IMAGE_RESOURCE_VERSION "${KQPET_IMAGE_URLS_SHA256}:${KQPET_ATTRIBUTE_IMAGE_SHA256}:${KQPET_STARGOD_IMAGE_HASHES}")
set(KQPET_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${KQPET_GENERATED_INCLUDE_DIR}")
file(GLOB_RECURSE KQPET_BUILD_INPUTS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*" "${CMAKE_CURRENT_SOURCE_DIR}/assets/*"
  "${CMAKE_CURRENT_SOURCE_DIR}/profiles/*" "${CMAKE_CURRENT_SOURCE_DIR}/tools/*"
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/*" "${CMAKE_CURRENT_SOURCE_DIR}/tests/*"
  "${CMAKE_CURRENT_SOURCE_DIR}/docs/*"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/*")
# Interpreter caches are generated machine-specific files, not source identity.
list(FILTER KQPET_BUILD_INPUTS EXCLUDE REGEX "(/__pycache__/|[.]py[co]$)")
list(APPEND KQPET_BUILD_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
list(APPEND KQPET_BUILD_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/README.md" "${CMAKE_CURRENT_SOURCE_DIR}/RELEASE_NOTES.md")
list(SORT KQPET_BUILD_INPUTS)
set(KQPET_SOURCE_HASH_INPUT "")
foreach(input IN LISTS KQPET_BUILD_INPUTS)
  if(NOT IS_DIRECTORY "${input}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
    file(SHA256 "${input}" input_hash)
    file(RELATIVE_PATH input_relative "${CMAKE_CURRENT_SOURCE_DIR}" "${input}")
    string(APPEND KQPET_SOURCE_HASH_INPUT "${input_relative}:${input_hash}\n")
  endif()
endforeach()
# Bind the exact inventory bytes that verification reads. On Windows CMake's
# file writer uses CRLF, which differs from hashing the in-memory LF string.
file(WRITE "${KQPET_GENERATED_INCLUDE_DIR}/build-input-hashes.txt" "${KQPET_SOURCE_HASH_INPUT}")
file(SHA256 "${KQPET_GENERATED_INCLUDE_DIR}/build-input-hashes.txt" KQPET_SOURCE_SHA256)
string(SUBSTRING "${KQPET_SOURCE_SHA256}" 0 12 KQPET_SOURCE_SHORT)
string(TIMESTAMP KQPET_RELEASE_TIME "%Y%m%dT%H%M%SZ" UTC)
set(KQPET_RELEASE_ID "${PROJECT_VERSION}-${KQPET_SOURCE_SHORT}-${KQPET_RELEASE_TIME}")
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/profiles/targets.json" KQPET_PROFILE_SHA256)
set(KQPET_TOOLCHAIN "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION};MSVC-${MSVC_VERSION};CMake-${CMAKE_VERSION}")
set(KQPET_CONFIGURATION "${CMAKE_BUILD_TYPE}")
if(NOT KQPET_CONFIGURATION)
  message(FATAL_ERROR "A fixed CMAKE_BUILD_TYPE is required for artifact identity; use scripts/build.ps1.")
endif()
configure_file(src/runtime/build_identity.json.in "${KQPET_GENERATED_INCLUDE_DIR}/build_identity.json" @ONLY)
set(KQPET_BUILD_IDENTITY_JSON "${KQPET_GENERATED_INCLUDE_DIR}/build_identity.json")
configure_file(src/runtime/build_identity.rc.in "${KQPET_GENERATED_INCLUDE_DIR}/build_identity.rc" @ONLY)
# RC's embedded RCDATA file is not reliably discovered by Ninja/MSVC. Keep
# both artifact resources tied to the newly configured JSON, not an old .res.
set_source_files_properties("${KQPET_GENERATED_INCLUDE_DIR}/build_identity.rc"
  PROPERTIES OBJECT_DEPENDS "${KQPET_GENERATED_INCLUDE_DIR}/build_identity.json")
configure_file(
  src/extension/version.h.in
  "${KQPET_GENERATED_INCLUDE_DIR}/version.h"
  @ONLY
)

function(kqpet_use_build_info target)
  target_include_directories(${target} PRIVATE "${KQPET_GENERATED_INCLUDE_DIR}")
endfunction()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT "${KQPET_GENERATED_INCLUDE_DIR}/target_profiles.cpp"
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_profiles.py"
          --input "${CMAKE_CURRENT_SOURCE_DIR}/profiles/targets.json"
          --output "${KQPET_GENERATED_INCLUDE_DIR}/target_profiles.cpp"
  DEPENDS tools/generate_profiles.py profiles/targets.json VERBATIM)
add_library(KQPetCompatibility STATIC src/compatibility/pe_view.cpp
  src/compatibility/target_check.cpp "${KQPET_GENERATED_INCLUDE_DIR}/target_profiles.cpp")
target_include_directories(KQPetCompatibility PUBLIC src/compatibility)
target_compile_definitions(KQPetCompatibility PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetCompatibility PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQPetCompatibility PRIVATE version bcrypt)
set_target_properties(KQPetCompatibility PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_executable(KQPetCompatibilityCheck src/compatibility/check_main.cpp)
target_link_libraries(KQPetCompatibilityCheck PRIVATE KQPetCompatibility)
target_compile_options(KQPetCompatibilityCheck PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(KQPetCompatibilityCheck PROPERTIES AUTOMOC OFF AUTORCC OFF)

add_library(KQPetDiagnostics STATIC
  src/extension/build_info.cpp
  src/extension/build_info.h
  src/extension/diagnostic_logger.cpp
  src/extension/diagnostic_logger.h
  src/extension/target_compatibility_guard.cpp
  src/extension/target_compatibility_guard.h
  src/extension/target_profile.h
  src/extension/target_profile_registry.cpp
  src/extension/target_profile_registry.h
)
target_include_directories(KQPetDiagnostics PUBLIC src/extension
                           "${KQPET_GENERATED_INCLUDE_DIR}")
target_compile_definitions(KQPetDiagnostics PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetDiagnostics PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetDiagnostics PUBLIC Qt6::Core KQPetCompatibility PRIVATE version)

configure_file(src/loader/cache_tools.rc.in "${CMAKE_CURRENT_BINARY_DIR}/cache_tools.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/cache_tools.rc"
  PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/cache-manager.ps1")
add_executable(KQPetLauncher WIN32
  src/loader/main.cpp
  src/loader/data_root_config.cpp
  src/loader/client_target.cpp
  src/loader/remote_module.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/cache_tools.rc"
  "${KQPET_GENERATED_INCLUDE_DIR}/build_identity.rc"
)
target_compile_definitions(KQPetLauncher PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetLauncher PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQPetLauncher PRIVATE user32)
kqpet_use_build_info(KQPetLauncher)

add_library(KQPetStartup STATIC src/runtime/startup_channel.cpp src/runtime/startup_channel.h)
target_include_directories(KQPetStartup PUBLIC src/runtime "${KQPET_GENERATED_INCLUDE_DIR}")
target_compile_definitions(KQPetStartup PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetStartup PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQPetStartup PRIVATE bcrypt advapi32)
set_target_properties(KQPetStartup PROPERTIES AUTOMOC OFF AUTORCC OFF)
target_link_libraries(KQPetLauncher PRIVATE KQPetCompatibility KQPetStartup shell32)
add_library(KQPetReleaseCore STATIC src/runtime/release_manifest.cpp src/runtime/release_manifest.h
  src/runtime/release_activation.cpp src/runtime/release_activation.h
  src/bootstrap/bootstrap.cpp src/bootstrap/bootstrap.h src/bootstrap/strict_json.h
  src/loader/client_target.cpp)
target_include_directories(KQPetReleaseCore PUBLIC src/runtime src/bootstrap src/loader)
target_link_libraries(KQPetReleaseCore PUBLIC KQPetCompatibility PRIVATE bcrypt)
target_compile_definitions(KQPetReleaseCore PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetReleaseCore PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(KQPetReleaseCore PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_executable(KQPetReleaseCheck src/runtime/release_check_main.cpp)
target_link_libraries(KQPetReleaseCheck PRIVATE KQPetReleaseCore)
target_compile_options(KQPetReleaseCheck PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(KQPetReleaseCheck PROPERTIES AUTOMOC OFF AUTORCC OFF)
configure_file(src/bootstrap/bootstrap_identity.rc.in "${KQPET_GENERATED_INCLUDE_DIR}/bootstrap_identity.rc" @ONLY)
set_source_files_properties("${KQPET_GENERATED_INCLUDE_DIR}/bootstrap_identity.rc"
  PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap/bootstrap_identity.json")
add_executable(KQPetBootstrap WIN32 src/bootstrap/main.cpp "${KQPET_GENERATED_INCLUDE_DIR}/bootstrap_identity.rc")
target_compile_definitions(KQPetBootstrap PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetBootstrap PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQPetBootstrap PRIVATE KQPetReleaseCore shell32)
set_target_properties(KQPetBootstrap PROPERTIES AUTOMOC OFF AUTORCC OFF)
target_link_libraries(KQPetLauncher PRIVATE KQPetReleaseCore)
add_executable(KQDataRootConfigSmoke tests/data_root_config_smoke.cpp src/loader/data_root_config.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/cache_tools.rc")
target_include_directories(KQDataRootConfigSmoke PRIVATE src/loader)
target_compile_definitions(KQDataRootConfigSmoke PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQDataRootConfigSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQDataRootConfigSmoke PRIVATE KQPetReleaseCore)
set_target_properties(KQDataRootConfigSmoke PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_test(NAME data_root_config_smoke COMMAND KQDataRootConfigSmoke)
add_test(NAME public_data_updater_smoke COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_public_data_updater.py")
add_test(NAME public_names_updater_smoke COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_public_names_updater.py")
add_test(NAME public_icons_updater_smoke COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/public_icon_updater_test.py")
add_test(NAME public_routine_updater_smoke COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_public_routine_updater.py")
add_test(NAME activity_evolution_selector_smoke COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_activity_evolution_selector.py")
add_test(NAME public_activity_exchange_smoke COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/public_activity_exchange_updater_test.py")
add_test(NAME cache_manager_smoke COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cache_manager.py")
add_executable(KQReleaseCoreSmoke tests/release_core_smoke.cpp)
target_link_libraries(KQReleaseCoreSmoke PRIVATE KQPetReleaseCore shell32)
target_compile_definitions(KQReleaseCoreSmoke PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQReleaseCoreSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(KQReleaseCoreSmoke PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_test(NAME release_core_smoke COMMAND KQReleaseCoreSmoke $<TARGET_FILE:KQPetLauncher> $<TARGET_FILE:KQPetInventory>)
set_tests_properties(release_core_smoke PROPERTIES TIMEOUT 30)
add_executable(KQReleaseProbeChild tests/release_probe_child.cpp)
target_compile_definitions(KQReleaseProbeChild PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQReleaseProbeChild PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(KQReleaseProbeChild PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_test(NAME release_tools_smoke COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
  -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/release-tools-smoke.ps1"
  -BuildBin "$<TARGET_FILE_DIR:KQPetInventory>")
set_tests_properties(release_tools_smoke PROPERTIES TIMEOUT 60)

add_library(KQPetDomain STATIC
  src/domain/local_stargod_count.cpp src/domain/local_stargod_count.h
  src/domain/pet_cultivation_requirements.cpp src/domain/pet_cultivation_requirements.h
  src/domain/account_resource_view.cpp
  src/domain/asset_derivation.cpp
  src/domain/asset_snapshot_comparator.cpp
  src/domain/catalog_types.cpp
  src/domain/checked_json_numbers.cpp
  src/domain/compiled_shop_catalog.cpp
  src/domain/pet_identity.cpp
  src/domain/pet_analysis_facts.cpp
  src/domain/pet_detail_preparation.cpp
  src/domain/pet_metadata_view.cpp
  src/domain/pet_era.cpp
  src/domain/activity_shop_observation.cpp
  src/domain/pet_move_policy.cpp
  src/domain/pet_power_calculator.cpp
  src/domain/prepared_shop_conditions.cpp
  src/domain/recommendation_engine.cpp
  src/domain/shop_actionability.cpp
  src/domain/shop_limit_facts.cpp
  src/domain/shop_pet_eligibility.cpp)
target_include_directories(KQPetDomain PUBLIC src/domain)
target_link_libraries(KQPetDomain PUBLIC Qt6::Core)
target_compile_options(KQPetDomain PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(KQPetDomain PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_library(KQPet::Domain ALIAS KQPetDomain)
add_executable(KQPetCultivationRequirementsSmoke tests/pet_cultivation_requirements_smoke.cpp)
target_include_directories(KQPetCultivationRequirementsSmoke PRIVATE src)
target_link_libraries(KQPetCultivationRequirementsSmoke PRIVATE KQPetDomain)
target_compile_options(KQPetCultivationRequirementsSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME pet_cultivation_requirements_smoke COMMAND KQPetCultivationRequirementsSmoke)
add_executable(KQPetPowerCompositionSmoke tests/pet_power_composition_smoke.cpp)
target_include_directories(KQPetPowerCompositionSmoke PRIVATE src)
target_link_libraries(KQPetPowerCompositionSmoke PRIVATE KQPetDomain)
target_compile_options(KQPetPowerCompositionSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME pet_power_composition_smoke COMMAND KQPetPowerCompositionSmoke "${CMAKE_CURRENT_SOURCE_DIR}/assets/pet-detail-data.json")
add_test(NAME power_metadata_smoke COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_power_metadata.py")
add_executable(KQPowerConsumersSmoke tests/power_consumers_smoke.cpp)
add_executable(KQShopPetParameterizedSmoke tests/shop_pet_parameterized_smoke.cpp)
target_link_libraries(KQShopPetParameterizedSmoke PRIVATE KQPetDomain)
target_compile_options(KQShopPetParameterizedSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME shop_pet_parameterized_smoke COMMAND KQShopPetParameterizedSmoke)
target_link_libraries(KQPowerConsumersSmoke PRIVATE KQPetDomain)
target_compile_options(KQPowerConsumersSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME power_consumers_smoke COMMAND KQPowerConsumersSmoke)
add_executable(KQDomainCoreSmoke tests/domain_core_smoke.cpp)
target_link_libraries(KQDomainCoreSmoke PRIVATE KQPetDomain Qt6::Core)
target_compile_options(KQDomainCoreSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_options(KQDomainCoreSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/WHOLEARCHIVE:$<TARGET_FILE:KQPetDomain>>)
set_target_properties(KQDomainCoreSmoke PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_test(NAME domain_core_smoke COMMAND KQDomainCoreSmoke)
add_test(NAME domain_boundary COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_domain_boundary.py" --self-test)
add_test(NAME ui_boundary COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_ui_boundary.py" --self-test)

add_library(KQPetProtocol STATIC
  src/extension/packet_contract.cpp
  src/extension/packet_contract.h
  src/extension/session_context.cpp
  src/extension/session_context.h
)
target_include_directories(KQPetProtocol PUBLIC src/extension)
target_compile_options(KQPetProtocol PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetProtocol PUBLIC KQPetDomain Qt6::Core)
add_library(KQPetInbound STATIC src/application/inbound_queue.cpp src/application/inbound_queue.h)
target_include_directories(KQPetInbound PUBLIC src/application)
target_link_libraries(KQPetInbound PUBLIC KQPetProtocol Qt6::Core)
target_compile_options(KQPetInbound PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_library(KQPetTransport STATIC src/application/protocol_transport.cpp src/application/protocol_transport.h)
target_include_directories(KQPetTransport PUBLIC src/application)
target_link_libraries(KQPetTransport PUBLIC KQPetProtocol Qt6::Core)
target_compile_options(KQPetTransport PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_library(KQPetApplicationCore STATIC
  src/application/pet_detail_preparation_service.cpp
  src/application/pet_detail_preparation_service.h
  src/application/pet_derivation_cache.cpp
  src/application/pet_derivation_cache.h
  src/application/pet_record_cache.cpp
  src/application/pet_record_cache.h
  src/application/observation_freshness.cpp
  src/application/observation_freshness.h
  src/application/catalog_io_service.cpp
  src/application/catalog_io_service.h
  src/application/recommendation_adapter.cpp
  src/application/recommendation_adapter.h
  src/application/shop_legacy_adapters.cpp
  src/application/shop_legacy_adapters.h
  src/extension/move_operation.cpp
  src/extension/move_operation.h
  src/extension/account_analysis_state.h
  src/extension/asset_analysis_settings.cpp
  src/extension/asset_analysis_settings.h
  src/extension/asset_analysis_types.h
  src/extension/asset_analysis_version.h
  src/extension/asset_analyzer.cpp
  src/extension/asset_analyzer.h
  src/extension/asset_snapshot_store.cpp
  src/extension/asset_snapshot_store.h
  src/extension/pet_detail_analyzer.cpp
  src/extension/pet_detail_analyzer.h
  src/extension/pet_detail_catalog.cpp
  src/extension/pet_detail_catalog.h
  src/extension/pet_detail_view_model.h
  src/extension/pet_refresh_controller.cpp
  src/extension/pet_refresh_controller.h
  src/extension/pet_repository.cpp
  src/extension/pet_repository.h
  src/extension/shop_exchange_catalog.cpp
  src/extension/shop_exchange_catalog.h
  src/extension/shop_exchange_controller.cpp
  src/extension/shop_exchange_controller.h
  src/extension/routine_overview_catalog.cpp
  src/extension/routine_overview_catalog.h
  src/extension/routine_overview_controller.cpp
  src/extension/routine_overview_controller.h
)
target_include_directories(KQPetApplicationCore PUBLIC src/extension src/application)
target_compile_definitions(KQPetApplicationCore PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetApplicationCore PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetApplicationCore PUBLIC KQPetDomain KQPetDiagnostics KQPetProtocol KQPetStorage KQPetReadViews KQPetTransport Qt6::Core)
# Existing build/test names remain an aggregate application alias; the actual
# Domain target above has no dependency on this implementation boundary.
add_library(KQPetCore INTERFACE)
target_link_libraries(KQPetCore INTERFACE KQPetApplicationCore)
add_library(KQPet::Core ALIAS KQPetCore)

add_library(KQPetBridge STATIC
  src/extension/inline_hook.cpp
  src/extension/inline_hook.h
  src/extension/original_bridge.cpp
  src/extension/original_bridge.h
)
target_include_directories(KQPetBridge PUBLIC src/extension)

# Private fixed upstream MinHook 1.3.4 source; see third_party/minhook-provenance.json.
add_library(KQPetMinHook STATIC third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde64.c)
set_target_properties(KQPetMinHook PROPERTIES AUTOMOC OFF AUTORCC OFF)
target_include_directories(KQPetMinHook PUBLIC third_party/minhook/include)
target_compile_definitions(KQPetMinHook PRIVATE WIN32_LEAN_AND_MEAN)
target_link_libraries(KQPetBridge PRIVATE KQPetMinHook)
target_compile_definitions(KQPetBridge PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetBridge PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetBridge PUBLIC KQPetDiagnostics KQPetProtocol KQPetInbound Qt6::Core Qt6::Gui Qt6::Widgets)
add_library(KQPet::Bridge ALIAS KQPetBridge)

add_library(KQPetUi STATIC
  src/extension/pet_detail_renderer.cpp
  src/extension/pet_detail_renderer.h
  src/extension/pet_search.cpp
  src/extension/pet_search.h
  src/extension/prepared_pet_detail_renderer.cpp
  src/extension/pet_power_analysis_renderer.cpp
  src/extension/pet_power_analysis_renderer.h
  src/extension/stargod_ring_object.cpp
  src/extension/stargod_ring_object.h
  src/extension/prepared_pet_detail_renderer.h
  src/extension/workbench_types.h
  src/extension/workbench_window.h
  src/extension/workbench_window.cpp
  src/extension/asset_analysis_filter_proxy_model.cpp
  src/extension/asset_analysis_filter_proxy_model.h
  src/extension/asset_analysis_model.cpp
  src/extension/asset_analysis_model.h
  src/extension/asset_analysis_window.cpp
  src/extension/asset_analysis_window.h
  src/extension/pet_filter_proxy_model.cpp
  src/extension/pet_filter_proxy_model.h
  src/extension/pet_image_cache.cpp
  src/extension/pet_image_cache.h
  src/extension/pet_raw_data_tree.cpp
  src/extension/pet_raw_data_tree.h
  src/extension/pet_settings_dialog.cpp
  src/extension/pet_settings_dialog.h
  src/extension/pet_table_model.cpp
  src/extension/pet_table_model.h
  src/extension/pet_window.cpp
  src/extension/pet_window.h
  src/extension/recommendation_model.cpp
  src/extension/recommendation_model.h
  src/extension/shop_window.cpp
  src/extension/shop_window.h
  src/extension/routine_overview_window.cpp
  src/extension/routine_overview_window.h
  src/extension/snapshot_history_model.cpp
  src/extension/snapshot_history_model.h
)
target_include_directories(KQPetUi PUBLIC src/extension)
target_compile_definitions(KQPetUi PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetUi PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetUi PUBLIC KQPetReadViews KQPetDomain Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
add_library(KQPet::Ui ALIAS KQPetUi)

set(KQPET_RCC_BINARY "${CMAKE_CURRENT_BINARY_DIR}/KQPetInventory.rcc")
add_custom_command(OUTPUT "${KQPET_RCC_BINARY}"
  COMMAND Qt6::rcc --binary "${CMAKE_CURRENT_SOURCE_DIR}/src/extension/resources.qrc"
          -o "${KQPET_RCC_BINARY}"
  DEPENDS src/extension/resources.qrc assets/pet-detail-data.json assets/pet-image-urls.json
          assets/attribute-icons.png assets/shop-exchange-data.json ${KQPET_STARGOD_ICON_FILES}
          assets/activity-exchange-data.json
          tools/cache-manager.ps1 tools/public_data_updater.py tools/bootstrap-public-data.ps1
          tools/public_names_updater.py tools/public_icon_updater.py tools/public_routine_updater.py assets/stargod-icons/sources.json
          tools/public_activity_exchange_updater.py tools/activity_evolution_selector.py
          tools/generate_pet_detail_data.py tools/generate_shop_exchange_data.py tools/generate_stargod_icons.py VERBATIM)
configure_file(src/extension/embedded_resources.rc.in
  "${CMAKE_CURRENT_BINARY_DIR}/embedded_resources.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/embedded_resources.rc"
  PROPERTIES OBJECT_DEPENDS "${KQPET_RCC_BINARY}")

add_library(KQPetInventory SHARED
  src/extension/dllmain.cpp
  src/extension/extension_context.cpp
  src/extension/extension_context.h
  src/extension/original_window_locator.cpp
  src/extension/original_window_locator.h
  "${CMAKE_CURRENT_BINARY_DIR}/embedded_resources.rc"
  "${KQPET_GENERATED_INCLUDE_DIR}/build_identity.rc"
  "${KQPET_RCC_BINARY}"
)
set_target_properties(KQPetInventory PROPERTIES AUTORCC OFF)
target_compile_definitions(KQPetInventory PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN KQ_PET_EXTENSION_BUILD)
target_compile_options(KQPetInventory PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetInventory PRIVATE KQPetBridge KQPetUi KQPetApplication KQPetStartup)
kqpet_use_build_info(KQPetInventory)

add_executable(KQPetCatalogSmoke
  tests/catalog_smoke.cpp
  src/extension/resources.qrc
)
target_include_directories(KQPetCatalogSmoke PRIVATE src/extension)
target_compile_options(KQPetCatalogSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetCatalogSmoke PRIVATE KQPetCore)

enable_testing()
add_executable(KQInboundQueueSmoke tests/inbound_queue_smoke.cpp)
target_link_libraries(KQInboundQueueSmoke PRIVATE KQPetInbound)
target_compile_options(KQInboundQueueSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME inbound_queue_smoke COMMAND KQInboundQueueSmoke)
add_executable(KQProtocolTransportSmoke tests/protocol_transport_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQProtocolTransportSmoke PRIVATE KQPetCore)
target_compile_options(KQProtocolTransportSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME protocol_transport_smoke COMMAND KQProtocolTransportSmoke)
set_tests_properties(protocol_transport_smoke PROPERTIES TIMEOUT 20)
add_executable(KQCompatibilityCoreSmoke tests/compatibility_core_smoke.cpp)
target_link_libraries(KQCompatibilityCoreSmoke PRIVATE KQPetCompatibility)
target_compile_options(KQCompatibilityCoreSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(KQCompatibilityCoreSmoke PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_test(NAME compatibility_core_smoke COMMAND KQCompatibilityCoreSmoke)

add_library(KQPetStorage STATIC src/storage/storage_service.cpp src/storage/storage_service.h
  src/storage/storage_write_context.cpp src/storage/storage_write_context.h src/storage/storage_types.h
  src/storage/diagnostic_store.cpp src/storage/diagnostic_store.h)
target_include_directories(KQPetStorage PUBLIC src/storage)
target_link_libraries(KQPetStorage PUBLIC Qt6::Core)
target_link_libraries(KQPetStorage PRIVATE bcrypt)
target_link_libraries(KQPetDiagnostics PUBLIC KQPetStorage PRIVATE bcrypt)
target_compile_options(KQPetStorage PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_library(KQPetReadViews STATIC src/application/inventory_read_view.h
  src/application/inventory_projection.h src/application/inventory_projection.cpp
  src/application/analysis_read_view.h src/application/analysis_projection.h src/application/analysis_projection.cpp)
target_include_directories(KQPetReadViews PUBLIC src/application src/extension)
target_link_libraries(KQPetReadViews PUBLIC KQPetDomain Qt6::Core)
target_compile_options(KQPetReadViews PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_executable(KQStorageQueueSmoke tests/storage_queue_smoke.cpp)
target_link_libraries(KQStorageQueueSmoke PRIVATE KQPetStorage)
target_compile_options(KQStorageQueueSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME storage_queue_smoke COMMAND KQStorageQueueSmoke)
set_tests_properties(storage_queue_smoke PROPERTIES TIMEOUT 20)
add_executable(KQDiagnosticSmoke tests/diagnostic_smoke.cpp)
target_link_libraries(KQDiagnosticSmoke PRIVATE KQPetDiagnostics KQPetStorage Qt6::Core bcrypt)
target_compile_definitions(KQDiagnosticSmoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQDiagnosticSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME diagnostic_smoke COMMAND KQDiagnosticSmoke)
set_tests_properties(diagnostic_smoke PROPERTIES TIMEOUT 20)
add_executable(KQCatalogIoSmoke tests/catalog_io_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQCatalogIoSmoke PRIVATE KQPetCore)
target_compile_options(KQCatalogIoSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME catalog_io_smoke COMMAND KQCatalogIoSmoke)
add_executable(KQActivityShopCatalogSmoke tests/activity_shop_catalog_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQActivityShopCatalogSmoke PRIVATE KQPetApplicationCore)
target_compile_options(KQActivityShopCatalogSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME activity_shop_catalog_smoke COMMAND KQActivityShopCatalogSmoke)
add_executable(KQActivityShopObservationSmoke tests/activity_shop_observation_smoke.cpp)
target_include_directories(KQActivityShopObservationSmoke PRIVATE src)
target_link_libraries(KQActivityShopObservationSmoke PRIVATE KQPetDomain)
target_compile_options(KQActivityShopObservationSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME activity_shop_observation_smoke COMMAND KQActivityShopObservationSmoke)
add_executable(KQActivityShopControllerSmoke tests/activity_shop_controller_smoke.cpp src/extension/resources.qrc)
target_include_directories(KQActivityShopControllerSmoke PRIVATE src)
target_link_libraries(KQActivityShopControllerSmoke PRIVATE KQPetApplicationCore)
target_compile_options(KQActivityShopControllerSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME activity_shop_controller_smoke COMMAND KQActivityShopControllerSmoke)
set_tests_properties(activity_shop_controller_smoke PROPERTIES TIMEOUT 30)
set_tests_properties(catalog_io_smoke PROPERTIES TIMEOUT 30)
add_executable(KQObservationFreshnessSmoke tests/observation_freshness_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQObservationFreshnessSmoke PRIVATE KQPetCore)
target_compile_options(KQObservationFreshnessSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME observation_freshness_smoke COMMAND KQObservationFreshnessSmoke)
set_tests_properties(observation_freshness_smoke PROPERTIES TIMEOUT 20)
add_executable(KQPetDerivationCacheSmoke tests/pet_derivation_cache_smoke.cpp)
target_link_libraries(KQPetDerivationCacheSmoke PRIVATE KQPetCore)
target_compile_options(KQPetDerivationCacheSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME pet_derivation_cache_smoke COMMAND KQPetDerivationCacheSmoke)
set_tests_properties(pet_derivation_cache_smoke PROPERTIES TIMEOUT 20)
add_executable(KQPetRecordCacheSmoke tests/pet_record_cache_smoke.cpp)
target_link_libraries(KQPetRecordCacheSmoke PRIVATE KQPetCore)
target_compile_options(KQPetRecordCacheSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME pet_record_cache_smoke COMMAND KQPetRecordCacheSmoke)
set_tests_properties(pet_record_cache_smoke PROPERTIES TIMEOUT 20)
add_executable(KQPetDetailPreparationSmoke tests/pet_detail_preparation_smoke.cpp)
target_link_libraries(KQPetDetailPreparationSmoke PRIVATE KQPetDomain)
target_compile_options(KQPetDetailPreparationSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME pet_detail_preparation_smoke COMMAND KQPetDetailPreparationSmoke)
add_executable(KQPetDetailServiceSmoke tests/pet_detail_service_smoke.cpp)
target_link_libraries(KQPetDetailServiceSmoke PRIVATE KQPetCore)
target_compile_options(KQPetDetailServiceSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME pet_detail_service_smoke COMMAND KQPetDetailServiceSmoke)

add_executable(KQSelectedDetailRefreshSmoke tests/selected_detail_refresh_smoke.cpp src/extension/resources.qrc)
target_include_directories(KQSelectedDetailRefreshSmoke PRIVATE tests)
target_link_libraries(KQSelectedDetailRefreshSmoke PRIVATE KQPetUi KQPetApplication)
target_compile_options(KQSelectedDetailRefreshSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME selected_detail_refresh_smoke COMMAND KQSelectedDetailRefreshSmoke)
set_tests_properties(selected_detail_refresh_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
set_tests_properties(pet_detail_preparation_smoke pet_detail_service_smoke PROPERTIES TIMEOUT 30)
add_executable(KQPetRecordRepositorySmoke tests/pet_record_repository_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQPetRecordRepositorySmoke PRIVATE KQPetCore)
target_compile_options(KQPetRecordRepositorySmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME pet_record_repository_smoke COMMAND KQPetRecordRepositorySmoke)
set_tests_properties(pet_record_repository_smoke PROPERTIES TIMEOUT 30)
add_executable(KQSnapshotStorageSmoke tests/snapshot_storage_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQSnapshotStorageSmoke PRIVATE KQPetCore)
target_compile_options(KQSnapshotStorageSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME snapshot_storage_smoke COMMAND KQSnapshotStorageSmoke)
set_tests_properties(snapshot_storage_smoke PROPERTIES TIMEOUT 120)
add_library(KQPetAnalysisWorker STATIC src/application/analysis_worker.cpp src/application/analysis_worker.h)
target_include_directories(KQPetAnalysisWorker PUBLIC src/application)
target_link_libraries(KQPetAnalysisWorker PUBLIC KQPetDomain Qt6::Core)
target_compile_options(KQPetAnalysisWorker PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_library(KQPetApplication STATIC src/application/inventory_publisher.cpp src/application/inventory_publisher.h
  src/application/local_stargod_statistics_service.cpp src/application/local_stargod_statistics_service.h
  src/application/analysis_publisher.cpp src/application/analysis_publisher.h
  src/application/application_runtime.cpp src/application/application_runtime.h src/application/runtime_types.h
  src/application/data_update_service.cpp src/application/data_update_service.h
  src/application/cache_management_service.cpp src/application/cache_management_service.h
  src/extension/asset_analysis_controller.cpp src/extension/asset_analysis_controller.h)
target_include_directories(KQPetApplication PUBLIC src/application)
target_link_libraries(KQPetApplication PUBLIC KQPetApplicationCore KQPetReadViews KQPetAnalysisWorker)
target_compile_options(KQPetApplication PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_library(KQPetImages STATIC src/application/image_service.cpp src/application/image_service.h)
target_include_directories(KQPetImages PUBLIC src/application)
target_link_libraries(KQPetImages PUBLIC Qt6::Core Qt6::Gui Qt6::Network)
target_link_libraries(KQPetImages PRIVATE KQPetStorage)
target_compile_options(KQPetImages PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQPetApplication PRIVATE KQPetImages)
add_executable(KQLocalStargodStatisticsSmoke tests/local_stargod_statistics_smoke.cpp)
target_link_libraries(KQLocalStargodStatisticsSmoke PRIVATE KQPetApplication KQPetStorage)
target_compile_options(KQLocalStargodStatisticsSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME local_stargod_statistics_smoke COMMAND KQLocalStargodStatisticsSmoke)
add_executable(KQCacheManagementServiceSmoke tests/cache_management_service_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQCacheManagementServiceSmoke PRIVATE KQPetApplication KQPetStorage)
target_compile_options(KQCacheManagementServiceSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME cache_management_service_smoke COMMAND KQCacheManagementServiceSmoke)
set_tests_properties(cache_management_service_smoke PROPERTIES TIMEOUT 60)
target_link_libraries(KQPetUi PRIVATE KQPetImages)
add_executable(KQImageServiceSmoke tests/image_service_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQImageServiceSmoke PRIVATE KQPetImages KQPetUi KQPetApplication KQPetStorage Qt6::Test Qt6::Widgets Qt6::Network)
target_compile_options(KQImageServiceSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME image_service_smoke COMMAND KQImageServiceSmoke)
set_tests_properties(image_service_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
target_link_libraries(KQPetInventory PRIVATE KQPetApplication)
add_executable(KQApplicationRuntimeSmoke tests/application_runtime_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQApplicationRuntimeSmoke PRIVATE KQPetApplication)
target_compile_options(KQApplicationRuntimeSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME application_runtime_smoke COMMAND KQApplicationRuntimeSmoke)
set_tests_properties(application_runtime_smoke PROPERTIES TIMEOUT 20)
add_executable(KQInventoryProjectionSmoke tests/inventory_projection_smoke.cpp)
target_link_libraries(KQInventoryProjectionSmoke PRIVATE KQPetReadViews)
target_compile_options(KQInventoryProjectionSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME inventory_projection_smoke COMMAND KQInventoryProjectionSmoke)
add_executable(KQAnalysisCacheIntegrationSmoke tests/analysis_cache_integration_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQAnalysisCacheIntegrationSmoke PRIVATE KQPetApplication)
target_compile_options(KQAnalysisCacheIntegrationSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME analysis_cache_integration_smoke COMMAND KQAnalysisCacheIntegrationSmoke)
set_tests_properties(analysis_cache_integration_smoke PROPERTIES TIMEOUT 45)
add_executable(KQAnalysisWorkerSmoke tests/analysis_worker_smoke.cpp)
target_link_libraries(KQAnalysisWorkerSmoke PRIVATE KQPetAnalysisWorker)
target_compile_options(KQAnalysisWorkerSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME analysis_worker_smoke COMMAND KQAnalysisWorkerSmoke)
set_tests_properties(analysis_worker_smoke PROPERTIES TIMEOUT 20)
add_executable(KQStartupChannelSmoke tests/startup_channel_smoke.cpp)
target_link_libraries(KQStartupChannelSmoke PRIVATE KQPetStartup)
target_compile_options(KQStartupChannelSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(KQStartupChannelSmoke PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(KQStartupChannelSmoke PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_test(NAME startup_channel_smoke COMMAND KQStartupChannelSmoke)
set_tests_properties(startup_channel_smoke PROPERTIES TIMEOUT 15)
add_executable(KQMoveOperationSmoke tests/move_operation_smoke.cpp)
target_link_libraries(KQMoveOperationSmoke PRIVATE KQPetCore)
target_compile_options(KQMoveOperationSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME move_operation_smoke COMMAND KQMoveOperationSmoke)
add_executable(KQOutboundContractSmoke tests/outbound_contract_smoke.cpp)
target_link_libraries(KQOutboundContractSmoke PRIVATE KQPetProtocol)
target_compile_options(KQOutboundContractSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME outbound_contract_smoke COMMAND KQOutboundContractSmoke)
add_executable(KQAlgorithmPipelineSmoke tests/algorithm_pipeline_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQAlgorithmPipelineSmoke PRIVATE KQPetCore)
target_compile_options(KQAlgorithmPipelineSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME algorithm_pipeline_smoke COMMAND KQAlgorithmPipelineSmoke)
add_test(NAME pet_detail_catalog_smoke COMMAND KQPetCatalogSmoke)

add_executable(KQPetRepositorySmoke
  tests/repository_smoke.cpp
  src/extension/resources.qrc
)
target_include_directories(KQPetRepositorySmoke PRIVATE src/extension)
target_compile_options(KQPetRepositorySmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetRepositorySmoke PRIVATE KQPetCore)
add_test(NAME pet_repository_smoke COMMAND KQPetRepositorySmoke)

add_executable(KQPetRefreshSmoke
  tests/refresh_controller_smoke.cpp
  src/extension/resources.qrc
)
target_include_directories(KQPetRefreshSmoke PRIVATE src/extension)
target_compile_options(KQPetRefreshSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetRefreshSmoke PRIVATE KQPetCore)
add_test(NAME pet_refresh_controller_smoke COMMAND KQPetRefreshSmoke)

add_executable(KQPetMoveSmoke
  tests/move_controller_smoke.cpp
  src/extension/resources.qrc
)
target_include_directories(KQPetMoveSmoke PRIVATE src/extension)
target_compile_options(KQPetMoveSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetMoveSmoke PRIVATE KQPetCore)
add_test(NAME pet_move_controller_smoke COMMAND KQPetMoveSmoke)

add_executable(KQPetSearchSmoke
  tests/search_smoke.cpp
)
target_include_directories(KQPetSearchSmoke PRIVATE src/extension)
target_compile_definitions(KQPetSearchSmoke PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetSearchSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetSearchSmoke PRIVATE KQPetUi)
add_test(NAME pet_search_smoke COMMAND KQPetSearchSmoke)

add_executable(KQPetTableModelSmoke
  tests/pet_table_model_smoke.cpp
  src/extension/resources.qrc
)
target_include_directories(KQPetTableModelSmoke PRIVATE src/extension)
target_compile_options(KQPetTableModelSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetTableModelSmoke PRIVATE KQPetUi KQPetApplication Qt6::Test)
add_test(NAME pet_table_model_smoke COMMAND KQPetTableModelSmoke)

add_executable(KQAssetAnalysisSmoke
  tests/asset_analysis_smoke.cpp
  src/extension/resources.qrc
)
target_include_directories(KQAssetAnalysisSmoke PRIVATE src/extension)
target_compile_options(KQAssetAnalysisSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQAssetAnalysisSmoke PRIVATE KQPetApplication)
add_test(NAME asset_analysis_smoke COMMAND KQAssetAnalysisSmoke)

add_executable(KQRecommendationSmoke
  tests/recommendation_smoke.cpp
  src/extension/resources.qrc
)
target_include_directories(KQRecommendationSmoke PRIVATE src/extension)
target_compile_options(KQRecommendationSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQRecommendationSmoke PRIVATE KQPetCore)
add_test(NAME recommendation_smoke COMMAND KQRecommendationSmoke)

add_executable(KQAssetAnalysisPerformance
  tests/asset_analysis_performance.cpp
  tests/performance_dataset.cpp
  tests/performance_dataset.h
  src/extension/resources.qrc
)
target_include_directories(KQAssetAnalysisPerformance PRIVATE src/extension)
target_compile_options(KQAssetAnalysisPerformance PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQAssetAnalysisPerformance PRIVATE KQPetUi KQPetApplication)
add_test(NAME asset_analysis_performance COMMAND KQAssetAnalysisPerformance)

add_executable(KQPetShopSmoke
  tests/shop_exchange_smoke.cpp
  src/extension/resources.qrc
)
target_include_directories(KQPetShopSmoke PRIVATE src/extension)
target_compile_options(KQPetShopSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetShopSmoke PRIVATE KQPetCore)
add_test(NAME shop_exchange_smoke COMMAND KQPetShopSmoke)
add_executable(KQCultivationMaterialInventorySmoke tests/cultivation_material_inventory_smoke.cpp src/extension/resources.qrc)
target_link_libraries(KQCultivationMaterialInventorySmoke PRIVATE KQPetApplicationCore)
target_compile_options(KQCultivationMaterialInventorySmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME cultivation_material_inventory_smoke COMMAND KQCultivationMaterialInventorySmoke)
set_tests_properties(cultivation_material_inventory_smoke PROPERTIES TIMEOUT 30)

add_executable(KQRoutineOverviewSmoke
  tests/routine_overview_smoke.cpp
  src/extension/resources.qrc
)
target_include_directories(KQRoutineOverviewSmoke PRIVATE src/extension)
target_compile_options(KQRoutineOverviewSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQRoutineOverviewSmoke PRIVATE KQPetCore)
add_test(NAME routine_overview_smoke COMMAND KQRoutineOverviewSmoke)

add_executable(KQProtocolFixtureSmoke
  tests/fixture_smoke.cpp
  src/extension/resources.qrc
)
target_include_directories(KQProtocolFixtureSmoke PRIVATE src/extension)
target_compile_definitions(
  KQProtocolFixtureSmoke PRIVATE
  KQPET_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
)
target_compile_options(KQProtocolFixtureSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQProtocolFixtureSmoke PRIVATE KQPetCore KQPetUi)
add_test(NAME protocol_fixture_smoke COMMAND KQProtocolFixtureSmoke)

add_executable(KQInlineHookPolicySmoke tests/inline_hook_policy_smoke.cpp)
target_include_directories(KQInlineHookPolicySmoke PRIVATE src/extension)
target_compile_options(KQInlineHookPolicySmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQInlineHookPolicySmoke PRIVATE KQPetBridge)
add_test(NAME inline_hook_policy_smoke COMMAND KQInlineHookPolicySmoke)
add_executable(KQInlineHookRuntimeSmoke tests/inline_hook_runtime_smoke.cpp)
target_compile_definitions(KQInlineHookRuntimeSmoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQInlineHookRuntimeSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQInlineHookRuntimeSmoke PRIVATE KQPetBridge)
add_test(NAME inline_hook_runtime_smoke COMMAND KQInlineHookRuntimeSmoke)
set_tests_properties(inline_hook_runtime_smoke PROPERTIES TIMEOUT 20)

add_executable(KQTargetProfileSmoke tests/target_profile_smoke.cpp)
target_compile_definitions(KQTargetProfileSmoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQTargetProfileSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQTargetProfileSmoke PRIVATE KQPetDiagnostics)
add_test(NAME target_profile_smoke COMMAND KQTargetProfileSmoke)

add_executable(KQLauncherTargetSmoke tests/launcher_target_smoke.cpp src/loader/client_target.cpp)
target_include_directories(KQLauncherTargetSmoke PRIVATE src/loader)
target_link_libraries(KQLauncherTargetSmoke PRIVATE KQPetCompatibility)
target_compile_options(KQLauncherTargetSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME launcher_target_smoke COMMAND KQLauncherTargetSmoke)

add_executable(KQLoaderModuleSmoke tests/loader_module_smoke.cpp src/loader/remote_module.cpp)
target_include_directories(KQLoaderModuleSmoke PRIVATE src/loader)
target_compile_definitions(KQLoaderModuleSmoke PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQLoaderModuleSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(KQLoaderModuleSmoke PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_test(NAME loader_module_smoke COMMAND KQLoaderModuleSmoke)
set_tests_properties(loader_module_smoke PROPERTIES TIMEOUT 12)

add_executable(KQOriginalWindowLocatorSmoke tests/original_window_locator_smoke.cpp
  src/extension/original_window_locator.cpp src/extension/original_window_locator.h)
target_include_directories(KQOriginalWindowLocatorSmoke PRIVATE src/extension)
target_compile_options(KQOriginalWindowLocatorSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQOriginalWindowLocatorSmoke PRIVATE Qt6::Widgets)
add_test(NAME original_window_locator_smoke COMMAND KQOriginalWindowLocatorSmoke)

add_test(NAME target_tools_smoke COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
  -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/target-tools-smoke.ps1")

get_target_property(KQ_QT_CORE_RUNTIME Qt6::Core IMPORTED_LOCATION_RELEASE)
if(NOT KQ_QT_CORE_RUNTIME)
  get_target_property(KQ_QT_CORE_RUNTIME Qt6::Core IMPORTED_LOCATION)
endif()
get_filename_component(KQ_QT_RUNTIME_DIR "${KQ_QT_CORE_RUNTIME}" DIRECTORY)
set_tests_properties(
  move_operation_smoke
  inbound_queue_smoke
  protocol_transport_smoke
  storage_queue_smoke
  diagnostic_smoke
  catalog_io_smoke
  activity_shop_catalog_smoke
  activity_shop_observation_smoke
  activity_shop_controller_smoke
  cache_management_service_smoke
  observation_freshness_smoke
  pet_derivation_cache_smoke
  pet_record_cache_smoke
  pet_detail_preparation_smoke
  pet_detail_service_smoke
  selected_detail_refresh_smoke
  pet_record_repository_smoke
  domain_core_smoke
  local_stargod_statistics_smoke
  power_consumers_smoke
  shop_pet_parameterized_smoke
  pet_power_composition_smoke
  pet_cultivation_requirements_smoke
  cultivation_material_inventory_smoke
  image_service_smoke
  snapshot_storage_smoke
  analysis_worker_smoke
  inventory_projection_smoke
  application_runtime_smoke
  analysis_cache_integration_smoke
  outbound_contract_smoke algorithm_pipeline_smoke
  pet_detail_catalog_smoke pet_repository_smoke pet_refresh_controller_smoke
  pet_move_controller_smoke pet_search_smoke pet_table_model_smoke shop_exchange_smoke
  asset_analysis_smoke routine_overview_smoke protocol_fixture_smoke
  recommendation_smoke
  asset_analysis_performance
  inline_hook_policy_smoke
  inline_hook_runtime_smoke
  target_profile_smoke
  original_window_locator_smoke
  PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${KQ_QT_RUNTIME_DIR}"
)
set_tests_properties(asset_analysis_performance
  PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20)
set_tests_properties(target_tools_smoke
  PROPERTIES ENVIRONMENT "KQPET_COMPATIBILITY_CHECK=$<TARGET_FILE:KQPetCompatibilityCheck>")
set_tests_properties(original_window_locator_smoke
  PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)

add_executable(KQPetUiPreview
  tests/ui_preview.cpp
  src/extension/resources.qrc
)
target_include_directories(KQPetUiPreview PRIVATE src/extension)
target_compile_options(KQPetUiPreview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetUiPreview PRIVATE KQPetUi KQPetApplication)
kqpet_use_build_info(KQPetUiPreview)
add_executable(KQWorkbenchUiPreview tests/workbench_ui_preview.cpp src/extension/resources.qrc)
target_link_libraries(KQWorkbenchUiPreview PRIVATE KQPetUi KQPetApplication Qt6::Test)
target_compile_options(KQWorkbenchUiPreview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
add_test(NAME workbench_ui_preview_smoke COMMAND KQWorkbenchUiPreview)
set_tests_properties(workbench_ui_preview_smoke PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;KQPET_PREVIEW_SELF_TEST=1"
  ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${KQ_QT_RUNTIME_DIR}"
  TIMEOUT 20)

add_executable(KQShopUiPreview
  tests/shop_ui_preview.cpp
  src/extension/resources.qrc
)
target_include_directories(KQShopUiPreview PRIVATE src/extension)
target_compile_options(KQShopUiPreview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQShopUiPreview PRIVATE KQPetUi KQPetApplication)
kqpet_use_build_info(KQShopUiPreview)

add_executable(KQRoutineUiPreview
  tests/routine_ui_preview.cpp
)
target_include_directories(KQRoutineUiPreview PRIVATE src/extension)
target_compile_options(KQRoutineUiPreview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQRoutineUiPreview PRIVATE KQPetUi KQPetApplication)
kqpet_use_build_info(KQRoutineUiPreview)

add_executable(KQAssetAnalysisUiPreview
  tests/asset_analysis_ui_preview.cpp
  src/extension/resources.qrc
)
target_include_directories(KQAssetAnalysisUiPreview PRIVATE src/extension)
target_compile_options(KQAssetAnalysisUiPreview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQAssetAnalysisUiPreview PRIVATE KQPetUi KQPetApplication)
kqpet_use_build_info(KQAssetAnalysisUiPreview)

add_test(NAME pet_ui_preview_smoke COMMAND KQPetUiPreview)
add_test(NAME shop_ui_preview_smoke COMMAND KQShopUiPreview)
add_test(NAME routine_ui_preview_smoke COMMAND KQRoutineUiPreview)
add_test(NAME asset_analysis_ui_preview_smoke COMMAND KQAssetAnalysisUiPreview)
set_tests_properties(
  pet_ui_preview_smoke shop_ui_preview_smoke routine_ui_preview_smoke
  asset_analysis_ui_preview_smoke
  PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;KQPET_PREVIEW_SELF_TEST=1;KQPET_PREVIEW_EXIT_MS=3000"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${KQ_QT_RUNTIME_DIR}"
    TIMEOUT 15
)

get_property(KQPET_OUTPUT_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS KQPET_OUTPUT_TARGETS)
  get_target_property(output_target_type ${target} TYPE)
  if(output_target_type STREQUAL "INTERFACE_LIBRARY")
    continue()
  endif()
  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
  )
  foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set_target_properties(${target} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin/${config}"
      LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin/${config}"
      ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/lib/${config}"
    )
  endforeach()
endforeach()
