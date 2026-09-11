add_library(orders SHARED
    order.cpp
)
add_library(exchange_engine::orders ALIAS orders)

target_include_directories(orders
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(orders
    PUBLIC
    fmt::fmt
)

target_compile_options(orders PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/wd4251 /wd4275>
)

generate_module_export_header(orders)
enable_coverage(orders)
set_warnings(orders)
enable_hardening(orders)
