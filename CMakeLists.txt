cmake_minimum_required(VERSION 3.20)

project(kwinshot VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets DBus)

add_executable(kwinshot
    src/main.cpp
)

target_link_libraries(kwinshot
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::DBus
)

install(TARGETS kwinshot RUNTIME DESTINATION bin)
install(FILES data/net.local.kwinshot.desktop DESTINATION share/applications)
