cmake_minimum_required(VERSION 3.20)
project(Degrees720 CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  add_compile_options(/bigobj /utf-8)
endif()

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(D720_ROM_DIR "${REPO_ROOT}/Game" CACHE PATH "Folder containing the 720 Degrees (rev 4, MAME set '720') ROM files")
option(D720_REGENERATE "Re-run the static translators at configure time" OFF)

# ---------------------------------------------------------------------------
# Static translation of the arcade program ROMs into C++ (src/game).
# The generated code is derived from the user's ROMs and is not part of the repository.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(D720_REGENERATE OR NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/game/files.cmake
   OR NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/game/s65_code.cpp)
  if(NOT EXISTS ${D720_ROM_DIR}/136047-3126.7lm)
    message(FATAL_ERROR "720 Degrees ROM files not found in '${D720_ROM_DIR}'. "
      "Copy the rev 4 ROM set (MAME '720') there or pass -DD720_ROM_DIR=<folder>.")
  endif()
  message(STATUS "Translating the 720 Degrees program ROMs to C++ ...")
  foreach(tool t11_to_cpp.py m6502_to_cpp.py)
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E env D720_ROM_DIR=${D720_ROM_DIR} ${Python3_EXECUTABLE} -I tools/${tool}
      WORKING_DIRECTORY ${REPO_ROOT}
      RESULT_VARIABLE res OUTPUT_VARIABLE out ERROR_VARIABLE out)
    message(STATUS "  ${tool}: ${out}")
    if(NOT res EQUAL 0)
      message(FATAL_ERROR "${tool} failed")
    endif()
  endforeach()
endif()
add_custom_target(regenerate
  COMMAND ${CMAKE_COMMAND} -E env D720_ROM_DIR=${D720_ROM_DIR} ${Python3_EXECUTABLE} -I tools/t11_to_cpp.py
  COMMAND ${CMAKE_COMMAND} -E env D720_ROM_DIR=${D720_ROM_DIR} ${Python3_EXECUTABLE} -I tools/m6502_to_cpp.py
  WORKING_DIRECTORY ${REPO_ROOT}
  COMMENT "Re-translating the program ROMs (re-run CMake afterwards)")

# ---------------------------------------------------------------------------
# SDL3: use an installed package if available, otherwise download the official
# development package for this toolchain into third_party/.
set(SDL3_VERSION 3.2.28)
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
  set(SDL3_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL3-${SDL3_VERSION})
  if(MINGW)
    set(SDL3_PKG mingw)
    set(SDL3_DIR ${SDL3_ROOT}/x86_64-w64-mingw32/lib/cmake/SDL3)
  else()
    set(SDL3_PKG VC)
    set(SDL3_DIR ${SDL3_ROOT}/cmake)
  endif()
  if(NOT EXISTS ${SDL3_DIR}/SDL3Config.cmake)
    set(zip ${CMAKE_BINARY_DIR}/SDL3-devel-${SDL3_VERSION}-${SDL3_PKG}.zip)
    message(STATUS "Downloading SDL3 ${SDL3_VERSION} (${SDL3_PKG})")
    file(DOWNLOAD https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VERSION}/SDL3-devel-${SDL3_VERSION}-${SDL3_PKG}.zip
      ${zip} STATUS st)
    list(GET st 0 code)
    if(NOT code EQUAL 0)
      message(FATAL_ERROR "SDL3 download failed: ${st}")
    endif()
    file(ARCHIVE_EXTRACT INPUT ${zip} DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
  endif()
  find_package(SDL3 REQUIRED CONFIG)
endif()

# ---------------------------------------------------------------------------
# Shared board support (memory map, video, sound chips)
add_library(board STATIC
  src/board/board.cpp
  src/board/soundchips.cpp
  src/board/tms5220.cpp
  third_party/ymfm/ymfm_opm.cpp)
target_include_directories(board PUBLIC src/board third_party/ymfm)

# ---------------------------------------------------------------------------
# Reference interpreters (development oracle only: verification and RE tools)
add_library(refcpu STATIC
  src/ref/t11.cpp
  src/ref/m6502.cpp
  src/ref/refmachine.cpp)
target_include_directories(refcpu PUBLIC src/ref)
target_link_libraries(refcpu PUBLIC board)

# ---------------------------------------------------------------------------
# Native game core: translated programs + runtime
include(src/game/files.cmake)
add_library(gamecore STATIC
  src/native/t11rt.cpp
  src/native/coro_win32.cpp
  src/native/nativemachine.cpp
  src/native/s65rt.cpp
  src/game/s65_code.cpp
  src/game/s65_tables.cpp
  ${T11_GENERATED})
target_include_directories(gamecore PUBLIC src/native src/game)
target_link_libraries(gamecore PUBLIC board)
if(NOT MSVC)
  set_source_files_properties(${T11_GENERATED} src/game/s65_code.cpp PROPERTIES COMPILE_OPTIONS "-O1;-fno-strict-aliasing")
endif()

# ---------------------------------------------------------------------------
# Level data library
add_library(leveldata STATIC src/level/leveldata.cpp)
target_include_directories(leveldata PUBLIC src/level)
target_link_libraries(leveldata PUBLIC board)

# ---------------------------------------------------------------------------
# The game
add_executable(720degrees WIN32 src/app/main.cpp)
target_link_libraries(720degrees PRIVATE gamecore leveldata SDL3::SDL3)
set(D720_GAME_DIR "${REPO_ROOT}/Game" CACHE PATH "Folder the finished game is copied to (with the ROMs)")
add_custom_command(TARGET 720degrees POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:720degrees>
  COMMAND ${CMAKE_COMMAND} -E make_directory ${D720_GAME_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:720degrees> $<TARGET_FILE:SDL3::SDL3> ${D720_GAME_DIR}
  COMMENT "Copying the game to ${D720_GAME_DIR}")

# ---------------------------------------------------------------------------
# Level editor (Dear ImGui is vendored in third_party/imgui)
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

add_executable(editor720 WIN32 src/editor/editor.cpp)
target_link_libraries(editor720 PRIVATE gamecore leveldata imgui SDL3::SDL3)
add_custom_command(TARGET editor720 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:editor720>
  COMMAND ${CMAKE_COMMAND} -E make_directory ${D720_GAME_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:editor720> $<TARGET_FILE:SDL3::SDL3> ${D720_GAME_DIR}
  COMMENT "Copying the level editor to ${D720_GAME_DIR}")

# ---------------------------------------------------------------------------
# Tools
add_executable(ref720 src/ref/refmain.cpp)
target_link_libraries(ref720 PRIVATE refcpu)
add_executable(verify720 src/tools/verify720.cpp)
target_link_libraries(verify720 PRIVATE gamecore refcpu)
add_executable(render720 src/tools/render720.cpp)
target_link_libraries(render720 PRIVATE gamecore leveldata)
target_include_directories(leveldata PUBLIC src/native)
add_executable(leveltool src/tools/leveltool.cpp)
target_link_libraries(leveltool PRIVATE leveldata board)

if(MINGW)
  # Link the MinGW runtime (libgcc, libstdc++, winpthread) statically so the programs run
  # outside MSYS2.  SDL3 is still used through its import library and ships as SDL3.dll.
  foreach(t 720degrees editor720 ref720 verify720 render720 leveltool)
    target_link_options(${t} PRIVATE -static)
  endforeach()
endif()
