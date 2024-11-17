#include "gc.h"
#include <print>

namespace gc {
void TracingContext::shade(const GcObjectContainer *ptr) const noexcept {
    if (!ptr) return;
    heap.shade(ptr);
}
GcHeap &get_heap() {
    static thread_local GcHeap heap;
    return heap;
}
void GcHeap::mark_some() {
}
void GcHeap::scan_roots() {
}
void GcHeap::collect() {
    with_tracing_func([&](auto tracing_func) {
        auto *ptr = head_;
        for (ptr = head_; ptr != nullptr; ptr = ptr->next_) {
            if (ptr->is_root()) {
                tracing_func(ptr);
            }
        }
        while (!work_list.empty()) {
            auto *ptr = pop_from_working_list();
            tracing_func(ptr);
        }
        GcObjectContainer *prev = nullptr;
        ptr = head_;
        while (ptr) {
            auto next = ptr->next_;
            GC_ASSERT(!is_grey(ptr), "Object should not be gray");
            if (ptr->is_root() || is_black(ptr)) {
                prev = ptr;
                ptr->set_marked(false);
                ptr = next;
            } else {
                if constexpr (is_debug) {
                    std::printf("freeing object %p\n", static_cast<void *>(ptr));
                }
                free_object(ptr);
                GC_ASSERT(!ptr->is_alive(), "Object should not be alive");
                if (!prev) {
                    head_ = next;
                } else {
                    prev->next_ = next;
                }
                ptr = next;
            }
        }
    });
}
}// namespace gc