cmake_minimum_required(VERSION 3.27)
project(hyprmac VERSION 0.1.0)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB_RECURSE SRC "main.cpp" "src/**/*.cpp")
add_library(hyprmac SHARED ${SRC})

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland libdrm libinput libudev pangocairo pixman-1 wayland-server xkbcommon
)
target_link_libraries(hyprmac PRIVATE rt PkgConfig::deps)
target_include_directories(hyprmac PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(hyprmac PROPERTIES PREFIX "lib")

install(TARGETS hyprmac)
