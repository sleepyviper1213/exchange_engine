#include "counted.hpp"

namespace exchange::core::util{
counted::counted() noexcept { alive.fetch_add(1); }

counted::counted(int v) noexcept : value(v) { alive.fetch_add(1); }

counted::counted(const counted &o) noexcept : value(o.value) {
	alive.fetch_add(1);
}

counted::counted(counted &&o) noexcept : value(o.value) { alive.fetch_add(1); }

counted::~counted() { alive.fetch_sub(1); }
} // namespace exchange::util
