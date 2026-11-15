cmake_minimum_required(VERSION 3.20)
project(afz-cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# libclang: discovery parses translation units with it. llvm-config names the
# toolchain directory; the glob covers distributions that ship the config
# script separately from the library.
find_program(LLVM_CONFIG NAMES llvm-config llvm-config-21 llvm-config-19 llvm-config-18)
set(AFZ_LLVM_LIBDIR "")
set(AFZ_LLVM_INCLUDEDIR "")
set(AFZ_LLVM_BINDIR "")
if(LLVM_CONFIG)
  execute_process(COMMAND ${LLVM_CONFIG} --libdir OUTPUT_VARIABLE AFZ_LLVM_LIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${LLVM_CONFIG} --includedir OUTPUT_VARIABLE AFZ_LLVM_INCLUDEDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${LLVM_CONFIG} --bindir OUTPUT_VARIABLE AFZ_LLVM_BINDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
file(GLOB AFZ_LLVM_LIB_CANDIDATES /usr/lib/llvm-*/lib /usr/lib64/llvm*/lib /opt/homebrew/opt/llvm/lib)
file(GLOB AFZ_LLVM_INC_CANDIDATES /usr/lib/llvm-*/include /opt/homebrew/opt/llvm/include)

find_library(CLANG_LIB NAMES clang libclang.so libclang.dylib PATHS ${AFZ_LLVM_LIBDIR} ${AFZ_LLVM_LIB_CANDIDATES} REQUIRED)
find_path(CLANG_INC NAMES clang-c/Index.h PATHS ${AFZ_LLVM_INCLUDEDIR} ${AFZ_LLVM_INC_CANDIDATES} REQUIRED)

add_library(afz_cpp STATIC
  src/wire.cpp
  src/options.cpp
  src/sha256.cpp
  src/lower.cpp
  src/discover.cpp
  src/driver.cpp
  src/coverage.cpp
)
target_include_directories(afz_cpp PUBLIC src third_party ${CLANG_INC})
target_compile_definitions(afz_cpp PUBLIC AFZ_CPP_VERSION="${PROJECT_VERSION}")
target_link_libraries(afz_cpp PUBLIC ${CLANG_LIB} ${CMAKE_DL_LIBS} pthread)

add_executable(afz-cpp src/main.cpp)
target_link_libraries(afz-cpp PRIVATE afz_cpp)

add_executable(afz-cpp-emit src/bin/emit.cpp)
target_link_libraries(afz-cpp-emit PRIVATE afz_cpp)

add_executable(afz-cpp-check src/bin/check.cpp)
target_link_libraries(afz-cpp-check PRIVATE afz_cpp)

enable_testing()
file(GLOB AFZ_TEST_SOURCES tests/*.cpp)
foreach(test_src ${AFZ_TEST_SOURCES})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_link_libraries(${test_name} PRIVATE afz_cpp)
  target_compile_definitions(${test_name} PRIVATE AFZ_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
