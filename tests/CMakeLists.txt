find_package(Qt6 REQUIRED COMPONENTS Widgets Test)
# Шаблон всех тестовых целей: линкуют ядро, offscreen-платформа, плагины из vcpkg-install.
function(tb_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE Qt6::Widgets Qt6::Test tokenbucket_core)
  add_test(NAME ${name} COMMAND ${name} -o "${CMAKE_CURRENT_BINARY_DIR}/${name}.txt,txt")
  set_tests_properties(${name} PROPERTIES ENVIRONMENT
    "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins")
endfunction()

tb_add_test(test_smoke)
tb_add_test(test_crypto)
tb_add_test(test_container)

tb_add_test(test_model)

tb_add_test(test_store)

tb_add_test(test_app)

tb_add_test(test_ui)
tb_add_test(test_clipboard)
tb_add_test(test_lock)
tb_add_test(test_ssh)
tb_add_test(test_paste_win)
tb_add_test(test_pwchange)
tb_add_test(test_e2e)
tb_add_test(test_import)
if(WIN32)
  tb_add_test(test_env_win)
  add_test(NAME test_theme_win COMMAND test_ui themeModesApplyAndLoad -o "${CMAKE_CURRENT_BINARY_DIR}/test_theme_win.txt,txt")
  set_tests_properties(test_theme_win PROPERTIES ENVIRONMENT
    "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins")
  set_tests_properties(test_lock PROPERTIES ENVIRONMENT
    "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins")
  set_tests_properties(test_paste_win PROPERTIES ENVIRONMENT
    "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins")
  set_tests_properties(test_e2e PROPERTIES ENVIRONMENT
    "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins")
  set_tests_properties(test_env_win PROPERTIES ENVIRONMENT
    "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins")
endif()

add_test(NAME test_quickpaste_ui COMMAND test_ui quickPanelClosesOnLock quickPanelKeyboardManualMode quickPanelDisambiguatesEntries -o "${CMAKE_CURRENT_BINARY_DIR}/test_quickpaste_ui.txt,txt")
set_tests_properties(test_quickpaste_ui PROPERTIES ENVIRONMENT
  "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins")
