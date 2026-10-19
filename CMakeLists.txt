cmake_minimum_required(VERSION 3.30)
project(TokenBucket VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Test Concurrent)
find_package(unofficial-sodium CONFIG REQUIRED)
qt_standard_project_setup()

add_library(tokenbucket_core STATIC
  src/vault/Crypto.cpp
  src/vault/Container.cpp
  src/model/VaultModel.cpp
  src/vault/VaultStore.cpp
  src/app/Application.cpp
  src/ui/SetupDialog.cpp
  src/ui/UnlockDialog.cpp
  src/ui/EntryEditDialog.cpp
  src/ui/MainWindow.cpp
  src/ui/MiniPanel.cpp
  src/platform/ClipboardGuard.cpp
  src/ui/EntryCard.cpp
  src/ui/ExportDialog.cpp
  src/ui/VerifyDialog.cpp
  src/app/IdleTimer.cpp
  src/platform/SessionWatcher.h
)
target_include_directories(tokenbucket_core PUBLIC src)
target_link_libraries(tokenbucket_core PUBLIC Qt6::Widgets Qt6::Concurrent unofficial-sodium::sodium)

qt_add_executable(TokenBucket WIN32 src/main.cpp)
target_link_libraries(TokenBucket PRIVATE tokenbucket_core)

if(WIN32)
  target_sources(tokenbucket_core PRIVATE src/platform/fileops_win.cpp)
  target_sources(tokenbucket_core PRIVATE src/platform/hello_win.cpp)
  target_sources(tokenbucket_core PRIVATE src/platform/sessionlock_win.cpp)
  target_sources(tokenbucket_core PRIVATE src/platform/quickpaste_win.cpp src/platform/QuickPasteWin.h)
  target_link_libraries(tokenbucket_core PUBLIC wtsapi32)
  target_link_libraries(tokenbucket_core PUBLIC windowsapp user32)
  target_link_libraries(tokenbucket_core PUBLIC advapi32)
  target_link_libraries(tokenbucket_core PUBLIC crypt32)
  target_link_libraries(TokenBucket PRIVATE crypt32 user32 wtsapi32 windowsapp)
endif()
if(NOT WIN32)
  find_package(Qt6 REQUIRED COMPONENTS DBus)
  target_link_libraries(tokenbucket_core PUBLIC Qt6::DBus)
  target_sources(tokenbucket_core PRIVATE src/platform/sessionlock_linux.cpp)
  target_sources(tokenbucket_core PRIVATE src/platform/fileops_posix.cpp)
endif()

enable_testing()
add_subdirectory(tests)
