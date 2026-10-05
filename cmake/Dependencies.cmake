include_guard(GLOBAL)

find_package(fmt REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(hwy CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(CLI11 REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Boost CONFIG COMPONENTS intrusive pool unordered beast)
find_package(Crc32c CONFIG REQUIRED)
find_package(mp-units 2.4 CONFIG REQUIRED)
set_property(TARGET mp-units::core APPEND PROPERTY
    INTERFACE_COMPILE_DEFINITIONS gsl_FEATURE_GSL_COMPATIBILITY_MODE=1)
    
if(EXCHANGE_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
endif()

if(EXCHANGE_BUILD_BENCHMARKS)
    find_package(benchmark CONFIG REQUIRED)
endif()
