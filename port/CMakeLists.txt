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
# OFF builds only what does not need the ROMs (board, reference CPUs, level library,
# tools, coroutine test): used by CI, where no ROMs are available.
option(D720_GAME "Build the game, the level editor and the tools that use the translated ROM code" ON)

# ---------------------------------------------------------------------------
# Static translation of the arcade program ROMs into C++ (src/game).
# The generated code is derived from the user's ROMs and is not part of the repository.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(D720_GAME)
# re-translate whenever the translators or their inputs change (hash stamp in src/game)
set(D720_TRANSLATOR_INPUTS
  ${REPO_ROOT}/tools/t11_to_cpp.py ${REPO_ROOT}/tools/m6502_to_cpp.py ${REPO_ROOT}/tools/codedb.py
  ${REPO_ROOT}/tools/t11dis.py ${REPO_ROOT}/tools/romset.py ${REPO_ROOT}/tools/t11_cycles.txt
  ${REPO_ROOT}/re/coverage/main_cov.txt ${REPO_ROOT}/re/coverage/main_edges.txt
  ${REPO_ROOT}/re/coverage/sound_cov.txt ${REPO_ROOT}/re/coverage/sound_edges.txt
  ${CMAKE_CURRENT_SOURCE_DIR}/src/logic/routines.txt)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${D720_TRANSLATOR_INPUTS})
set(D720_TRANSLATOR_HASH "")
foreach(f ${D720_TRANSLATOR_INPUTS})
  if(EXISTS ${f})
    file(SHA256 ${f} h)
    string(APPEND D720_TRANSLATOR_HASH ${h})
  endif()
endforeach()
string(SHA256 D720_TRANSLATOR_HASH "${D720_TRANSLATOR_HASH}")
set(D720_STAMP ${CMAKE_CURRENT_SOURCE_DIR}/src/game/translator.sha256)
set(D720_OLD_HASH "")
if(EXISTS ${D720_STAMP})
  file(READ ${D720_STAMP} D720_OLD_HASH)
endif()
if(D720_REGENERATE OR NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/game/files.cmake
   OR NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/game/s65_code.cpp
   OR NOT D720_OLD_HASH STREQUAL D720_TRANSLATOR_HASH)
  # a MAME 720.zip dropped into the ROM folder is extracted first
  execute_process(COMMAND ${Python3_EXECUTABLE} -I ${REPO_ROOT}/tools/romzip.py ${D720_ROM_DIR}
    OUTPUT_QUIET ERROR_QUIET)
  if(NOT EXISTS ${D720_ROM_DIR}/136047-3126.7lm)
    message(FATAL_ERROR "720 Degrees ROM files not found in '${D720_ROM_DIR}'. "
      "Copy the rev 4 ROM set (MAME '720', the files or the zip) there or pass -DD720_ROM_DIR=<folder>.")
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
  file(WRITE ${D720_STAMP} "${D720_TRANSLATOR_HASH}")
endif()
add_custom_target(regenerate
  COMMAND ${CMAKE_COMMAND} -E env D720_ROM_DIR=${D720_ROM_DIR} ${Python3_EXECUTABLE} -I tools/t11_to_cpp.py
  COMMAND ${CMAKE_COMMAND} -E env D720_ROM_DIR=${D720_ROM_DIR} ${Python3_EXECUTABLE} -I tools/m6502_to_cpp.py
  WORKING_DIRECTORY ${REPO_ROOT}
  COMMENT "Re-translating the program ROMs (re-run CMake afterwards)")
endif()

# ---------------------------------------------------------------------------
# SDL3.  Windows: an installed package, otherwise the official development package for
# this toolchain is downloaded into third_party/ (SDL3.dll ships next to the programs).
# Linux / macOS: SDL3 is built from source as a static library, so the programs need no
# extra shared library (-DD720_SYSTEM_SDL=ON uses an installed SDL3 instead).
set(SDL3_VERSION 3.2.28)
if(NOT WIN32)
  option(D720_SYSTEM_SDL "Use an installed SDL3 instead of building it from source" OFF)
  if(D720_SYSTEM_SDL)
    find_package(SDL3 REQUIRED CONFIG)
  else()
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
      URL https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VERSION}/SDL3-${SDL3_VERSION}.tar.gz)
    FetchContent_MakeAvailable(SDL3)
    if(NOT TARGET SDL3::SDL3)
      add_library(SDL3::SDL3 ALIAS SDL3-static)
    endif()
  endif()
else()
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
endif()

