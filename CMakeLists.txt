cmake_minimum_required(VERSION 3.20)
project(proxy_gateway VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(PG_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(PG_BUILD_TESTS "Build unit tests" ON)

add_compile_options(-Wall -Wextra)
if(PG_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

# Everything except main() lives in this library so tests can link it.
add_library(pg_core STATIC
  src/common/config.cpp
  src/common/http_util.cpp
  src/common/metrics.cpp
  src/health/health_monitor.cpp
  src/health/health_state.cpp
  src/ingress/http_parser.cpp
  src/ingress/http_response.cpp
  src/ingress/listener.cpp
  src/net/http_response_parser.cpp
  src/net/epoll_forwarder.cpp
  src/net/simple_forwarder.cpp
  src/admin/admin_server.cpp
  src/gateway/gateway_core.cpp
  src/routing/policy_router.cpp
  src/scheduler/domain_scheduler.cpp
  src/workers/aimd.cpp
  src/workers/worker_pool.cpp
)
target_include_directories(pg_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(pg_core PUBLIC Threads::Threads)

add_executable(gateway src/main.cpp)
target_link_libraries(gateway PRIVATE pg_core)

if(PG_BUILD_TESTS)
  enable_testing()
  add_library(catch2_amalgamated STATIC third_party/catch2/catch_amalgamated.cpp)
  target_include_directories(catch2_amalgamated PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/catch2)
  # Third-party code: do not fail or warn on its internals.
  target_compile_options(catch2_amalgamated PRIVATE -w)

  file(GLOB PG_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  add_executable(pg_tests ${PG_TEST_SOURCES})
  target_link_libraries(pg_tests PRIVATE pg_core catch2_amalgamated)
  target_compile_definitions(pg_tests PRIVATE PG_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  target_include_directories(pg_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(NAME unit COMMAND pg_tests)
endif()
