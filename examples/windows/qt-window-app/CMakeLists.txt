cmake_minimum_required(VERSION 3.17)

# project(qt_window_app VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory(crashpad)
endif ()

# 自动查找 Qt5 或 Qt6
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)

# 使用主版本号来查找完整的 Qt 模块
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS 
    Widgets
)

set(PROJECT_SOURCES
    backtrace.cpp
    backtrace.hpp
    backtrace.qrc
    main.cpp
    mainwindow.cpp
    mainwindow.hpp
    mainwindow.ui
)

add_executable(qt_window_app ${PROJECT_SOURCES})

target_link_libraries(qt_window_app PRIVATE Qt::Gui Qt::Core Qt::Widgets client)
target_compile_features(qt_window_app PRIVATE cxx_std_17)

if (WIN32)
    set(APP_ICON_RESOURCE_WINDOWS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    set_target_properties(qt_window_app PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(qt_window_app PRIVATE ${APP_ICON_RESOURCE_WINDOWS})
endif ()
