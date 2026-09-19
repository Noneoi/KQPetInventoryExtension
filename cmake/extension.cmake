# In-process extension: host bridge (inline hooks), Qt Widgets UI and the
# KQPetInventory.dll composition root with its embedded resources.

# --- Bridge: MinHook-based hooks into the original client -------------------
# Private fixed upstream MinHook 1.3.4 source; see third_party/minhook-provenance.json.
add_library(KQPetMinHook STATIC third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde64.c)
set_target_properties(KQPetMinHook PROPERTIES AUTOMOC OFF AUTORCC OFF)
target_include_directories(KQPetMinHook PUBLIC third_party/minhook/include)
target_compile_definitions(KQPetMinHook PRIVATE WIN32_LEAN_AND_MEAN)

add_library(KQPetBridge STATIC
  src/bridge/inline_hook.cpp
  src/bridge/inline_hook.h
  src/bridge/original_bridge.cpp
  src/bridge/original_bridge.h
)
target_compile_definitions(KQPetBridge PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetBridge PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetBridge PRIVATE KQPetMinHook
  PUBLIC KQPetDiagnostics KQPetProtocol KQPetInbound Qt6::Core Qt6::Gui Qt6::Widgets)
add_library(KQPet::Bridge ALIAS KQPetBridge)

# --- UI: windows, models and renderers --------------------------------------
# Allowed dependencies are enforced by tools/build/check_ui_boundary.py.
add_library(KQPetUi STATIC
  src/ui/workbench/workbench_types.h
  src/ui/workbench/workbench_window.h
  src/ui/workbench/workbench_window.cpp
  src/ui/workbench/pet_settings_dialog.cpp
  src/ui/workbench/pet_settings_dialog.h
  src/ui/pet/pet_search.cpp
  src/ui/pet/pet_search.h
  src/ui/pet/pet_filter_proxy_model.cpp
  src/ui/pet/pet_filter_proxy_model.h
  src/ui/pet/pet_table_model.cpp
  src/ui/pet/pet_table_model.h
  src/ui/pet/pet_table_view.cpp
  src/ui/pet/pet_table_view.h
  src/ui/pet/pet_window.cpp
  src/ui/pet/pet_window.h
  src/ui/detail/html_document.h
  src/ui/detail/prepared_pet_detail_renderer.cpp
  src/ui/detail/prepared_pet_detail_renderer.h
  src/ui/detail/pet_power_analysis_renderer.cpp
  src/ui/detail/pet_power_analysis_renderer.h
  src/ui/detail/stargod_ring_object.cpp
  src/ui/detail/stargod_ring_object.h
  src/ui/detail/pet_raw_data_tree.cpp
  src/ui/detail/pet_raw_data_tree.h
  src/ui/shop/shop_window.cpp
  src/ui/shop/shop_window.h
  src/ui/routine/routine_overview_window.cpp
  src/ui/routine/routine_overview_window.h
  src/ui/analysis/asset_analysis_filter_proxy_model.cpp
  src/ui/analysis/asset_analysis_filter_proxy_model.h
  src/ui/analysis/asset_analysis_model.cpp
  src/ui/analysis/asset_analysis_model.h
  src/ui/analysis/asset_analysis_window.cpp
  src/ui/analysis/asset_analysis_window.h
  src/ui/analysis/recommendation_model.cpp
  src/ui/analysis/recommendation_model.h
  src/ui/analysis/snapshot_history_model.cpp
  src/ui/analysis/snapshot_history_model.h
  src/ui/common/display_text.h
  src/ui/common/pet_image_cache.cpp
  src/ui/common/pet_image_cache.h
  src/ui/common/ui_preferences.cpp
  src/ui/common/ui_preferences.h
)
target_compile_definitions(KQPetUi PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetUi PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetUi
  PUBLIC KQPetReadViews KQPetDomain Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network PRIVATE KQPetImages)
add_library(KQPet::Ui ALIAS KQPetUi)

# --- Embedded resources (catalog data, icons, bundled update tools) ---------
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

# --- KQPetInventory.dll: composition root -----------------------------------
add_library(KQPetInventory SHARED
  src/extension/dllmain.cpp
  src/extension/extension_context.cpp
  src/extension/extension_context.h
  src/bridge/original_window_locator.cpp
  src/bridge/original_window_locator.h
  "${CMAKE_CURRENT_BINARY_DIR}/embedded_resources.rc"
  "${KQPET_GENERATED_INCLUDE_DIR}/build_identity.rc"
  "${KQPET_RCC_BINARY}"
)
set_target_properties(KQPetInventory PROPERTIES AUTORCC OFF)
target_compile_definitions(KQPetInventory PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN KQ_PET_EXTENSION_BUILD)
target_compile_options(KQPetInventory PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetInventory PRIVATE KQPetBridge KQPetUi KQPetApplication KQPetStartup)
kqpet_use_build_info(KQPetInventory)
