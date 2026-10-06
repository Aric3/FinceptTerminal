# ── Fincept Terminal unit tests (Qt Test) ────────────────────────────────────
#
# Reached ONLY from the top-level CMakeLists.txt under `if(FINCEPT_BUILD_TESTS)`,
# which is OFF by default. Nothing in this file affects a normal build.
#
#   cmake -B build -DFINCEPT_BUILD_TESTS=ON ...
#   cmake --build build
#   ctest --test-dir build --output-on-failure          # add -C Release on MSVC
#
# ─────────────────────────────────────────────────────────────────────────────
# HARD RULE — read this before adding a target.
#
#   A test target lists the SPECIFIC sources it needs, explicitly, by path.
#
# Do NOT "solve" a missing symbol by refactoring the app into an OBJECT library
# and linking it here. The release/CI presets build with CMAKE_UNITY_BUILD=ON
# (batch size 20) while `win-dev` builds unity OFF, so any change to the main
# source list re-batches every file after it and can produce file-scope symbol
# redefinitions that appear ONLY on CI and never reproduce locally. See the
# "unity build" notes in ../CLAUDE.md.
#
# If a unit cannot be tested without dragging in half the application, that is a
# signal about the unit, not about this file: extract the pure logic into a small
# header (or a leaf .cpp) and test that. Do not grow the link line.
#
# Current cost of the whole suite: 4 executables, 7 translation units.
# ─────────────────────────────────────────────────────────────────────────────

# Qt Test ships with every Qt installation, so this adds no new dependency.
# _FINCEPT_QT_VER_ARGS is inherited from the parent scope so the version pin
# (6.8.3 EXACT by default) stays identical to the one the app is built against.
find_package(Qt6 ${_FINCEPT_QT_VER_ARGS} QUIET COMPONENTS Test)
if(NOT Qt6Test_FOUND)
    message(FATAL_ERROR
        "FINCEPT_BUILD_TESTS=ON but the Qt6 Test module was not found.\n"
        "Qt Test ships with the standard Qt installer — re-run the Qt maintenance "
        "tool and make sure 'Qt Test' is selected for your kit, or configure with "
        "-DFINCEPT_BUILD_TESTS=OFF.")
endif()

# Windows has no rpath: a freshly built tst_*.exe cannot find Qt6Core.dll unless
# the Qt bin dir is on PATH. Resolve it once and inject it per-test below, so
# `ctest` works straight out of a fresh configure without anyone having to run
# windeployqt against the test binaries.
#
# Best-effort by design: if the Qt layout is unusual (distro split packages, no
# qmake shipped) we simply skip the injection rather than fail the configure —
# on those layouts Qt is on the loader path already.
set(_fincept_qt_bindir "")
set(_fincept_qt_libdir "")
if(TARGET Qt6::qmake)
    get_target_property(_fincept_qt_qmake Qt6::qmake IMPORTED_LOCATION)
    if(_fincept_qt_qmake AND EXISTS "${_fincept_qt_qmake}")
        get_filename_component(_fincept_qt_bindir "${_fincept_qt_qmake}" DIRECTORY)
        get_filename_component(_fincept_qt_prefix "${_fincept_qt_bindir}" DIRECTORY)
        set(_fincept_qt_libdir "${_fincept_qt_prefix}/lib")
    endif()
