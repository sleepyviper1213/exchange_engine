#pragma once
// CPU-affinity & thread-isolation framework.

// IWYU pragma: begin_exports
#include "affinity/affinity.hpp" //pin the calling thread (the syscall layer)
#include "affinity/core_allocator.hpp" //assign one dedicated core per named role
#include "affinity/isolation.hpp" //which CPUs the kernel keeps its own work off
#include "affinity/topology.hpp" //discover which CPUs share a physical core
// IWYU pragma: end_exports
