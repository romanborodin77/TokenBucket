cmake_minimum_required(VERSION 3.30)
project(TokenBucket VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Test)
find_package(unofficial-sodium CONFIG REQUIRED)
qt_standard_project_setup()

qt_add_executable(TokenBucket WIN32 src/main.cpp)
target_link_libraries(TokenBucket PRIVATE Qt6::Widgets unofficial-sodium::sodium)

if(WIN32)
  target_link_libraries(TokenBucket PRIVATE crypt32 user32 wtsapi32 windowsapp)
endif()

enable_testing()
add_subdirectory(tests)
