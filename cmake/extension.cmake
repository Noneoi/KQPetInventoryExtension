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
  src/ui/battle/battle_replay_page.cpp src/ui/battle/battle_replay_page.h
  src/ui/battle/battle_replay_life_timeline.cpp src/ui/battle/battle_replay_life_timeline.h
  src/ui/battle/battle_replay_life_timeline_data.h
  src/ui/battle/battle_replay_timeline_events.h
  src/ui/battle/battle_replay_cell.cpp src/ui/battle/battle_replay_cell.h
  src/ui/battle/battle_replay_meter_overlay.cpp src/ui/battle/battle_replay_meter_overlay.h
  src/ui/battle/battle_replay_action_meters.h
  src/ui/battle/battle_replay_meter_sequence.h
  src/ui/battle/battle_replay_numbers.h
  src/ui/battle/battle_replay_spirit.cpp src/ui/battle/battle_replay_spirit.h
  src/ui/battle/battle_replay_links.cpp src/ui/battle/battle_replay_links.h
  src/ui/battle/battle_replay_source.cpp src/ui/battle/battle_replay_source.h
  src/ui/battle/battle_replay_paths.h
  src/ui/battle/battle_replay_inspection.h src/ui/battle/battle_replay_buff_presentation.h
  src/ui/battle/battle_replay_reference_library.h
  src/ui/battle/battle_replay_buff_history.h
  src/ui/battle/battle_replay_identity_presentation.h
  src/ui/workbench/workbench_types.h
  src/ui/workbench/workbench_window.h
  src/ui/workbench/workbench_window.cpp
  src/ui/workbench/pet_settings_dialog.cpp
  src/ui/workbench/pet_settings_dialog.h
  src/ui/workbench/public_update_panel.cpp src/ui/workbench/public_update_panel.h
  src/ui/detail/public_data_detail_dialog.cpp src/ui/detail/public_data_detail_dialog.h
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
  src/ui/detail/pet_skill_renderer.cpp
  src/ui/detail/pet_skill_renderer.h
  src/ui/detail/stargod_ring_object.cpp
  src/ui/detail/stargod_ring_object.h
  src/ui/detail/pet_raw_data_tree.cpp
  src/ui/detail/pet_raw_data_tree.h
  src/ui/shop/shop_window.cpp
  src/ui/shop/shop_window.h
  src/ui/shop/shop_selection_dialog.cpp
  src/ui/shop/shop_selection_dialog.h
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
# The updater runs the same domain parser against frozen real reports before
# publishing a candidate. Embed the small Core-only helper and its exact Qt
# dependency; it does not depend on the inventory resources (no build cycle).
add_executable(KQBattleReplayUpdateCheck
  src/tools/battle_replay_update_check.cpp
  src/tools/battle_replay_update_audit.h
  src/application/catalog/battle_replay_catalog_pack.cpp
  src/application/catalog/battle_replay_catalog_validation.cpp
  assets/battle-replay-regression/fixtures.qrc)
