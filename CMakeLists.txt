cmake_minimum_required(VERSION 3.21)
project(matrix-client
    VERSION 0.1.0
    DESCRIPTION "Native C++/Qt Matrix desktop client"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Gui
    Qml
    Quick
    QuickControls2
    Network
    Sql
    Widgets
)

qt_standard_project_setup(REQUIRES 6.5)

set(APP_SOURCES
    src/main.cpp

    src/app/AppController.h
    src/app/AppController.cpp
    src/app/SettingsManager.h
    src/app/SettingsManager.cpp

    src/auth/AuthManager.h
    src/auth/AuthManager.cpp
    src/auth/AccountManager.h
    src/auth/AccountManager.cpp

    src/matrix/MatrixClient.h
    src/matrix/MatrixClient.cpp
    src/matrix/MockMatrixClient.h
    src/matrix/MockMatrixClient.cpp
    src/matrix/CppHttpMatrixClient.h
    src/matrix/CppHttpMatrixClient.cpp
    src/matrix/MediaHelpers.h
    src/matrix/MediaHelpers.cpp
    src/matrix/RoomInfo.h
    src/matrix/TimelineEvent.h

    src/storage/CacheStore.h
    src/storage/CacheStore.cpp

    src/models/RoomListModel.h
    src/models/RoomListModel.cpp
    src/models/TimelineModel.h
    src/models/TimelineModel.cpp
    src/models/MessageComposer.h
    src/models/MessageComposer.cpp

    src/notifications/NotificationManager.h
    src/notifications/NotificationManager.cpp
    src/media/MediaManager.h
    src/media/MediaManager.cpp
    src/crypto/CryptoManager.h
    src/crypto/CryptoManager.cpp
    src/spaces/SpaceManager.h
    src/spaces/SpaceManager.cpp
    src/threads/ThreadManager.h
    src/threads/ThreadManager.cpp
)

set(APP_QML_FILES
    qml/Main.qml
    qml/LoginScreen.qml
    qml/SettingsScreen.qml
    qml/MainScreen.qml
    qml/RoomListPane.qml
    qml/TimelinePane.qml
    qml/RoomDelegate.qml
    qml/MessageDelegate.qml
    qml/MessageComposerBar.qml
    qml/AppTheme.qml
)

# Strip the "qml/" prefix so QML files sit at the module root and can be
# resolved via QQmlApplicationEngine::loadFromModule("MatrixClient", "Main").
foreach(_qml_file IN LISTS APP_QML_FILES)
    get_filename_component(_qml_name ${_qml_file} NAME)
    set_source_files_properties(${_qml_file}
        PROPERTIES QT_RESOURCE_ALIAS ${_qml_name})
endforeach()

# AppTheme is a singleton; mark it so `pragma Singleton` is honored across
# the module import boundary.
set_source_files_properties(qml/AppTheme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(matrix-client ${APP_SOURCES})

qt_add_qml_module(matrix-client
    URI MatrixClient
    VERSION 1.0
    QML_FILES ${APP_QML_FILES}
    RESOURCE_PREFIX /qt/qml
)

target_include_directories(matrix-client PRIVATE src)

target_link_libraries(matrix-client PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Sql
    Qt6::Widgets
)

target_compile_definitions(matrix-client PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    $<$<CONFIG:Release>:QT_NO_DEBUG_OUTPUT>
)

if(MSVC)
    target_compile_options(matrix-client PRIVATE /W4 /permissive-)
else()
    target_compile_options(matrix-client PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

install(TARGETS matrix-client
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION bin
)
