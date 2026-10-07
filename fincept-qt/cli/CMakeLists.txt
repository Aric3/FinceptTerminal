# This target is also buildable independently: no GUI, account services or DB migrations.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.27)
    project(FinceptLocalCli LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Core Network Sql)
endif()
add_executable(fincept-local main.cpp Runtime.cpp PortfolioReader.cpp ../src/network/ProxyManager.cpp)
target_include_directories(fincept-local PRIVATE ../src)
target_compile_definitions(fincept-local PRIVATE FINCEPT_LOCAL_EDITION
    FINCEPT_CLI_SCRIPTS="${CMAKE_CURRENT_SOURCE_DIR}/../scripts")
target_link_libraries(fincept-local PRIVATE Qt6::Core Qt6::Network Qt6::Sql)
set_target_properties(fincept-local PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF UNITY_BUILD OFF)

if(FINCEPT_BUILD_TESTS OR CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        enable_testing()
        add_test(NAME tst_local_cli COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_local_cli.py" $<TARGET_FILE:fincept-local>)
        set_tests_properties(tst_local_cli PROPERTIES TIMEOUT 30)
    endif()
endif()
