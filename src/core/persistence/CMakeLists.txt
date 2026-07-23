# persistence — namespace persistence. Event-sourcing store (scaffold).
# Header-only for now; the include roots give it the domain event types.
add_library(persistence INTERFACE)
add_library(exchange_engine::persistence ALIAS persistence)

target_include_directories(persistence
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${EE_INCLUDE_ROOTS}
)
