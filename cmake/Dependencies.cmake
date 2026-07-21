include_guard(GLOBAL)

find_package(fmt REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

find_package(Boost COMPONENTS beast)
if(boost_beast_FOUND)
    message(STATUS "Using standalone Boost.beast")
else()
    find_package(Boost CONFIG REQUIRED COMPONENTS thread)
    message(STATUS "Using classic Boost")
endif()

if(ORDER_BOOK_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
endif()

if(ORDER_BOOK_BUILD_BENCHMARKS)
    find_package(benchmark CONFIG REQUIRED)
endif()
