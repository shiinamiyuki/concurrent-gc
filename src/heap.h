#pragma once
#include <cstddef>
#include <cstdint>
#include <list>
namespace gc {
template<size_t ObjectSize>
struct ObjectPool {
};
class RawHeap {

public:
    uint8_t *allocate(size_t size) noexcept;
};
}// namespace gc