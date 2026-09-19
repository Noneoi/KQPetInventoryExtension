# Native (no Qt) side: client compatibility checks, the startup channel, the
# versioned launcher, the stable bootstrap entry and release manifest tooling.

# --- Compatibility: PE/profile checks run before any Qt call -----------------
add_custom_command(OUTPUT "${KQPET_GENERATED_INCLUDE_DIR}/target_profiles.cpp"
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/build/generate_profiles.py"
          --input "${CMAKE_CURRENT_SOURCE_DIR}/profiles/targets.json"
          --output "${KQPET_GENERATED_INCLUDE_DIR}/target_profiles.cpp"
  DEPENDS tools/build/generate_profiles.py profiles/targets.json VERBATIM)
add_library(KQPetCompatibility STATIC src/compatibility/pe_view.cpp
  src/compatibility/target_check.cpp "${KQPET_GENERATED_INCLUDE_DIR}/target_profiles.cpp")
target_compile_definitions(KQPetCompatibility PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetCompatibility PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQPetCompatibility PRIVATE version bcrypt)
set_target_properties(KQPetCompatibility PROPERTIES AUTOMOC OFF AUTORCC OFF)

add_executable(KQPetCompatibilityCheck src/compatibility/check_main.cpp)
target_link_libraries(KQPetCompatibilityCheck PRIVATE KQPetCompatibility)
target_compile_options(KQPetCompatibilityCheck PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(KQPetCompatibilityCheck PROPERTIES AUTOMOC OFF AUTORCC OFF)

# --- Startup channel shared by launcher and DLL ------------------------------
add_library(KQPetStartup STATIC src/runtime/startup_channel.cpp src/runtime/startup_channel.h)
target_include_directories(KQPetStartup PUBLIC "${KQPET_GENERATED_INCLUDE_DIR}")
target_compile_definitions(KQPetStartup PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetStartup PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQPetStartup PRIVATE bcrypt advapi32)
set_target_properties(KQPetStartup PROPERTIES AUTOMOC OFF AUTORCC OFF)

# --- Release manifest/activation and bootstrap core --------------------------
add_library(KQPetReleaseCore STATIC src/runtime/release_manifest.cpp src/runtime/release_manifest.h
  src/runtime/release_activation.cpp src/runtime/release_activation.h
  src/bootstrap/bootstrap.cpp src/bootstrap/bootstrap.h src/bootstrap/strict_json.h
  src/loader/client_target.cpp)
target_link_libraries(KQPetReleaseCore PUBLIC KQPetCompatibility PRIVATE bcrypt)
target_compile_definitions(KQPetReleaseCore PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetReleaseCore PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(KQPetReleaseCore PROPERTIES AUTOMOC OFF AUTORCC OFF)

add_executable(KQPetReleaseCheck src/runtime/release_check_main.cpp)
target_link_libraries(KQPetReleaseCheck PRIVATE KQPetReleaseCore)
target_compile_options(KQPetReleaseCheck PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(KQPetReleaseCheck PROPERTIES AUTOMOC OFF AUTORCC OFF)

# --- Versioned launcher (KQPetRuntime\releases\<id>\KQPetLauncher.exe) -------
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
target_link_libraries(KQPetLauncher PRIVATE user32 KQPetCompatibility KQPetStartup shell32 KQPetReleaseCore)
kqpet_use_build_info(KQPetLauncher)

# --- Stable bootstrap entry (packaged as the client-root KQPetLauncher.exe) --
configure_file(src/bootstrap/bootstrap_identity.rc.in "${KQPET_GENERATED_INCLUDE_DIR}/bootstrap_identity.rc" @ONLY)
set_source_files_properties("${KQPET_GENERATED_INCLUDE_DIR}/bootstrap_identity.rc"
  PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap/bootstrap_identity.json")
add_executable(KQPetBootstrap WIN32 src/bootstrap/main.cpp "${KQPET_GENERATED_INCLUDE_DIR}/bootstrap_identity.rc")
target_compile_definitions(KQPetBootstrap PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(KQPetBootstrap PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(KQPetBootstrap PRIVATE KQPetReleaseCore shell32)
set_target_properties(KQPetBootstrap PROPERTIES AUTOMOC OFF AUTORCC OFF)
