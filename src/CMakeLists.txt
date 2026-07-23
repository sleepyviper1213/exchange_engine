set(EE_INCLUDE_ROOTS
    ${CMAKE_CURRENT_SOURCE_DIR}          # top-level modules: transport/…
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/trading-engine
    ${CMAKE_CURRENT_SOURCE_DIR}/market-data
    CACHE INTERNAL "exchange-engine module include roots")

add_subdirectory(core)
add_subdirectory(transport)
add_subdirectory(trading-engine)
add_subdirectory(market-data)
add_subdirectory(app)
