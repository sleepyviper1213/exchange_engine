add_subdirectory(core)
add_subdirectory(transport)
add_subdirectory(trading-engine)
add_subdirectory(market-data)
# Above the engine and market data, and below the composition root: risk screens
# the engine's commands and strategy writes them. @see docs/directory_layout.md
add_subdirectory(risk_management)
add_subdirectory(strategy)
# The live pipeline sits above every module and below the composition root: it is
# the only place market-data and trading-engine are both named, which a module
# above both may do and an edge between them may not.
add_subdirectory(session)
add_subdirectory(app)
