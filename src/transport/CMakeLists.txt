add_library(transport SHARED
    rest.cpp
    replay.cpp
    websocket.cpp)
add_library(exchange_engine::transport ALIAS transport)

target_include_directories(transport
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}   # generated transport_export.h
)

target_compile_options(transport
    PUBLIC
    $<$<CXX_COMPILER_ID:GNU>:-Wa,-mbig-obj>
    $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
)

if (WIN32)
    set_target_properties(transport
        PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
    target_compile_definitions(transport PUBLIC _WIN32_WINNT=0x0601)
endif ()

target_link_libraries(transport
    PUBLIC
    fmt::fmt
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Boost.Beast: standalone package when present, otherwise the header-only target.
if (boost_beast_FOUND)
    target_link_libraries(transport PUBLIC Boost::beast)
else ()
    target_link_libraries(transport PUBLIC Boost::thread)
endif ()

if (WIN32)
    target_link_libraries(transport PUBLIC ws2_32 crypt32)
endif ()

# Explicit dllexport/dllimport via TRANSPORT_EXPORT (works under MSVC ASan,
# unlike WINDOWS_EXPORT_ALL_SYMBOLS). Public API is annotated in the headers.
include(GenerateExportHeader)
generate_export_header(transport)
target_compile_options(transport PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/wd4251>
    $<$<CXX_COMPILER_ID:MSVC>:/wd4275>
)
