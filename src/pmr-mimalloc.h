#pragma once

#include <mimalloc.h>

#include <memory_resource>

struct mi_memory_sourece : std::pmr::memory_resource {
    void *do_allocate(size_t bytes, size_t alignment) override {
        return mi_malloc_aligned(bytes, alignment);
    }
    void do_deallocate(void *p, size_t bytes, size_t alignment) override {
        mi_free_aligned(p, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
        return this == &other;
    }
};