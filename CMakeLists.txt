cmake_minimum_required(VERSION 3.10)
project(SQLCompiler CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# 头文件目录
include_directories(${CMAKE_SOURCE_DIR}/include)

# 收集所有源文件
file(GLOB_RECURSE SQLCOMPILER_LIB_SOURCES
    # ---- 编译器模块（词法/语法/语义/计划/优化/代码生成） ----
    ${CMAKE_SOURCE_DIR}/src/common/*.cpp
    ${CMAKE_SOURCE_DIR}/src/lexer/*.cpp
    ${CMAKE_SOURCE_DIR}/src/ast/*.cpp
    ${CMAKE_SOURCE_DIR}/src/parser/*.cpp
    ${CMAKE_SOURCE_DIR}/src/semantic/*.cpp
    ${CMAKE_SOURCE_DIR}/src/plan/*.cpp
    ${CMAKE_SOURCE_DIR}/src/optimizer/*.cpp
    ${CMAKE_SOURCE_DIR}/src/codegen/*.cpp

    # ---- 存储子系统（页式存储 + 缓存管理） ----
    ${CMAKE_SOURCE_DIR}/src/storage/*.cpp

    # ---- 存储引擎（记录/元组 <-> 页的映射） ----
    ${CMAKE_SOURCE_DIR}/src/storage_engine/*.cpp

    # ---- 系统目录（元数据持久化） ----
    ${CMAKE_SOURCE_DIR}/src/catalog/*.cpp

    # ---- 执行引擎（算子实现） ----
    ${CMAKE_SOURCE_DIR}/src/execution/*.cpp

    # ---- 数据库总入口（门面类） ----
    ${CMAKE_SOURCE_DIR}/src/db/*.cpp
)

# 编译为静态库，方便主程序与未来的测试共同链接
add_library(sqlcompiler_lib STATIC ${SQLCOMPILER_LIB_SOURCES})
target_include_directories(sqlcompiler_lib PUBLIC ${CMAKE_SOURCE_DIR}/include)

# 主程序可执行文件
add_executable(sqlcompiler ${CMAKE_SOURCE_DIR}/src/main.cpp)
target_link_libraries(sqlcompiler PRIVATE sqlcompiler_lib)
