cmake_minimum_required(VERSION 3.24)

project(KQPetInventoryExtension VERSION 1.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This project only supports 64-bit Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

find_package(Qt6 6.6.3 EXACT REQUIRED COMPONENTS Core Gui Widgets Network)

add_executable(KQPetLauncher WIN32
  src/loader/main.cpp
)
target_compile_definitions(KQPetLauncher PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetLauncher PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQPetLauncher PRIVATE user32)

add_library(KQPetInventory SHARED
  src/extension/dllmain.cpp
  src/extension/extension_context.cpp
  src/extension/extension_context.h
  src/extension/inline_hook.cpp
  src/extension/inline_hook.h
  src/extension/original_bridge.cpp
  src/extension/original_bridge.h
  src/extension/pet_detail_catalog.cpp
  src/extension/pet_detail_catalog.h
  src/extension/pet_identity.cpp
  src/extension/pet_identity.h
  src/extension/pet_image_cache.cpp
  src/extension/pet_image_cache.h
  src/extension/pet_move_policy.cpp
  src/extension/pet_move_policy.h
  src/extension/pet_refresh_controller.cpp
  src/extension/pet_refresh_controller.h
  src/extension/pet_repository.cpp
  src/extension/pet_repository.h
  src/extension/pet_search.cpp
  src/extension/pet_search.h
  src/extension/pet_settings_dialog.cpp
  src/extension/pet_settings_dialog.h
  src/extension/pet_window.cpp
  src/extension/pet_window.h
  src/extension/shop_exchange_catalog.cpp
  src/extension/shop_exchange_catalog.h
  src/extension/shop_exchange_controller.cpp
  src/extension/shop_exchange_controller.h
  src/extension/shop_pet_eligibility.cpp
  src/extension/shop_pet_eligibility.h
  src/extension/shop_window.cpp
  src/extension/shop_window.h
  src/extension/routine_overview_catalog.cpp
  src/extension/routine_overview_catalog.h
  src/extension/routine_overview_controller.cpp
  src/extension/routine_overview_controller.h
  src/extension/routine_overview_window.cpp
  src/extension/routine_overview_window.h
  src/extension/resources.qrc
)
target_compile_definitions(KQPetInventory PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN KQ_PET_EXTENSION_BUILD)
target_compile_options(KQPetInventory PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetInventory PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)

add_executable(KQPetCatalogSmoke
  tests/catalog_smoke.cpp
  src/extension/pet_detail_catalog.cpp
  src/extension/pet_detail_catalog.h
  src/extension/pet_identity.cpp
  src/extension/pet_identity.h
  src/extension/resources.qrc
)
target_include_directories(KQPetCatalogSmoke PRIVATE src/extension)
target_compile_options(KQPetCatalogSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetCatalogSmoke PRIVATE Qt6::Core)

enable_testing()
add_test(NAME pet_detail_catalog_smoke COMMAND KQPetCatalogSmoke)

add_executable(KQPetRepositorySmoke
  tests/repository_smoke.cpp
  src/extension/pet_detail_catalog.cpp
  src/extension/pet_detail_catalog.h
  src/extension/pet_identity.cpp
  src/extension/pet_identity.h
  src/extension/pet_repository.cpp
  src/extension/pet_repository.h
  src/extension/resources.qrc
)
target_include_directories(KQPetRepositorySmoke PRIVATE src/extension)
target_compile_options(KQPetRepositorySmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetRepositorySmoke PRIVATE Qt6::Core)
add_test(NAME pet_repository_smoke COMMAND KQPetRepositorySmoke)

add_executable(KQPetRefreshSmoke
  tests/refresh_controller_smoke.cpp
  src/extension/pet_detail_catalog.cpp
  src/extension/pet_detail_catalog.h
  src/extension/pet_identity.cpp
  src/extension/pet_identity.h
  src/extension/pet_move_policy.cpp
  src/extension/pet_move_policy.h
  src/extension/pet_refresh_controller.cpp
  src/extension/pet_refresh_controller.h
  src/extension/pet_repository.cpp
  src/extension/pet_repository.h
  src/extension/resources.qrc
)
target_include_directories(KQPetRefreshSmoke PRIVATE src/extension)
target_compile_options(KQPetRefreshSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetRefreshSmoke PRIVATE Qt6::Core)
add_test(NAME pet_refresh_controller_smoke COMMAND KQPetRefreshSmoke)

add_executable(KQPetMoveSmoke
  tests/move_controller_smoke.cpp
  src/extension/pet_detail_catalog.cpp
  src/extension/pet_detail_catalog.h
  src/extension/pet_identity.cpp
  src/extension/pet_identity.h
  src/extension/pet_move_policy.cpp
  src/extension/pet_move_policy.h
  src/extension/pet_refresh_controller.cpp
  src/extension/pet_refresh_controller.h
  src/extension/pet_repository.cpp
  src/extension/pet_repository.h
  src/extension/resources.qrc
)
target_include_directories(KQPetMoveSmoke PRIVATE src/extension)
target_compile_options(KQPetMoveSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetMoveSmoke PRIVATE Qt6::Core)
add_test(NAME pet_move_controller_smoke COMMAND KQPetMoveSmoke)

add_executable(KQPetSearchSmoke
  tests/search_smoke.cpp
  src/extension/pet_search.cpp
  src/extension/pet_search.h
)
target_include_directories(KQPetSearchSmoke PRIVATE src/extension)
target_compile_definitions(KQPetSearchSmoke PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetSearchSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetSearchSmoke PRIVATE Qt6::Core)
add_test(NAME pet_search_smoke COMMAND KQPetSearchSmoke)

add_executable(KQPetShopSmoke
  tests/shop_exchange_smoke.cpp
  src/extension/pet_detail_catalog.cpp
  src/extension/pet_detail_catalog.h
  src/extension/pet_identity.cpp
  src/extension/pet_identity.h
  src/extension/pet_repository.cpp
  src/extension/pet_repository.h
  src/extension/shop_exchange_catalog.cpp
  src/extension/shop_exchange_catalog.h
  src/extension/shop_exchange_controller.cpp
  src/extension/shop_exchange_controller.h
  src/extension/shop_pet_eligibility.cpp
  src/extension/shop_pet_eligibility.h
  src/extension/resources.qrc
)
target_include_directories(KQPetShopSmoke PRIVATE src/extension)
target_compile_options(KQPetShopSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetShopSmoke PRIVATE Qt6::Core)
add_test(NAME shop_exchange_smoke COMMAND KQPetShopSmoke)

add_executable(KQRoutineOverviewSmoke
  tests/routine_overview_smoke.cpp
  src/extension/pet_detail_catalog.cpp
  src/extension/pet_detail_catalog.h
  src/extension/pet_identity.cpp
  src/extension/pet_identity.h
  src/extension/pet_repository.cpp
  src/extension/pet_repository.h
  src/extension/routine_overview_catalog.cpp
  src/extension/routine_overview_catalog.h
  src/extension/routine_overview_controller.cpp
  src/extension/routine_overview_controller.h
  src/extension/resources.qrc
)
target_include_directories(KQRoutineOverviewSmoke PRIVATE src/extension)
target_compile_options(KQRoutineOverviewSmoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQRoutineOverviewSmoke PRIVATE Qt6::Core)
add_test(NAME routine_overview_smoke COMMAND KQRoutineOverviewSmoke)

get_target_property(KQ_QT_CORE_RUNTIME Qt6::Core IMPORTED_LOCATION_RELEASE)
if(NOT KQ_QT_CORE_RUNTIME)
  get_target_property(KQ_QT_CORE_RUNTIME Qt6::Core IMPORTED_LOCATION)
endif()
get_filename_component(KQ_QT_RUNTIME_DIR "${KQ_QT_CORE_RUNTIME}" DIRECTORY)
set_tests_properties(
  pet_detail_catalog_smoke pet_repository_smoke pet_refresh_controller_smoke
  pet_move_controller_smoke pet_search_smoke shop_exchange_smoke
  routine_overview_smoke
  PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${KQ_QT_RUNTIME_DIR}"
)

add_executable(KQPetUiPreview
  tests/ui_preview.cpp
  src/extension/pet_detail_catalog.cpp
  src/extension/pet_detail_catalog.h
  src/extension/pet_identity.cpp
  src/extension/pet_identity.h
  src/extension/pet_image_cache.cpp
  src/extension/pet_image_cache.h
  src/extension/pet_move_policy.cpp
  src/extension/pet_move_policy.h
  src/extension/pet_refresh_controller.cpp
  src/extension/pet_refresh_controller.h
  src/extension/pet_repository.cpp
  src/extension/pet_repository.h
  src/extension/pet_search.cpp
  src/extension/pet_search.h
  src/extension/pet_settings_dialog.cpp
  src/extension/pet_settings_dialog.h
  src/extension/pet_window.cpp
  src/extension/pet_window.h
  src/extension/resources.qrc
)
target_include_directories(KQPetUiPreview PRIVATE src/extension)
target_compile_options(KQPetUiPreview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetUiPreview PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)

add_executable(KQShopUiPreview
  tests/shop_ui_preview.cpp
  src/extension/pet_detail_catalog.cpp
  src/extension/pet_detail_catalog.h
  src/extension/pet_identity.cpp
  src/extension/pet_identity.h
  src/extension/pet_image_cache.cpp
  src/extension/pet_image_cache.h
  src/extension/pet_move_policy.cpp
  src/extension/pet_move_policy.h
  src/extension/pet_repository.cpp
  src/extension/pet_repository.h
  src/extension/pet_search.cpp
  src/extension/pet_search.h
  src/extension/pet_window.cpp
  src/extension/pet_window.h
  src/extension/shop_exchange_catalog.cpp
  src/extension/shop_exchange_catalog.h
  src/extension/shop_pet_eligibility.cpp
  src/extension/shop_pet_eligibility.h
  src/extension/shop_window.cpp
  src/extension/shop_window.h
  src/extension/resources.qrc
)
target_include_directories(KQShopUiPreview PRIVATE src/extension)
target_compile_options(KQShopUiPreview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQShopUiPreview PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)

add_executable(KQRoutineUiPreview
  tests/routine_ui_preview.cpp
  src/extension/routine_overview_catalog.cpp
  src/extension/routine_overview_catalog.h
  src/extension/routine_overview_window.cpp
  src/extension/routine_overview_window.h
)
target_include_directories(KQRoutineUiPreview PRIVATE src/extension)
target_compile_options(KQRoutineUiPreview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQRoutineUiPreview PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

foreach(target KQPetLauncher KQPetInventory KQPetCatalogSmoke KQPetRepositorySmoke KQPetRefreshSmoke KQPetMoveSmoke KQPetSearchSmoke KQPetShopSmoke KQRoutineOverviewSmoke KQPetUiPreview KQShopUiPreview KQRoutineUiPreview)
  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  )
  foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set_target_properties(${target} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin/${config}"
      LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin/${config}"
    )
  endforeach()
endforeach()
