cmake_minimum_required(VERSION 3.16)
project(IntelligentProxyGateway VERSION 1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)
add_compile_options(-Wall -Wextra -O3)
include_directories(${CMAKE_SOURCE_DIR}/include)

# 1. Core Intelligent Proxy Gateway
add_executable(gateway src/main.cpp)
target_link_libraries(gateway Threads::Threads)

# 2. Controlled proxy node (HTTP forward + CONNECT + SOCKS5, delay/failure injection)
add_executable(proxy_node src/proxy_node.cpp)
target_link_libraries(proxy_node Threads::Threads)

# 3. Deterministic target (origin) server
add_executable(target_server src/target_server.cpp)
target_link_libraries(target_server Threads::Threads)

# 4. Closed-loop load generator
add_executable(loadgen src/loadgen.cpp)
target_link_libraries(loadgen Threads::Threads)

# 5. Unit tests
enable_testing()
add_executable(unit_tests tests/unit_tests.cpp)
target_link_libraries(unit_tests Threads::Threads)
add_test(NAME GatewayUnitTests COMMAND unit_tests)

# Mutation fuzzer for all parsers (quick in ctest; run it with millions of iterations and sanitizers for real fuzzing)
add_executable(fuzz_parsers tests/fuzz_parsers.cpp)
target_link_libraries(fuzz_parsers Threads::Threads)
add_test(NAME ParserFuzzQuick COMMAND fuzz_parsers 20000 1)

# 6. End-to-end tests (need python3)
find_program(PYTHON3 python3)
if(PYTHON3)
  add_test(NAME GatewayE2E COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/scripts/e2e_test.py --build ${CMAKE_BINARY_DIR})
  set_tests_properties(GatewayE2E PROPERTIES TIMEOUT 300)
endif()