target_link_libraries(KQBattleReplayUpdateCheck PRIVATE KQPetDomain Qt6::Core Qt6::ZlibPrivate)
target_compile_options(KQBattleReplayUpdateCheck PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
set_target_properties(KQBattleReplayUpdateCheck PROPERTIES AUTOMOC OFF)
add_executable(KQPublicDatasetCheck src/tools/public_dataset_check.cpp)
target_link_libraries(KQPublicDatasetCheck PRIVATE KQPetApplicationCore)
target_compile_options(KQPublicDatasetCheck PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
set_target_properties(KQPublicDatasetCheck PROPERTIES AUTOMOC OFF AUTORCC OFF)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)
set(KQPET_UPDATE_CRT_FILES)
set(KQPET_UPDATE_CRT_QRC "")
foreach(runtime_name msvcp140.dll msvcp140_1.dll vcruntime140.dll vcruntime140_1.dll)
  set(runtime_found FALSE)
  foreach(runtime_file IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    get_filename_component(runtime_basename "${runtime_file}" NAME)
    if(runtime_basename STREQUAL runtime_name)
      list(APPEND KQPET_UPDATE_CRT_FILES "${runtime_file}")
      string(APPEND KQPET_UPDATE_CRT_QRC "<file alias=\"${runtime_name}\">${runtime_file}</file>")
      set(runtime_found TRUE)
      break()
    endif()
  endforeach()
  if(NOT runtime_found)
    message(FATAL_ERROR "Replay update checker needs the compiler redistributable: ${runtime_name}")
  endif()
endforeach()
set(KQPET_UPDATE_NATIVE_QRC "${CMAKE_CURRENT_BINARY_DIR}/update-native-$<CONFIG>.qrc")
file(GENERATE OUTPUT "${KQPET_UPDATE_NATIVE_QRC}" CONTENT
  "<RCC><qresource prefix=\"/kqpet/data-updater\"><file alias=\"KQBattleReplayUpdateCheck.exe\">$<TARGET_FILE:KQBattleReplayUpdateCheck></file><file alias=\"KQPublicDatasetCheck.exe\">$<TARGET_FILE:KQPublicDatasetCheck></file><file alias=\"Qt6Core.dll\">$<TARGET_FILE:Qt6::Core></file>${KQPET_UPDATE_CRT_QRC}</qresource></RCC>\n")
set(KQPET_REPLAY_UPDATE_INPUTS
  tools/public_battle_replay_updater.py assets/battle-replay-update-sources.zip
  tools/battle_replay/build_catalog.py tools/battle_replay/catalog_pack.py
  tools/battle_replay/fetch_reference.py tools/battle_replay/fetch_pet_reference.py
  tools/battle_replay/fetch_formation_reference.py tools/battle_replay/extract_buff_tables.py
  tools/battle_replay/extract_skill_tables.py tools/battle_replay/extract_skill_embed_effect.py
  tools/battle_replay/extract_entry_tables.py tools/battle_replay/extract_pet_mechanisms.py
  tools/battle_replay/extract_pet_identity.py tools/battle_replay/extract_presentation_config.py
  tools/battle_replay/extract_pet_dragons.py tools/battle_replay/extract_replay_supplements.py
  tools/battle_replay/extract_replay_cultivation.py tools/battle_replay/link_battle_tables.py
  tools/battle_replay/audit_reference_coverage.py tools/battle_replay/update_generated_evidence.py
  tools/public_catalog_quality.py tools/public_skill_fidelity.py tools/public_replay_fidelity.py
  tools/public_replay_regression.py tools/battle_replay/audit_source_coverage.py
  tools/battle_replay/update_source_bundle.py)
set(KQPET_RCC_BINARY "${CMAKE_CURRENT_BINARY_DIR}/KQPetInventory.rcc")
add_custom_command(OUTPUT "${KQPET_RCC_BINARY}"
  COMMAND Qt6::rcc --binary "${CMAKE_CURRENT_SOURCE_DIR}/src/extension/resources.qrc" "${KQPET_UPDATE_NATIVE_QRC}"
          -o "${KQPET_RCC_BINARY}"
  DEPENDS src/extension/resources.qrc "${KQPET_UPDATE_NATIVE_QRC}" KQBattleReplayUpdateCheck KQPublicDatasetCheck Qt6::Core
          ${KQPET_UPDATE_CRT_FILES}
          assets/battle-replay-catalog.json assets/pet-detail-data.json assets/pet-skill-data.json assets/pet-image-urls.json
          assets/attribute-icons.png assets/shop-exchange-data.json ${KQPET_STARGOD_ICON_FILES}
          assets/activity-exchange-data.json assets/battle-replay-regression/manifest.json ${KQPET_REPLAY_UPDATE_INPUTS}
          tools/cache-manager.ps1 tools/public_data_updater.py tools/bootstrap-public-data.ps1
          tools/public_resource_session.py tools/public_resource_plan.py tools/public_cultivation_supplement.py
          tools/public_names_updater.py tools/public_icon_updater.py tools/public_routine_updater.py assets/stargod-icons/sources.json
          tools/public_routine_rules.py tools/public_dataset_store.py
          tools/public_activity_exchange_updater.py tools/activity_evolution_selector.py tools/activity_reward_structures.py tools/activity_periods.py tools/activity_trade_facts.py
          tools/generate_pet_detail_data.py tools/generate_pet_skill_data.py tools/public_skill_updater.py
          tools/generate_shop_exchange_data.py tools/generate_stargod_icons.py VERBATIM)
configure_file(src/extension/embedded_resources.rc.in
  "${CMAKE_CURRENT_BINARY_DIR}/embedded_resources.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/embedded_resources.rc"
  PROPERTIES OBJECT_DEPENDS "${KQPET_RCC_BINARY}")

# --- KQPetInventory.dll: composition root -----------------------------------
# External shared-data artifact. Keep the existing Qt-compressed embedded
# resource as a fallback; do not embed a second copy of this package.
set(KQPET_BATTLE_REPLAY_PACK "${CMAKE_CURRENT_BINARY_DIR}/data-packs/battle-replay-catalog.brpack")
add_custom_command(OUTPUT "${KQPET_BATTLE_REPLAY_PACK}"
  COMMAND Python3::Interpreter -X utf8 -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/battle_replay/catalog_pack.py"
          --input "${CMAKE_CURRENT_SOURCE_DIR}/assets/battle-replay-catalog.json"
          --output "${KQPET_BATTLE_REPLAY_PACK}"
  DEPENDS tools/battle_replay/catalog_pack.py assets/battle-replay-catalog.json
  VERBATIM)
add_custom_target(KQBattleReplayCatalogPack DEPENDS "${KQPET_BATTLE_REPLAY_PACK}")
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
add_dependencies(KQPetInventory KQBattleReplayCatalogPack)
target_compile_definitions(KQPetInventory PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN KQ_PET_EXTENSION_BUILD)
target_compile_options(KQPetInventory PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /wd4828>)
target_link_libraries(KQPetInventory PRIVATE KQPetBridge KQPetUi KQPetApplication KQPetStartup)
kqpet_use_build_info(KQPetInventory)
