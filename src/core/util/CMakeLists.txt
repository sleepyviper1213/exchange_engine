add_library(util SHARED
    slurp.cpp
)
add_library(exchange_engine::util ALIAS util)

target_include_directories(util
    PUBLIC
    ${EE_INCLUDE_ROOTS}
    ${CMAKE_CURRENT_BINARY_DIR}   # generated util_export.h
)


include(GenerateExportHeader)
generate_export_header(util)

enable_coverage(util)
set_warnings(util)
enable_hardening(util)
