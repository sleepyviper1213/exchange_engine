# strategy — namespace strategy. Order-type strategies (iceberg, stop) — scaffold.
# Header-only for now; the include roots give it the domain types to build on.
add_library(strategies INTERFACE)
add_library(exchange_engine::strategies ALIAS strategies)

target_include_directories(strategies
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${EE_INCLUDE_ROOTS}
)
