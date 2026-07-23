add_executable(exchange_tool
    cli.cpp
    logger.cpp
    main.cpp)

target_link_libraries(exchange_tool
    PRIVATE
    exchange_engine::transport
    exchange_engine::market_data
    exchange_engine::core
    Threads::Threads
    CLI11::CLI11
    spdlog::spdlog
    exchange_engine::malloc
)

# Feed the project version (from project(VERSION …)) into version.hpp.
target_compile_definitions(exchange_tool
    PRIVATE
    EXCHANGE_ENGINE_VERSION="${PROJECT_VERSION}"
    EXCHANGE_ENGINE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    EXCHANGE_ENGINE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    EXCHANGE_ENGINE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

