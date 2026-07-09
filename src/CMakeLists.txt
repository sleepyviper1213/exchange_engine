add_subdirectory(concurrent_queue)

add_library(order_book SHARED)

target_sources(order_book
    PRIVATE
        engine/order_book.cpp
        io/binance_depth.cpp
)

target_include_directories(order_book
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(order_book
    PUBLIC
        fmt::fmt
        simdjson::simdjson
)

target_compile_options(order_book
    PUBLIC
        $<$<CXX_COMPILER_ID:GNU>:-Wa,-mbig-obj>
        $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
)
if(WIN32)
    set_target_properties(order_book
        PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
	target_add_compile_definitions(order_book PUBLIC _WIN32_WINNT=0x0601)
endif()

function(add_tool target source)

    add_executable(${target} ${source})

    target_link_libraries(${target}
        PRIVATE
            order_book
            OpenSSL::SSL
            OpenSSL::Crypto
    )

    if(boost_beast_FOUND)
        target_link_libraries(${target} PRIVATE Boost::beast)
    else()
        target_link_libraries(${target} PRIVATE Boost::thread)
    endif()

    if(WIN32)
        target_link_libraries(${target} PRIVATE ws2_32 crypt32)
    endif()
endfunction()

enable_coverage(order_book)
set_warnings(order_book)
add_tool(snapshot_tool tools/snapshot_main.cpp)
add_tool(ws_capture_tool tools/ws_capture_main.cpp)
