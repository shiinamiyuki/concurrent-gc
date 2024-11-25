#include "rc.h"

namespace rc {
std::unique_ptr<std::pmr::memory_resource> RefCounter::resource = std::make_unique<std::pmr::unsynchronized_pool_resource>();
std::unique_ptr<std::pmr::memory_resource> AtomicRefCounter::resource = std::make_unique<std::pmr::synchronized_pool_resource>();
}// namespace rc