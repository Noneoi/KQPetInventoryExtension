cmake_minimum_required(VERSION 3.24)

project(KQPetInventoryExtension VERSION 3.0.0 LANGUAGES C CXX)

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

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Every project include is written relative to src/ ("domain/pet_era.h",
# "application/pet/pet_repository.h"), so a header name can never silently
# resolve to a same-named file in another layer.
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/src")
enable_testing()

# Fragments are include()d, not add_subdirectory()'d: every target, relative
# source path and CMAKE_CURRENT_{SOURCE,BINARY}_DIR stays at the project root.
include(cmake/build_identity.cmake)  # version header, source hash, release id
include(cmake/native.cmake)          # compatibility, launcher, bootstrap, release
include(cmake/core.cmake)            # domain, storage, diagnostics, protocol, application
include(cmake/extension.cmake)       # bridge, UI, KQPetInventory.dll
include(cmake/tests.cmake)           # registered tests

# Uniform output layout: bin/<Config> for executables/DLL, lib/<Config> for archives.
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
