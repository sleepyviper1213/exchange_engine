add_library(transport SHARED
    rest.cpp
    replay.cpp
    websocket.cpp
)

add_library(exchange_engine::transport ALIAS transport)

target_include_directories(transport
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>)


target_compile_options(transport
    PUBLIC
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<PLATFORM_ID:Windows>>:-Wa,-mbig-obj>
    $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
)

if (WIN32)
    target_compile_definitions(transport PUBLIC _WIN32_WINNT=0x0601)
endif ()

target_link_libraries(transport
    PUBLIC
    fmt::fmt
	Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
)

# DPDK discovery comes from cmake/Dpdk.cmake; both targets are empty when the
# option is off. dpdk_api carries only the ORDER_BOOK_WITH_DPDK macro that
# transport's headers switch on, so it is PUBLIC; the rte_* link line stays
# PRIVATE to this library. Only the DPDK source is conditional here.
target_link_libraries(transport
    PUBLIC exchange_engine::dpdk_api
    PRIVATE exchange_engine::dpdk)

if(ORDER_BOOK_WITH_DPDK)
    target_sources(transport PRIVATE dpdk.cpp)
endif()


if (WIN32)
    target_link_libraries(transport PUBLIC ws2_32 crypt32)
endif ()

generate_module_export_header(transport)
target_compile_options(transport PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/wd4251>
    $<$<CXX_COMPILER_ID:MSVC>:/wd4275>
)
