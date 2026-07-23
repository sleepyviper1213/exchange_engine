#pragma once

namespace core {
struct Configuration; // defined in configuration.hpp (must match the struct key)
void init_logging(const Configuration &config);

} // namespace core
