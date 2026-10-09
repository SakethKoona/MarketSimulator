cmake_minimum_required(VERSION 3.20)
project(MarketSimulator LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)
find_package(Threads REQUIRED)
enable_testing()

# ---------------------------------------------------------------- engine
set(ENGINE_DIR ${CMAKE_SOURCE_DIR}/engine)
file(GLOB ENGINE_SOURCES ${ENGINE_DIR}/src/*.cpp)
list(REMOVE_ITEM ENGINE_SOURCES ${ENGINE_DIR}/src/main.cpp)
add_library(engine STATIC ${ENGINE_SOURCES})
target_include_directories(engine PUBLIC ${ENGINE_DIR}/include ${ENGINE_DIR}/third_party)
target_link_libraries(engine PUBLIC Threads::Threads)

add_executable(engine_demo ${ENGINE_DIR}/src/main.cpp)
target_link_libraries(engine_demo engine)
set_target_properties(engine_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/engine)

file(GLOB ENGINE_TEST_SOURCES ${ENGINE_DIR}/tests/*.cpp)
add_executable(engine_tests ${ENGINE_TEST_SOURCES})
target_compile_options(engine_tests PRIVATE -UNDEBUG)
target_include_directories(engine_tests PRIVATE ${ENGINE_DIR}/tests)
target_link_libraries(engine_tests engine)
set_target_properties(engine_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/engine)
add_test(NAME engine_tests COMMAND engine_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(engine_bench ${ENGINE_DIR}/bench/bench_main.cpp)
target_link_libraries(engine_bench engine)
set_target_properties(engine_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/engine)

# The mktsim CLI uses mach-o/dyld.h for its self-path lookup; macOS only
# until it grows a Linux path.
if(APPLE)
  file(GLOB CLI_SOURCES ${ENGINE_DIR}/cli/*.cpp)
  add_executable(mktsim ${CLI_SOURCES})
  target_include_directories(mktsim PRIVATE ${ENGINE_DIR}/cli ${CMAKE_SOURCE_DIR}/gateway/include)
  target_link_libraries(mktsim engine)
  set_target_properties(mktsim PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/engine)
endif()

# ---------------------------------------------------------------- gateway
set(GW_DIR ${CMAKE_SOURCE_DIR}/gateway)
add_library(gateway STATIC
  ${GW_DIR}/src/feed_publisher.cpp
  ${GW_DIR}/src/boe_server.cpp
  ${GW_DIR}/src/ingress/core.cpp
  ${GW_DIR}/src/ingress/plugin.cpp)
target_include_directories(gateway PUBLIC ${GW_DIR}/include)
target_link_libraries(gateway PUBLIC engine ${CMAKE_DL_LIBS})

foreach(bin exchange_server)
  add_executable(${bin} ${GW_DIR}/src/${bin}.cpp)
  target_link_libraries(${bin} gateway)
  set_target_properties(${bin} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/gateway)
endforeach()
foreach(tool feed_dump flowgen)
  add_executable(${tool} ${GW_DIR}/tools/${tool}.cpp)
  target_link_libraries(${tool} gateway)
  set_target_properties(${tool} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/gateway)
endforeach()

# Ingress plugins: shared libraries built against ingress/api.h only.
add_library(jsonl_ingress MODULE ${GW_DIR}/plugins/jsonl_ingress.cpp)
target_include_directories(jsonl_ingress PRIVATE ${GW_DIR}/include)
target_link_libraries(jsonl_ingress Threads::Threads)
set_target_properties(jsonl_ingress PROPERTIES
  PREFIX "" LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/gateway)
if(APPLE)
  set_target_properties(jsonl_ingress PROPERTIES SUFFIX ".dylib")
endif()

# Gateway tests. They check with assert(), so keep assertions on even in
# Release (CMake adds -DNDEBUG; -UNDEBUG comes later on the command line).
foreach(t protocol_test spsc_ring_test mpmc_ring_test)
  add_executable(${t} ${GW_DIR}/tests/${t}.cpp)
  target_compile_options(${t} PRIVATE -UNDEBUG)
  target_link_libraries(${t} gateway)
  set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/gateway)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
add_executable(feed_reconstruct_test ${GW_DIR}/tests/feed_reconstruct_test.cpp)
target_compile_options(feed_reconstruct_test PRIVATE -UNDEBUG)
target_link_libraries(feed_reconstruct_test gateway)
set_target_properties(feed_reconstruct_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/gateway)
add_test(NAME feed_reconstruct_1shard COMMAND feed_reconstruct_test 200000 1)
add_test(NAME feed_reconstruct_3shard COMMAND feed_reconstruct_test 100000 3)
