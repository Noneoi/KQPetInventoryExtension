# Build identity: version header, source-input hash, release id and the
# generated build_identity/version resources shared by the launcher and DLL.
# Included from the top-level CMakeLists.txt, so CMAKE_CURRENT_SOURCE_DIR and
# CMAKE_CURRENT_BINARY_DIR still name the project root and build root.

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
  "${CMAKE_CURRENT_SOURCE_DIR}/docs/*" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/*"
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
  src/runtime/version.h.in
  "${KQPET_GENERATED_INCLUDE_DIR}/version.h"
  @ONLY
)

function(kqpet_use_build_info target)
  target_include_directories(${target} PRIVATE "${KQPET_GENERATED_INCLUDE_DIR}")
endfunction()
