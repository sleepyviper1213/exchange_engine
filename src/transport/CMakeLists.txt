add_library(transport SHARED
    rest.cpp
    replay.cpp
    websocket.cpp
    fwd.hpp
    detail/coroutine_token.hpp)
add_library(exchange_engine::transport ALIAS transport)

target_include_directories(transport
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)


target_compile_options(transport
    PUBLIC
    $<$<CXX_COMPILER_ID:GNU>:-Wa,-mbig-obj>
    $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
)

if (WIN32)
    target_compile_definitions(transport PUBLIC _WIN32_WINNT=0x0601)
endif ()

target_link_libraries(transport
    PUBLIC
    fmt::fmt
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(ORDER_BOOK_WITH_DPDK)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "ORDER_BOOK_WITH_DPDK requires Linux")
    endif()

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)

    target_sources(transport PRIVATE dpdk.cpp)
    target_link_libraries(transport PRIVATE PkgConfig::DPDK)
    target_compile_definitions(transport PUBLIC ORDER_BOOK_WITH_DPDK=1)
endif()

# Boost.Beast: standalone package when present, otherwise the header-only target.
if (boost_beast_FOUND)
    target_link_libraries(transport PUBLIC Boost::beast)
else ()
    target_link_libraries(transport PUBLIC Boost::thread)
endif ()

if (WIN32)
    target_link_libraries(transport PUBLIC ws2_32 crypt32)
endif ()

generate_module_export_header(transport)
target_compile_options(transport PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/wd4251>
    $<$<CXX_COMPILER_ID:MSVC>:/wd4275>
)
