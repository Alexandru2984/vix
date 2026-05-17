cmake_minimum_required(VERSION 3.20)

project(vix_arena VERSION 1.0.0 LANGUAGES CXX)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED COMPONENTS system)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBPQXX REQUIRED IMPORTED_TARGET libpqxx)

include(CTest)

set(ARENA_CORE_SOURCES
  src/GameServer.cpp
  src/Player.cpp
  src/Persistence.cpp
  src/World.cpp
  src/Protocol.cpp
  src/Validation.cpp
  src/Utils.cpp
)

add_library(vix-arena-core STATIC ${ARENA_CORE_SOURCES})
target_include_directories(vix-arena-core PUBLIC src)
target_link_libraries(vix-arena-core PUBLIC Boost::system nlohmann_json::nlohmann_json Threads::Threads PkgConfig::LIBPQXX)

add_executable(vix-arena src/main.cpp)
target_link_libraries(vix-arena PRIVATE vix-arena-core)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(vix-arena-core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(vix-arena PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
  add_executable(vix-arena-tests tests/unit_tests.cpp)
  target_link_libraries(vix-arena-tests PRIVATE vix-arena-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(vix-arena-tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME vix-arena-unit COMMAND vix-arena-tests)
endif()
