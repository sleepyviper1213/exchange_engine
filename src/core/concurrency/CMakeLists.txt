add_library(concurrency INTERFACE)

add_library(exchange_engine::concurrency ALIAS concurrency)

target_include_directories(concurrency INTERFACE ${EE_INCLUDE_ROOTS})
