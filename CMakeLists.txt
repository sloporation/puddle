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
  src/core/access.c
  src/core/auth.c
  src/core/client.c
  src/core/config.c
  src/core/encoding.c
  src/core/flow.c
  src/core/fsutil.c
  src/core/hash.c
  src/core/http.c
  src/core/identity.c
  src/core/inbound.c
  src/core/json.c
  src/core/keysync.c
  src/core/log.c
  src/core/media.c
  src/core/message.c
  src/core/messages.c
  src/core/phrase.c
  src/core/outbox.c
  src/core/owner_streams.c
  src/core/profile.c
  src/core/profiles_api.c
  src/core/ratelimit.c
  src/core/group.c
  src/core/groupapi.c
  src/core/groupflow.c
  src/core/link.c
  src/core/router.c
  src/core/seal.c
  src/core/routes.c
  src/core/reqsig.c
  src/core/serve.c
  src/core/server.c
  src/core/store.c
  src/core/streams.c
  src/core/tls.c
  src/core/wireprofile.c
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
  find_package(Qt6 COMPONENTS Quick QuickControls2 Network Test QUIET)
  if(Qt6_FOUND)
    qt_standard_project_setup()
    qt_policy(SET QTP0001 NEW)
    qt_policy(SET QTP0004 NEW)
    qt_add_executable(puddle WIN32 MACOSX_BUNDLE
      src/gui/main.cpp
      src/gui/backend.cpp
      src/gui/backend.h
      src/gui/chats.cpp
      src/gui/chats.h
      src/gui/apiclient.cpp
      src/gui/apiclient.h
      src/gui/jsonlistmodel.cpp
      src/gui/jsonlistmodel.h)
    qt_add_qml_module(puddle
      URI Puddle
      VERSION 1.0
      QML_FILES src/gui/Main.qml src/gui/Setup.qml src/gui/Shell.qml src/gui/Avatar.qml src/gui/PersonRow.qml
        src/gui/ChatPage.qml src/gui/GroupsPage.qml src/gui/NewGroupPopup.qml src/gui/GroupInviteRow.qml src/gui/GroupPopup.qml src/gui/FriendsPage.qml src/gui/AddFriendPopup.qml src/gui/LogPage.qml
      RESOURCES src/gui/puddle.png)
    target_link_libraries(puddle PRIVATE puddle_core Qt6::Quick Qt6::QuickControls2 Qt6::Network)
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

    # The first-run state machine, against the real core, with no window (needs the offscreen platform).
    add_executable(backend_test tests/backend_test.cpp src/gui/backend.cpp src/gui/backend.h src/gui/chats.cpp src/gui/chats.h
      src/gui/apiclient.cpp src/gui/apiclient.h src/gui/jsonlistmodel.cpp src/gui/jsonlistmodel.h)
    target_include_directories(backend_test PRIVATE src/gui)
    target_link_libraries(backend_test PRIVATE puddle_core Qt6::Gui Qt6::Network)
    if(APPLE)
      target_sources(backend_test PRIVATE src/gui/dock_mac.mm)
      target_link_libraries(backend_test PRIVATE "-framework AppKit")
    endif()
    add_test(NAME backend COMMAND backend_test)

    # The app's view of a Puddle (owner API over pinned TLS), against two real Puddles talking to each other.
    add_executable(chats_test tests/chats_test.cpp src/gui/chats.cpp src/gui/chats.h src/gui/apiclient.cpp src/gui/apiclient.h
      src/gui/jsonlistmodel.cpp src/gui/jsonlistmodel.h)
    set_target_properties(chats_test PROPERTIES AUTOMOC ON)
    target_link_libraries(chats_test PRIVATE puddle_core Qt6::Core Qt6::Gui Qt6::Network Qt6::Test)
    add_test(NAME chats COMMAND chats_test)

    # The window, driven with real mouse and key events against two real Puddles.
    add_executable(ui_test tests/ui_test.cpp src/gui/backend.cpp src/gui/backend.h src/gui/chats.cpp src/gui/chats.h
      src/gui/apiclient.cpp src/gui/apiclient.h src/gui/jsonlistmodel.cpp src/gui/jsonlistmodel.h)
    set_target_properties(ui_test PROPERTIES AUTOMOC ON)
    target_include_directories(ui_test PRIVATE src/gui)
    target_compile_definitions(ui_test PRIVATE PUDDLE_GUI_DIR="${CMAKE_SOURCE_DIR}/src/gui")
    target_link_libraries(ui_test PRIVATE puddle_core Qt6::Quick Qt6::QuickControls2 Qt6::Network Qt6::Test)
    if(APPLE)
      target_sources(ui_test PRIVATE src/gui/dock_mac.mm)
      target_link_libraries(ui_test PRIVATE "-framework AppKit")
    endif()
    add_test(NAME ui COMMAND ui_test)
    set_tests_properties(ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
    set_tests_properties(chats PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
    set_tests_properties(backend PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  else()
    message(WARNING "Qt6 Quick not found; skipping the Puddle app (set -DPUDDLE_BUILD_GUI=OFF to silence)")
  endif()
endif()

# --- tests --------------------------------------------------------------------
enable_testing()
foreach(t http json store config yaml profile encoding identity auth reqsig phrase serve router routes link group groupapi groupowner streams ratelimit access owner_streams client outbox inbound open_request flow message seal messages keysync wireprofile profiles e2e group_e2e)
  add_executable(${t}_test tests/${t}_test.c)
  target_link_libraries(${t}_test PRIVATE puddle_core)
  add_test(NAME ${t} COMMAND ${t}_test)
  # fill new and freed heap memory with garbage, so reading memory nobody wrote shows up as a failure
  set_tests_properties(${t} PROPERTIES ENVIRONMENT "MallocScribble=1;MALLOC_PERTURB_=165")
endforeach()

install(FILES dist/puddled.service DESTINATION share/puddle)
