add_library(memory INTERFACE)

add_library(exchange_engine::memory ALIAS memory)

target_include_directories(memory INTERFACE ${EE_INCLUDE_ROOTS})
