cmake_minimum_required(VERSION 3.20)
project(mchud LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
# Статический рантайм: DLL не требует vcredist внутри процесса javaw.exe
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# JDK нужен только при сборке: заголовки jni.h/jvmti.h и javac для Java-части
if(NOT DEFINED ENV{JAVA_HOME})
    message(FATAL_ERROR "JAVA_HOME is not set (install a JDK)")
endif()
file(TO_CMAKE_PATH "$ENV{JAVA_HOME}" JDK_HOME)
find_program(JAVAC_EXE NAMES javac HINTS "${JDK_HOME}/bin" REQUIRED)

set(PAYLOAD_HDR ${CMAKE_CURRENT_BINARY_DIR}/payload_data.h)
file(GLOB_RECURSE PAYLOAD_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/java/*.java)

add_custom_command(
    OUTPUT ${PAYLOAD_HDR}
    COMMAND ${CMAKE_COMMAND}
        -DJAVAC=${JAVAC_EXE}
        -DSRC_DIR=${CMAKE_SOURCE_DIR}/java
        -DOUT_HDR=${PAYLOAD_HDR}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/payload_classes
        -P ${CMAKE_SOURCE_DIR}/cmake/embed_payload.cmake
    DEPENDS ${PAYLOAD_SRC} ${CMAKE_SOURCE_DIR}/cmake/embed_payload.cmake
    VERBATIM)

add_library(mchud SHARED src/dllmain.cpp ${PAYLOAD_HDR})
target_include_directories(mchud PRIVATE
    ${JDK_HOME}/include
    ${JDK_HOME}/include/win32
    ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(mchud PRIVATE user32 kernel32)
