include_guard(GLOBAL)

find_package(fmt REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(CLI11 REQUIRED)
find_package(spdlog CONFIG REQUIRED)

find_package(Boost COMPONENTS beast)
if(boost_beast_FOUND)
    message(STATUS "Using standalone Boost.beast")
else()
    find_package(Boost CONFIG REQUIRED COMPONENTS thread)
    message(STATUS "Using classic Boost")
endif()

# The order book's storage: Boost.Intrusive for the per-level order FIFO and the
# price ladder, Boost.Pool for the nodes they link, Boost.Unordered for the
# price->level map. All header-only, and all found the same way whether Boost
# arrived modular (vcpkg) or classic.
find_package(Boost CONFIG REQUIRED COMPONENTS intrusive pool unordered)

if(ORDER_BOOK_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
endif()

if(ORDER_BOOK_BUILD_BENCHMARKS)
    find_package(benchmark CONFIG REQUIRED)
endif()
