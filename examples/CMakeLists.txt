
if (APPLE)
    add_subdirectory(macos/demo)
endif (APPLE)

if (WIN32)
    # 自动查找 Qt5 或 Qt6
    find_package(QT NAMES Qt6 Qt5 COMPONENTS Widgets)

    add_subdirectory(windows/demo)
    if (QT_FOUND)
        message(STATUS "Found Qt version ${QT_VERSION_MAJOR}")
        add_subdirectory(windows/qt-window-app)
    endif ()
endif (WIN32)

if (LINUX)
    add_subdirectory(linux/demo)
    add_subdirectory(linux/crash_loop_detection)
endif (LINUX)
