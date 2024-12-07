#include "rc.h"
#include "pmr-mimalloc.h"
namespace rc {
std::unique_ptr<std::pmr::memory_resource> RefCounter::resource = std::make_unique<mi_memory_sourece>();
std::unique_ptr<std::pmr::memory_resource> AtomicRefCounter::resource = std::make_unique<mi_memory_sourece>();
}// namespace rc