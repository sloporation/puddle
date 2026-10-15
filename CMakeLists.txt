cmake_minimum_required(VERSION 3.21)
project(puddle VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Homebrew's OpenSSL is keg-only on macOS; the system one is LibreSSL.
if(APPLE AND NOT OPENSSL_ROOT_DIR AND EXISTS /opt/homebrew/opt/openssl@3)
  set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
endif()

# Release builds link OpenSSL statically so the binaries don't depend on the
# host's libssl version.
option(PUDDLE_STATIC_OPENSSL "Link OpenSSL statically" OFF)
if(PUDDLE_STATIC_OPENSSL)
  set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()

# libyaml, for the config and profile files (pkg-config isn't assumed to exist).
find_path(YAML_INCLUDE_DIR yaml.h)
if(PUDDLE_STATIC_OPENSSL)
  find_library(YAML_LIBRARY NAMES libyaml.a yaml)
else()
  find_library(YAML_LIBRARY NAMES yaml)
endif()
if(NOT YAML_INCLUDE_DIR OR NOT YAML_LIBRARY)
  message(FATAL_ERROR "libyaml not found (brew install libyaml / apt install libyaml-dev)")
endif()

find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra)

# --- core: config, TLS, HTTP, listener (plain C, shared by both targets) -----
add_library(puddle_core STATIC
  src/core/auth.c
  src/core/config.c
  src/core/encoding.c
  src/core/fsutil.c
  src/core/hash.c
  src/core/http.c
  src/core/identity.c
  src/core/json.c
  src/core/log.c
  src/core/phrase.c
  src/core/profile.c
  src/core/reqsig.c
  src/core/server.c
  src/core/store.c
  src/core/tls.c
  src/core/yamltree.c)
target_include_directories(puddle_core PUBLIC src/core)
# Only yamltree.c sees libyaml's headers: a shared include dir like /opt/homebrew/include would
# also expose a different OpenSSL's headers.
set_source_files_properties(src/core/yamltree.c PROPERTIES INCLUDE_DIRECTORIES "${YAML_INCLUDE_DIR}")
target_link_libraries(puddle_core PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${YAML_LIBRARY})

# --- puddled: headless service (systemd --user, launchd, ...) -----------------
add_executable(puddled src/service/main.c)
target_link_libraries(puddled PRIVATE puddle_core)
install(TARGETS puddled RUNTIME DESTINATION bin)

# --- Puddle: the desktop app (Qt). Embeds the server, so nothing else is needed.
option(PUDDLE_BUILD_GUI "Build the Qt desktop app" ON)
if(PUDDLE_BUILD_GUI)
  find_package(Qt6 COMPONENTS Quick QuickControls2 QUIET)
  if(Qt6_FOUND)
    qt_standard_project_setup()
    qt_policy(SET QTP0001 NEW)
    qt_policy(SET QTP0004 NEW)
    qt_add_executable(puddle WIN32 MACOSX_BUNDLE
      src/gui/main.cpp
      src/gui/backend.cpp
      src/gui/backend.h)
    qt_add_qml_module(puddle
      URI Puddle
      VERSION 1.0
      QML_FILES src/gui/Main.qml
      RESOURCES src/gui/puddle.png)
    target_link_libraries(puddle PRIVATE puddle_core Qt6::Quick Qt6::QuickControls2)
    if(APPLE)
      enable_language(OBJCXX)
      target_sources(puddle PRIVATE src/gui/dock_mac.mm)
      target_link_libraries(puddle PRIVATE "-framework AppKit")
    endif()
    set_target_properties(puddle PROPERTIES
      OUTPUT_NAME Puddle
      MACOSX_BUNDLE_BUNDLE_NAME Puddle
      MACOSX_BUNDLE_GUI_IDENTIFIER io.github.sloporation.puddle
      MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
    install(TARGETS puddle BUNDLE DESTINATION . RUNTIME DESTINATION bin)
  else()
    message(WARNING "Qt6 Quick not found; skipping the Puddle app (set -DPUDDLE_BUILD_GUI=OFF to silence)")
  endif()
endif()

# --- tests --------------------------------------------------------------------
enable_testing()
foreach(t http json store config yaml profile encoding identity auth reqsig phrase)
  add_executable(${t}_test tests/${t}_test.c)
  target_link_libraries(${t}_test PRIVATE puddle_core)
  add_test(NAME ${t} COMMAND ${t}_test)
endforeach()

install(FILES dist/puddled.service DESTINATION share/puddle)
