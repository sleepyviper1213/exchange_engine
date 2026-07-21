# persistence — namespace persistence. Event-sourcing store (scaffold).
# Header-only for now; depends on core for the domain event types.
add_library(persistence INTERFACE)
add_library(order_book::persistence ALIAS persistence)

target_include_directories(persistence
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(persistence
    INTERFACE
        order_book::core
)
