# Компилирует java/*.java и генерирует C++-заголовок с байтами .class-файлов.
# Параметры: -DJAVAC=... -DSRC_DIR=... -DOUT_HDR=... -DWORK_DIR=...

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

file(GLOB_RECURSE JAVA_FILES "${SRC_DIR}/*.java")
execute_process(
    COMMAND "${JAVAC}" --release 17 -encoding UTF-8 -d "${WORK_DIR}" ${JAVA_FILES}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "javac failed (${rc}):\n${out}\n${err}")
endif()

file(GLOB_RECURSE CLASS_FILES RELATIVE "${WORK_DIR}" "${WORK_DIR}/*.class")
list(SORT CLASS_FILES)

set(HDR "#pragma once\n#include <cstddef>\nstruct EmbeddedClass { const char* name; const unsigned char* data; size_t size; };\n")
set(TABLE "")
set(i 0)
foreach(rel ${CLASS_FILES})
    file(READ "${WORK_DIR}/${rel}" hex HEX)
    string(LENGTH "${hex}" hexlen)
    math(EXPR size "${hexlen} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(REGEX REPLACE "\\.class$" "" cname "${rel}")
    string(APPEND HDR "static const unsigned char kClassData${i}[] = {${bytes}};\n")
    string(APPEND TABLE "    { \"${cname}\", kClassData${i}, ${size} },\n")
    math(EXPR i "${i} + 1")
endforeach()
string(APPEND HDR "static const EmbeddedClass kClasses[] = {\n${TABLE}};\nstatic const size_t kClassCount = ${i};\n")

file(WRITE "${OUT_HDR}" "${HDR}")
message(STATUS "Embedded ${i} class file(s) into ${OUT_HDR}")
