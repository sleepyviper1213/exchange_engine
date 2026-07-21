# strategies — namespace strategies. Order-type strategies (iceberg, stop) — scaffold.
# Header-only for now; depends on core for the domain types it will build on.
add_library(strategies INTERFACE)
add_library(order_book::strategies ALIAS strategies)

target_include_directories(strategies
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(strategies
    INTERFACE
        order_book::core
)