endif()
if(NOT _fincept_qt_bindir AND Qt6_DIR)
    # Qt6_DIR is <prefix>/lib/cmake/Qt6 for a standard installer layout.
    get_filename_component(_fincept_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
    if(EXISTS "${_fincept_qt_prefix}/bin")
        set(_fincept_qt_bindir "${_fincept_qt_prefix}/bin")
        set(_fincept_qt_libdir "${_fincept_qt_prefix}/lib")
    endif()
endif()

# fincept_add_test(<name> <source> [<extra sources>...])
#
# <name> must be tst_<unit>. Extra sources are the app .cpp files this unit
# genuinely needs — keep that list as short as it can possibly be.
function(fincept_add_test _name)
    add_executable(${_name} ${ARGN})

    # Qt6::Test pulls in Qt6::Core. Deliberately no Widgets/Network/Sql here:
    # a unit test that needs those is testing the wrong seam.
    target_link_libraries(${_name} PRIVATE Qt6::Test Qt6::Core)

    # Sources use project-root-relative includes ("trading/OrderValidator.h"),
    # matching target_include_directories(FinceptTerminal PRIVATE src).
    target_include_directories(${_name} PRIVATE "${PROJECT_SOURCE_DIR}/src")

    set_target_properties(${_name} PROPERTIES
        AUTOMOC ON            # Q_OBJECT test classes live in the .cpp (+ #include "<name>.moc")
        UNITY_BUILD OFF       # never batch test TUs, whatever the parent preset says
        FOLDER "tests")

    # NOTE: no install() rule, by design. Test executables must never ship in a
    # release artifact. They also land in ${CMAKE_CURRENT_BINARY_DIR} (build/tests/),
    # not next to FinceptTerminal.exe, so the installer's
    # install(DIRECTORY "$<TARGET_FILE_DIR:FinceptTerminal>/" ... FILES_MATCHING "*.dll")
    # rule cannot pick them up either.

    add_test(NAME ${_name} COMMAND ${_name})

    if(WIN32 AND _fincept_qt_bindir)
        set_tests_properties(${_name} PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_fincept_qt_bindir}")
    elseif(UNIX AND NOT APPLE AND _fincept_qt_libdir)
        set_tests_properties(${_name} PROPERTIES
            ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:${_fincept_qt_libdir}")
    endif()
endfunction()

# ── Suites ───────────────────────────────────────────────────────────────────

# src/trading/brokers/BrokerModifyFields.h — header-only, zero app sources.
fincept_add_test(tst_broker_modify_fields
    tst_broker_modify_fields.cpp)

# src/core/result/Result.h — header-only, zero app sources.
fincept_add_test(tst_result
    tst_result.cpp)

# src/trading/OrderValidator.{h,cpp} — the .cpp includes only its own header,
# which includes only trading/TradingTypes.h (Qt Core types + plain structs, no
# .cpp of its own). One extra TU, no service/network/broker-registry linkage.
fincept_add_test(tst_order_validator
    tst_order_validator.cpp
    "${PROJECT_SOURCE_DIR}/src/trading/OrderValidator.cpp")

# src/algo_engine/IndicatorEngine.{h,cpp}: the .cpp includes only its own
# header, which includes only algo_engine/AlgoEngineTypes.h (Qt Core types +
# plain structs, no .cpp of its own). One extra TU, same shape as the validator
# suite above.
fincept_add_test(tst_indicator_engine
    tst_indicator_engine.cpp
    "${PROJECT_SOURCE_DIR}/src/algo_engine/IndicatorEngine.cpp")

message(STATUS "Fincept tests: tst_broker_modify_fields, tst_result, tst_order_validator, tst_indicator_engine")

if(FINCEPT_LOCAL_EDITION)
    fincept_add_test(tst_local_edition
        tst_local_edition.cpp
        ../src/services/llm/ProviderCatalog.cpp
        ../src/network/http/HttpClient.cpp
        ../src/network/cloud/CloudClient.cpp
        ../src/core/logging/Logger.cpp)
    target_link_libraries(tst_local_edition PRIVATE Qt6::Network)
endif()

fincept_add_test(tst_proxy_manager tst_proxy_manager.cpp ../src/network/ProxyManager.cpp)
target_link_libraries(tst_proxy_manager PRIVATE Qt6::Network)

# macOS credential migration tests substitute only the OS Keychain boundary.
# Database/SQLite/AES are real; fixtures use temporary databases and fake keys.
if(APPLE)
    fincept_add_test(tst_secure_storage
        tst_secure_storage.cpp
        ../src/storage/secure/SecureStorage.cpp
        ../src/storage/sqlite/Database.cpp
        ../src/storage/sqlite/migrations/MigrationRunner.cpp
        ../src/core/logging/Logger.cpp)
    target_link_libraries(tst_secure_storage PRIVATE Qt6::Sql OpenSSL::Crypto)
endif()

# Real Keychain writes require an explicit local opt-in. CI/default tests keep
# the fake OS boundary above; this suite uses only its own new dummy item.
option(FINCEPT_TEST_REAL_KEYCHAIN "Run isolated real macOS Keychain integration test" OFF)
if(APPLE AND FINCEPT_TEST_REAL_KEYCHAIN)
    fincept_add_test(tst_mac_keychain
        tst_mac_keychain.cpp
        ../src/storage/secure/SecureStorage.cpp
        ../src/storage/secure/MacKeychain.cpp
        ../src/storage/sqlite/Database.cpp
        ../src/storage/sqlite/migrations/MigrationRunner.cpp
        ../src/core/logging/Logger.cpp)
    target_link_libraries(tst_mac_keychain PRIVATE Qt6::Sql OpenSSL::Crypto
        "-framework Security" "-framework CoreFoundation")
endif()

fincept_add_test(tst_datahub_refresh
    tst_datahub_refresh.cpp
    ../src/datahub/DataHub.cpp
    ../src/core/logging/Logger.cpp)
target_link_libraries(tst_datahub_refresh PRIVATE Qt6::Widgets)

# Exercise the existing canned RSS/Atom/JSON/CSV checks without booting the app,
# opening a user database, or making network requests.
fincept_add_test(tst_feed_parsing
    tst_feed_parsing.cpp
    ../src/services/feeds/FeedSelfTest.cpp
    ../src/services/feeds/FeedScraper.cpp
    ../src/services/feeds/FeedParseUtil.cpp)

# Repository integration against temporary SQLite files. Link only the real
# migrations needed by these entities, not the entire application schema.
fincept_add_test(tst_portfolio_storage
    tst_portfolio_storage.cpp
    ../src/services/portfolio/PortfolioTransfer.cpp
    ../src/storage/repositories/PortfolioRepository.cpp
    ../src/storage/repositories/WatchlistRepository.cpp
    ../src/storage/repositories/SettingsRepository.cpp
    ../src/storage/sync/SyncOutbox.cpp
    ../src/storage/sync/CloudSyncSettings.cpp
    ../src/storage/sqlite/Database.cpp
    ../src/storage/sqlite/migrations/MigrationRunner.cpp
    ../src/storage/sqlite/migrations/v001_initial.cpp
    ../src/storage/sqlite/migrations/v006_portfolio_multi.cpp
    ../src/storage/sqlite/migrations/v019_sector_cache.cpp
    ../src/storage/sqlite/migrations/v022_portfolio_broker_link.cpp
    ../src/core/logging/Logger.cpp)
target_link_libraries(tst_portfolio_storage PRIVATE Qt6::Sql)

fincept_add_test(tst_portfolio_valuation tst_portfolio_valuation.cpp)