# ---------------------------------------------------------------------------
# Coroutines for the translated programs: Win32 fibers, or a small assembly context
# switch on Linux / macOS (x86-64 and ARM64; ucontext elsewhere)
if(WIN32)
  set(CORO_SRC src/native/coro_win32.cpp)
else()
  set(CORO_SRC src/native/coro_posix.cpp)
endif()
add_executable(coro_test src/native/coro_test.cpp ${CORO_SRC})
target_include_directories(coro_test PRIVATE src/native)

# ---------------------------------------------------------------------------
# Shared board support (memory map, video, sound chips)
add_library(board STATIC
  src/board/board.cpp
  src/board/soundchips.cpp
  src/board/tms5220.cpp
  src/board/romzip.cpp
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
if(D720_GAME)
include(src/game/files.cmake)
file(GLOB D720_LOGIC_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/logic/*.cpp)
add_library(gamecore STATIC
  src/native/t11rt.cpp
  ${CORO_SRC}
  ${D720_LOGIC_SOURCES}
  src/native/nativemachine.cpp
  src/native/s65rt.cpp
  src/game/s65_code.cpp
  src/game/s65_tables.cpp
  ${T11_GENERATED})
target_include_directories(gamecore PUBLIC src/native src/game src/logic)
target_link_libraries(gamecore PUBLIC board)
if(NOT MSVC)
  set_source_files_properties(${T11_GENERATED} src/game/s65_code.cpp PROPERTIES COMPILE_OPTIONS "-O1;-fno-strict-aliasing")
endif()
endif()

# ---------------------------------------------------------------------------
# Level data library
add_library(leveldata STATIC src/level/leveldata.cpp)
target_include_directories(leveldata PUBLIC src/level)
target_link_libraries(leveldata PUBLIC board)
target_include_directories(leveldata PUBLIC src/native)

# ---------------------------------------------------------------------------
# Dear ImGui (vendored in third_party/imgui): debug menu, controls window, level editor
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

set(D720_OUTPUT_DIR "${REPO_ROOT}" CACHE PATH "Folder the finished game and editor are copied to (the ROMs go in its Game subfolder)")
# copy a program (and on Windows SDL3.dll) to the output folder after building it
function(d720_copy_to_output target)
  if(WIN32)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:${target}>
      COMMAND ${CMAKE_COMMAND} -E make_directory ${D720_OUTPUT_DIR}
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${target}> $<TARGET_FILE:SDL3::SDL3> ${D720_OUTPUT_DIR}
      COMMENT "Copying ${target} to ${D720_OUTPUT_DIR}")
  else()
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory ${D720_OUTPUT_DIR}
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${target}> ${D720_OUTPUT_DIR}
      COMMENT "Copying ${target} to ${D720_OUTPUT_DIR}")
  endif()
endfunction()

if(D720_GAME)
# ---------------------------------------------------------------------------
# The game
# program icon: a resource on Windows (port/res, drawn by tools/make_icon.py)
if(WIN32)
  enable_language(RC)
  set(D720_ICON_RC ${CMAKE_CURRENT_SOURCE_DIR}/res/720degrees.rc)
endif()
add_executable(720degrees WIN32 src/app/main.cpp src/app/input_config.cpp src/app/debugui.cpp ${D720_ICON_RC})
target_link_libraries(720degrees PRIVATE gamecore leveldata imgui SDL3::SDL3)
d720_copy_to_output(720degrees)

# ---------------------------------------------------------------------------
# Level editor

add_executable(editor720 WIN32 src/editor/editor.cpp ${D720_ICON_RC})
target_link_libraries(editor720 PRIVATE gamecore leveldata imgui SDL3::SDL3)
d720_copy_to_output(editor720)

add_executable(verify720 src/tools/verify720.cpp)
target_link_libraries(verify720 PRIVATE gamecore refcpu)
add_executable(render720 src/tools/render720.cpp)
target_link_libraries(render720 PRIVATE gamecore leveldata)
endif()

# ---------------------------------------------------------------------------
# Tools that do not need the translated code
add_executable(ref720 src/ref/refmain.cpp)
target_link_libraries(ref720 PRIVATE refcpu)
add_executable(leveltool src/tools/leveltool.cpp)
target_link_libraries(leveltool PRIVATE leveldata board)

if(MINGW)
  # Link the MinGW runtime (libgcc, libstdc++, winpthread) statically so the programs run
  # outside MSYS2.  SDL3 is still used through its import library and ships as SDL3.dll.
  foreach(t 720degrees editor720 ref720 verify720 render720 leveltool coro_test)
    if(TARGET ${t})
      target_link_options(${t} PRIVATE -static)
    endif()
  endforeach()
endif()
