cmake_minimum_required(VERSION 3.24)

project(KQPetInventoryExtension VERSION 1.3.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This project only supports 64-bit Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

find_package(Qt6 6.6.3 EXACT REQUIRED COMPONENTS Core Gui Widgets Network)

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
set(KQPET_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${KQPET_GENERATED_INCLUDE_DIR}")
configure_file(
  src/extension/version.h.in
  "${KQPET_GENERATED_INCLUDE_DIR}/version.h"
  @ONLY
)

function(kqpet_use_build_info target)
  target_include_directories(${target} PRIVATE "${KQPET_GENERATED_INCLUDE_DIR}")
endfunction()

add_library(KQPetDiagnostics STATIC
  src/extension/build_info.cpp
  src/extension/build_info.h
  src/extension/diagnostic_logger.cpp
  src/extension/diagnostic_logger.h
  src/extension/target_compatibility_guard.cpp
  src/extension/target_compatibility_guard.h
)
target_include_directories(KQPetDiagnostics PUBLIC src/extension
                           "${KQPET_GENERATED_INCLUDE_DIR}")
target_compile_definitions(KQPetDiagnostics PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetDiagnostics PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetDiagnostics PUBLIC Qt6::Core PRIVATE version)

add_executable(KQPetLauncher WIN32
  src/loader/main.cpp
)
target_compile_definitions(KQPetLauncher PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetLauncher PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQPetLauncher PRIVATE user32)

add_library(KQPetCore STATIC
  src/extension/asset_analysis_controller.cpp
  src/extension/asset_analysis_controller.h
  src/extension/pet_detail_analyzer.cpp
  src/extension/pet_detail_analyzer.h
  src/extension/pet_detail_catalog.cpp
  src/extension/pet_detail_catalog.h
  src/extension/pet_detail_renderer.cpp
  src/extension/pet_detail_renderer.h
  src/extension/pet_detail_view_model.h
  src/extension/pet_identity.cpp
  src/extension/pet_identity.h
  src/extension/pet_move_policy.cpp
  src/extension/pet_move_policy.h
  src/extension/pet_refresh_controller.cpp
  src/extension/pet_refresh_controller.h
  src/extension/pet_repository.cpp
  src/extension/pet_repository.h
  src/extension/pet_search.cpp
  src/extension/pet_search.h
  src/extension/shop_exchange_catalog.cpp
  src/extension/shop_exchange_catalog.h
  src/extension/shop_exchange_controller.cpp
  src/extension/shop_exchange_controller.h
  src/extension/shop_pet_eligibility.cpp
  src/extension/shop_pet_eligibility.h
  src/extension/routine_overview_catalog.cpp
  src/extension/routine_overview_catalog.h
  src/extension/routine_overview_controller.cpp
  src/extension/routine_overview_controller.h
)
target_include_directories(KQPetCore PUBLIC src/extension)
target_compile_definitions(KQPetCore PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetCore PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetCore PUBLIC KQPetDiagnostics Qt6::Core)
add_library(KQPet::Core ALIAS KQPetCore)

add_library(KQPetBridge STATIC
  src/extension/inline_hook.cpp
  src/extension/inline_hook.h
  src/extension/original_bridge.cpp
  src/extension/original_bridge.h
)
target_include_directories(KQPetBridge PUBLIC src/extension)
target_compile_definitions(KQPetBridge PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetBridge PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetBridge PUBLIC KQPetDiagnostics Qt6::Core Qt6::Gui Qt6::Widgets)
add_library(KQPet::Bridge ALIAS KQPetBridge)

add_library(KQPetUi STATIC
  src/extension/asset_analysis_window.cpp
  src/extension/asset_analysis_window.h
  src/extension/pet_filter_proxy_model.cpp
  src/extension/pet_filter_proxy_model.h
  src/extension/pet_image_cache.cpp
  src/extension/pet_image_cache.h
  src/extension/pet_settings_dialog.cpp
  src/extension/pet_settings_dialog.h
  src/extension/pet_table_model.cpp
  src/extension/pet_table_model.h
  src/extension/pet_window.cpp
  src/extension/pet_window.h
  src/extension/shop_window.cpp
  src/extension/shop_window.h
  src/extension/routine_overview_window.cpp
  src/extension/routine_overview_window.h
)
target_include_directories(KQPetUi PUBLIC src/extension)
target_compile_definitions(KQPetUi PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetUi PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetUi PUBLIC KQPetCore Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
add_library(KQPet::Ui ALIAS KQPetUi)

add_library(KQPetInventory SHARED
  src/extension/dllmain.cpp
  src/extension/extension_context.cpp
  src/extension/extension_context.h
  src/extension/resources.qrc
)
target_compile_definitions(KQPetInventory PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN KQ_PET_EXTENSION_BUILD)
target_compile_options(KQPetInventory PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetInventory PRIVATE KQPetBridge KQPetUi)
kqpet_use_build_info(KQPetInventory)

add_executable(KQPetCatalogSmoke
  tests/catalog_smoke.cpp
  src/extension/resources.qrc
)
target_include_directories(KQPetCatalogSmoke PRIVATE src/extension)
target_compile_options(KQPetCatalogSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetCatalogSmoke PRIVATE KQPetCore)

enable_testing()
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
target_link_libraries(KQPetSearchSmoke PRIVATE KQPetCore)
add_test(NAME pet_search_smoke COMMAND KQPetSearchSmoke)

add_executable(KQPetTableModelSmoke
  tests/pet_table_model_smoke.cpp
  src/extension/resources.qrc
)
target_include_directories(KQPetTableModelSmoke PRIVATE src/extension)
target_compile_options(KQPetTableModelSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetTableModelSmoke PRIVATE KQPetUi)
add_test(NAME pet_table_model_smoke COMMAND KQPetTableModelSmoke)

add_executable(KQAssetAnalysisSmoke
  tests/asset_analysis_smoke.cpp
  src/extension/resources.qrc
)
target_include_directories(KQAssetAnalysisSmoke PRIVATE src/extension)
target_compile_options(KQAssetAnalysisSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQAssetAnalysisSmoke PRIVATE KQPetCore)
add_test(NAME asset_analysis_smoke COMMAND KQAssetAnalysisSmoke)

add_executable(KQPetShopSmoke
  tests/shop_exchange_smoke.cpp
  src/extension/resources.qrc
)
target_include_directories(KQPetShopSmoke PRIVATE src/extension)
target_compile_options(KQPetShopSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetShopSmoke PRIVATE KQPetCore)
add_test(NAME shop_exchange_smoke COMMAND KQPetShopSmoke)

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
target_link_libraries(KQProtocolFixtureSmoke PRIVATE KQPetCore)
add_test(NAME protocol_fixture_smoke COMMAND KQProtocolFixtureSmoke)

get_target_property(KQ_QT_CORE_RUNTIME Qt6::Core IMPORTED_LOCATION_RELEASE)
if(NOT KQ_QT_CORE_RUNTIME)
  get_target_property(KQ_QT_CORE_RUNTIME Qt6::Core IMPORTED_LOCATION)
endif()
get_filename_component(KQ_QT_RUNTIME_DIR "${KQ_QT_CORE_RUNTIME}" DIRECTORY)
set_tests_properties(
  pet_detail_catalog_smoke pet_repository_smoke pet_refresh_controller_smoke
  pet_move_controller_smoke pet_search_smoke pet_table_model_smoke shop_exchange_smoke
  asset_analysis_smoke routine_overview_smoke protocol_fixture_smoke
  PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${KQ_QT_RUNTIME_DIR}"
)

add_executable(KQPetUiPreview
  tests/ui_preview.cpp
  src/extension/resources.qrc
)
target_include_directories(KQPetUiPreview PRIVATE src/extension)
target_compile_options(KQPetUiPreview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetUiPreview PRIVATE KQPetUi)
kqpet_use_build_info(KQPetUiPreview)

add_executable(KQShopUiPreview
  tests/shop_ui_preview.cpp
  src/extension/resources.qrc
)
target_include_directories(KQShopUiPreview PRIVATE src/extension)
target_compile_options(KQShopUiPreview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQShopUiPreview PRIVATE KQPetUi)
kqpet_use_build_info(KQShopUiPreview)

add_executable(KQRoutineUiPreview
  tests/routine_ui_preview.cpp
)
target_include_directories(KQRoutineUiPreview PRIVATE src/extension)
target_compile_options(KQRoutineUiPreview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQRoutineUiPreview PRIVATE KQPetUi)
kqpet_use_build_info(KQRoutineUiPreview)

add_executable(KQAssetAnalysisUiPreview
  tests/asset_analysis_ui_preview.cpp
  src/extension/resources.qrc
)
target_include_directories(KQAssetAnalysisUiPreview PRIVATE src/extension)
target_compile_options(KQAssetAnalysisUiPreview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQAssetAnalysisUiPreview PRIVATE KQPetUi)
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

foreach(target KQPetDiagnostics KQPetCore KQPetBridge KQPetUi KQPetLauncher KQPetInventory KQPetCatalogSmoke KQPetRepositorySmoke KQPetRefreshSmoke KQPetMoveSmoke KQPetSearchSmoke KQPetTableModelSmoke KQAssetAnalysisSmoke KQPetShopSmoke KQRoutineOverviewSmoke KQProtocolFixtureSmoke KQPetUiPreview KQShopUiPreview KQRoutineUiPreview KQAssetAnalysisUiPreview)
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
