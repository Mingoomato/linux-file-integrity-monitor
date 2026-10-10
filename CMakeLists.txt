cmake_minimum_required(VERSION 3.16)
project(linux_file_integrity_monitor VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(SQLite3 REQUIRED)

add_executable(fimctl src/fimctl.c)
target_compile_options(fimctl PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(fimctl PRIVATE OpenSSL::Crypto SQLite3::SQLite3)
install(TARGETS fimctl RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME fim-smoke COMMAND ${CMAKE_COMMAND} -E env BIN_DIR=$<TARGET_FILE_DIR:fimctl> ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.sh)
endif()
