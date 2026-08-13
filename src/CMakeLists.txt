add_subdirectory(core)
add_subdirectory(transport)
add_subdirectory(trading-engine)
add_subdirectory(market-data)
# Above the engine and market data, and below the composition root: risk screens
# the engine's commands and strategy writes them. @see docs/directory_layout.md
add_subdirectory(risk_management)
add_subdirectory(strategy)
add_subdirectory(app)
