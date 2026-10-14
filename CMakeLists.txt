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

find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra)

# --- core: config, TLS, HTTP, listener (plain C, shared by both targets) -----
add_library(puddle_core STATIC
  src/core/config.c
  src/core/http.c
  src/core/log.c
  src/core/server.c
  src/core/tls.c)
target_include_directories(puddle_core PUBLIC src/core)
target_link_libraries(puddle_core PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

# --- puddled: headless service (systemd --user, launchd, ...) -----------------
add_executable(puddled src/service/main.c)
target_link_libraries(puddled PRIVATE puddle_core)
install(TARGETS puddled RUNTIME DESTINATION bin)

# --- puddle-gui: Qt desktop app ----------------------------------------------
option(PUDDLE_BUILD_GUI "Build the Qt desktop app" ON)
if(PUDDLE_BUILD_GUI)
  find_package(Qt6 COMPONENTS Widgets QUIET)
  if(Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    add_executable(puddle-gui WIN32 MACOSX_BUNDLE src/gui/main.cpp)
    target_link_libraries(puddle-gui PRIVATE puddle_core Qt6::Widgets)
    install(TARGETS puddle-gui BUNDLE DESTINATION . RUNTIME DESTINATION bin)
  else()
    message(WARNING "Qt6 Widgets not found; skipping puddle-gui (set -DPUDDLE_BUILD_GUI=OFF to silence)")
  endif()
endif()

# --- tests --------------------------------------------------------------------
enable_testing()
add_executable(http_test tests/http_test.c)
target_link_libraries(http_test PRIVATE puddle_core)
add_test(NAME http COMMAND http_test)

install(FILES dist/puddled.service DESTINATION share/puddle)
