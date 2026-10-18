find_package(Qt6 REQUIRED COMPONENTS Widgets Test)
# Шаблон всех тестовых целей: линкуют ядро, offscreen-платформа, плагины из vcpkg-install.
function(tb_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE Qt6::Widgets Qt6::Test tokenbucket_core)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT
    "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins")
endfunction()

tb_add_test(test_smoke)
tb_add_test(test_crypto)
