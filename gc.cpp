#include "gc.h"
#include <print>
namespace gc {

GcHeap &get_heap() {
    static thread_local GcHeap heap;
    return heap;
}
void GcHeap::collect() {
    auto *ptr = head_;
    for (; ptr != nullptr; ptr = ptr->next_) {
        ptr->is_marked_ = false;
    };
    std::function<void(const GcObjectContainer*)> tracing_func = [&](const GcObjectContainer *ptr) {
        ptr->is_marked_ = true;
        if (auto traceable = ptr->as_tracable(); traceable != nullptr) {
            traceable->trace(tracing_func);
        }
    };
    for (ptr = head_; ptr != nullptr; ptr = ptr->next_) {
        if (ptr->is_root_) {
            tracing_func(ptr);
        }
    }
    GcObjectContainer *prev = nullptr;
    ptr = head_;
    while (ptr) {
        auto next = ptr->next_;
        if (ptr->is_root_ || ptr->is_marked_) {
            prev = ptr;
            ptr = next;
        } else {
            std::printf("freeing object %p\n", static_cast<void*>(ptr));
            free_object(ptr);
            if (!prev) {
                head_ = next;
            } else {
                prev->next_ = next;
            }
            ptr = next;
        }
    }
}
}// namespace gc