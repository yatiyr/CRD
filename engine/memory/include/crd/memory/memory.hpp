#pragma once

// Umbrella header for the crd-memory module.
//
// Typical usage:
//
//     #include <crd/memory/memory.hpp>
//
//     using namespace crd::memory;
//
//     IAllocator* alloc = default_allocator();
//     int* p = construct<int>(*alloc, 42);
//     // ...
//     destroy(*alloc, p);
//
// Or, with a frame-scoped scratch:
//
//     LinearAllocator scratch(1 * 1024 * 1024);
//     {
//         LinearScope scope(scratch);
//         auto* tmp = allocate_array<float>(scratch, 1024);
//         // ... use tmp ...
//     } // scratch is reset

#include <crd/memory/alignment.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/memory/allocators/pool_allocator.hpp>
#include <crd/memory/allocators/stack_allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/memory/log_channel.hpp>
#include <crd/memory/memory_stats.hpp>
