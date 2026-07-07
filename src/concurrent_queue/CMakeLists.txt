# Header-only single-producer/single-consumer byte queue.
add_library(concurrent_queue INTERFACE
        reader_writer_queue.hpp
        atomicops.h)
target_include_directories(concurrent_queue INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
