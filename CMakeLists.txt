cmake_minimum_required(VERSION 3.20)
project(odds_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

option(ODDS_WERROR "Treat warnings as errors" OFF)
if(ODDS_WERROR)
    add_compile_options(-Werror)
endif()

add_executable(odds_sim sim/main.cpp)
add_executable(verify_session tools/verify_session.cpp)
add_executable(scratch_latent tests/scratch_latent.cpp)
add_executable(scratch_books  tests/scratch_books.cpp)

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
)
FetchContent_MakeAvailable(Catch2)

add_executable(unit_tests
    tests/test_wire_format.cpp
    tests/test_seeding.cpp
    tests/test_latent.cpp
    tests/test_bookmaker.cpp
    tests/test_devig.cpp
)
target_link_libraries(unit_tests PRIVATE Catch2::Catch2WithMain)

enable_testing()
include(CTest)
include(Catch)
catch_discover_tests(unit_tests)
