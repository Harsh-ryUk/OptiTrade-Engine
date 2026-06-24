cmake_minimum_required(VERSION 3.22)

project(OptiTradeEngine
    VERSION 1.0.0
    LANGUAGES CXX
)

option(OPTITRADE_ENABLE_DPDK "Build DPDK applications and benchmarks" ON)
option(OPTITRADE_BUILD_BENCHMARKS "Build latency benchmark executables" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_library(optitrade_headers INTERFACE)

target_include_directories(optitrade_headers INTERFACE
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(optitrade_headers INTERFACE
    cxx_std_20
)

function(optitrade_enable_warnings target_name)
    target_compile_options(${target_name} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endfunction()

function(optitrade_add_test target_name source_file)
    if(BUILD_TESTING)
        add_executable(${target_name}
            ${PROJECT_SOURCE_DIR}/${source_file}
        )

        target_link_libraries(${target_name} PRIVATE
            optitrade_headers
        )

        optitrade_enable_warnings(${target_name})

        # Tests currently use assert(...); keep assertions enabled in Release builds.
        target_compile_options(${target_name} PRIVATE
            -UNDEBUG
        )

        add_test(
            NAME ${target_name}
            COMMAND ${target_name}
        )
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Unit and engine correctness tests
# ---------------------------------------------------------------------------

optitrade_add_test(
    optitrade_test_wire_market_data
    tests/test_wire_market_data.cpp
)

optitrade_add_test(
    optitrade_test_wire_order
    tests/test_wire_order.cpp
)

optitrade_add_test(
    optitrade_test_wire_invalid_packet
    tests/test_wire_invalid_packet.cpp
)

optitrade_add_test(
    optitrade_test_ethernet_market_frame
    tests/test_ethernet_market_frame.cpp
)

optitrade_add_test(
    optitrade_test_ethernet_order_frame
    tests/test_ethernet_order_frame.cpp
)

optitrade_add_test(
    optitrade_test_engine_wire_adapter
    tests/test_engine_wire_adapter.cpp
)

optitrade_add_test(
    optitrade_test_engine_scenarios
    tests/test_engine_scenarios.cpp
)

# ---------------------------------------------------------------------------
# Linux raw-packet peer applications
# ---------------------------------------------------------------------------

add_executable(optitrade_afpacket_generator
    ${PROJECT_SOURCE_DIR}/apps/optitrade_afpacket_generator.cpp
)

target_link_libraries(optitrade_afpacket_generator PRIVATE
    optitrade_headers
)

optitrade_enable_warnings(optitrade_afpacket_generator)

target_compile_options(optitrade_afpacket_generator PRIVATE
    -O3
)

if(OPTITRADE_BUILD_BENCHMARKS)
    add_executable(optitrade_afpacket_benchmark_generator
        ${PROJECT_SOURCE_DIR}/apps/optitrade_afpacket_benchmark_generator.cpp
    )

    target_link_libraries(optitrade_afpacket_benchmark_generator PRIVATE
        optitrade_headers
    )

    optitrade_enable_warnings(optitrade_afpacket_benchmark_generator)

    target_compile_options(optitrade_afpacket_benchmark_generator PRIVATE
        -O3
    )
endif()

# ---------------------------------------------------------------------------
# DPDK applications
# ---------------------------------------------------------------------------

if(OPTITRADE_ENABLE_DPDK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)

    pkg_get_variable(DPDK_LIBDIR libdpdk libdir)

    find_library(DPDK_NET_RING_LIBRARY
        NAMES rte_net_ring
        HINTS
            "${DPDK_LIBDIR}"
            "/usr/lib/x86_64-linux-gnu"
        PATH_SUFFIXES
            "dpdk/pmds-26.0"
            "dpdk/pmds-26"
            "dpdk/pmds-25.11"
            "dpdk/pmds-25"
            "dpdk/pmds-24.0"
            "dpdk/pmds-24"
            "dpdk/pmds-23.11"
            "dpdk/pmds-23"
            "dpdk/pmds-22.11"
            "dpdk/pmds-22"
    )

    if(NOT DPDK_NET_RING_LIBRARY)
        message(FATAL_ERROR
            "Could not find librte_net_ring required for virtual Ring PMD executables.")
    endif()

    message(STATUS "DPDK Ring PMD library: ${DPDK_NET_RING_LIBRARY}")

    get_filename_component(DPDK_NET_RING_DIRECTORY
        "${DPDK_NET_RING_LIBRARY}"
        DIRECTORY
    )

    function(optitrade_add_dpdk_app target_name source_file)
        add_executable(${target_name}
            ${PROJECT_SOURCE_DIR}/${source_file}
        )

        target_link_libraries(${target_name} PRIVATE
            optitrade_headers
            PkgConfig::DPDK
        )

        optitrade_enable_warnings(${target_name})

        target_compile_options(${target_name} PRIVATE
            -O3
        )
    endfunction()

    function(optitrade_link_ring_pmd target_name)
        target_link_libraries(${target_name} PRIVATE
            "${DPDK_NET_RING_LIBRARY}"
        )

        set_property(
            TARGET ${target_name}
            APPEND PROPERTY BUILD_RPATH "${DPDK_NET_RING_DIRECTORY}"
        )
    endfunction()

    # Virtual Ring PMD correctness validation.
    optitrade_add_dpdk_app(
        optitrade_dpdk_scenarios
        apps/optitrade_dpdk_scenarios.cpp
    )

    optitrade_link_ring_pmd(optitrade_dpdk_scenarios)

    if(BUILD_TESTING)
        add_test(
            NAME optitrade_test_dpdk_scenarios_virtual
            COMMAND optitrade_dpdk_scenarios
                -l 0
                --no-pci
                --no-huge
                --in-memory
                --mbuf-pool-ops-name=ring_mp_mc
                --file-prefix=ot_ctest_scenarios
        )
    endif()

    # Kernel-backed AF_PACKET functional engine.
    optitrade_add_dpdk_app(
        optitrade_dpdk_afpacket_engine
        apps/optitrade_dpdk_afpacket_engine.cpp
    )

    if(OPTITRADE_BUILD_BENCHMARKS)
        # Ring PMD latency baseline used for the published virtual result.
        optitrade_add_dpdk_app(
            optitrade_dpdk_latency_benchmark
            apps/optitrade_dpdk_latency_benchmark.cpp
        )

        optitrade_link_ring_pmd(optitrade_dpdk_latency_benchmark)

        # Regime-specific Ring PMD benchmark.
        optitrade_add_dpdk_app(
            optitrade_dpdk_regime_benchmark
            apps/optitrade_dpdk_regime_benchmark.cpp
        )

        optitrade_link_ring_pmd(optitrade_dpdk_regime_benchmark)

        # AF_PACKET / veth application-side latency benchmark.
        optitrade_add_dpdk_app(
            optitrade_dpdk_afpacket_latency
            apps/optitrade_dpdk_afpacket_latency.cpp
        )
    endif()
endif()

add_executable(optitrade_hello apps/optitrade_hello.cpp)
target_link_libraries(optitrade_hello PRIVATE optitrade_headers)
