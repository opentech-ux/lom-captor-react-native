cmake_minimum_required(VERSION 3.4.1)
project(SessionRecorderReactNative)

set (CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_OBJECT_PATH_MAX 180)

add_library(
    session-recorder-react-native    STATIC
    ../cpp/SessionRecorderReactNativeImpl.cpp
    ../cpp/SessionRecorderTreeScanner.cpp
    ../cpp/SessionRecorderObserver.cpp
    ../cpp/SessionRecorderObserverCapture.cpp
    ../cpp/SessionRecorderObserverScheduling.cpp
)

set_target_properties(
    session-recorder-react-native PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_compile_definitions(session-recorder-react-native PRIVATE RN_SERIALIZABLE_STATE)

target_include_directories(
    session-recorder-react-native    PUBLIC
    ../cpp
)

target_link_libraries(
    session-recorder-react-native    jsi
    reactnative
    react_codegen_SessionRecorderReactNativeSpec
    log
)
