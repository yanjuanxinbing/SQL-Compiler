cmake_minimum_required(VERSION 3.10)
project(SQLCompiler CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# 头文件目录
include_directories(${CMAKE_SOURCE_DIR}/include)

# 收集所有源文件
file(GLOB_RECURSE SQLCOMPILER_LIB_SOURCES
    ${CMAKE_SOURCE_DIR}/src/common/*.cpp
    ${CMAKE_SOURCE_DIR}/src/lexer/*.cpp
    ${CMAKE_SOURCE_DIR}/src/ast/*.cpp
    ${CMAKE_SOURCE_DIR}/src/parser/*.cpp
    ${CMAKE_SOURCE_DIR}/src/semantic/*.cpp
    ${CMAKE_SOURCE_DIR}/src/plan/*.cpp
    ${CMAKE_SOURCE_DIR}/src/optimizer/*.cpp
    ${CMAKE_SOURCE_DIR}/src/codegen/*.cpp
)

# 编译为静态库，方便主程序与未来的测试共同链接
add_library(sqlcompiler_lib STATIC ${SQLCOMPILER_LIB_SOURCES})
target_include_directories(sqlcompiler_lib PUBLIC ${CMAKE_SOURCE_DIR}/include)

# 主程序可执行文件
add_executable(sqlcompiler ${CMAKE_SOURCE_DIR}/src/main.cpp)
target_link_libraries(sqlcompiler PRIVATE sqlcompiler_lib)

# 预留测试目录（可在实现完成后接入GoogleTest等框架）
enable_testing()
add_subdirectory(tests)
