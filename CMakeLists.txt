cmake_minimum_required(VERSION 3.30)
project(TokenBucket VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Исходники хранятся в UTF-8 без BOM: без этого флага MSVC читает их в системной
# ANSI-кодировке, и русские литералы зависят от настроек локали машины сборки.
if(MSVC)
  add_compile_options(/utf-8)
endif()
find_package(Qt6 REQUIRED COMPONENTS Widgets Test Concurrent Network)
find_package(unofficial-sodium CONFIG REQUIRED)
qt_standard_project_setup()

# Счётчик версий и ресурсы EXE из единого источника — project VERSION.
configure_file(src/app/Version.h.in Version.h @ONLY)
if(WIN32)
  # Иконка EXE (assets/icon.ico) перегенерируется утилитой brandtool + tools/make-icon.cmd.
  set(TB_ICON "${CMAKE_SOURCE_DIR}/assets/icon.ico")
  configure_file(cmake/TokenBucket.rc.in TokenBucket.rc @ONLY)
endif()

add_library(tokenbucket_core STATIC
  src/app/Preferences.cpp
  src/vault/Crypto.cpp
  src/vault/CborWriter.cpp
  src/vault/Container.cpp
  src/model/VaultModel.cpp
  src/model/TextFile.cpp
  src/vault/VaultStore.cpp
  src/app/Application.cpp
  src/ui/SetupDialog.cpp
  src/ui/UnlockDialog.cpp
  src/ui/EntryEditDialog.cpp
  src/ui/MainWindow.cpp
  src/ui/SettingsDialog.cpp
  src/ui/SessionFlow.cpp
  src/ui/MiniPanel.cpp
  src/platform/ClipboardGuard.cpp
  src/ui/EntryCard.cpp
  src/ui/EntryAccess.cpp
  src/ui/ExportDialog.cpp
  src/ui/VerifyDialog.cpp
  src/app/IdleTimer.cpp
  src/platform/SessionWatcher.h
  src/ui/TrayController.h
  src/ui/TrayController.cpp
  src/ui/Theme.h
  src/ui/Theme.cpp
  src/ui/Brand.h
  src/ui/Brand.cpp
  src/ui/AboutDialog.h
  src/ui/AboutDialog.cpp
  src/io/Import.cpp
)
target_include_directories(tokenbucket_core PUBLIC src $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)
target_link_libraries(tokenbucket_core PUBLIC Qt6::Widgets Qt6::Concurrent Qt6::Network unofficial-sodium::sodium)

qt_add_executable(TokenBucket WIN32 src/main.cpp)
target_link_libraries(TokenBucket PRIVATE tokenbucket_core)
if(WIN32)
  target_sources(TokenBucket PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/TokenBucket.rc)
  add_executable(brandtool tools/brandtool.cpp)
  target_link_libraries(brandtool PRIVATE tokenbucket_core)
endif()

if(WIN32)
  target_sources(tokenbucket_core PRIVATE src/platform/fileops_win.cpp)
  target_sources(tokenbucket_core PRIVATE src/platform/hello_win.cpp)
  target_sources(tokenbucket_core PRIVATE src/platform/sessionlock_win.cpp)
  target_sources(tokenbucket_core PRIVATE src/platform/quickpaste_win.cpp src/platform/QuickPasteWin.h)
  target_sources(tokenbucket_core PRIVATE src/platform/envops_win.cpp src/platform/EnvOps.h)
  target_sources(tokenbucket_core PRIVATE src/platform/crashpolicy_win.cpp src/platform/CrashPolicy.h)
  target_sources(tokenbucket_core PRIVATE src/ui/EnvExportDialog.cpp src/ui/EnvExportDialog.h)
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
