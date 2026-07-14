cmake_minimum_required(VERSION 3.16)
project(bulutklinik_sdk VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(bulutklinik_sdk src/bulutklinik.cpp src/cpr_backend.cpp)
add_library(bulutklinik::sdk ALIAS bulutklinik_sdk)
target_include_directories(bulutklinik_sdk PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(bulutklinik_sdk PUBLIC cpr::cpr nlohmann_json::nlohmann_json)
target_compile_options(bulutklinik_sdk PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra>)

option(BULUTKLINIK_BUILD_TESTS "Build the test suite" ON)
if(BULUTKLINIK_BUILD_TESTS)
    enable_testing()
    find_package(Catch2 3 CONFIG REQUIRED)
    add_executable(bulutklinik_tests tests/test_transport.cpp tests/test_ai.cpp)
    target_link_libraries(bulutklinik_tests PRIVATE
        bulutklinik_sdk Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
    add_test(NAME bulutklinik_tests COMMAND bulutklinik_tests)
endif()

include(GNUInstallDirs)
install(TARGETS bulutklinik_sdk EXPORT bulutklinik-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT bulutklinik-targets
    NAMESPACE bulutklinik::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bulutklinik)
