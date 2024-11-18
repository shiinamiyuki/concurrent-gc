#include "gc.h"

namespace gc {
void TracingContext::shade(const GcObjectContainer *ptr) const noexcept {
    heap.shade(ptr);
}
GcHeap &get_heap() {
    static thread_local GcHeap heap;
    return heap;
}

bool GcHeap::mark_some() {
    if (work_list.empty()) {
        scan_roots();
        if (work_list.empty()) {
            sweep();
            return false;
        }
    }
    auto ptr = pop_from_working_list();
    scan(ptr);
    return true;
}
void GcHeap::scan_roots() {
    auto *ptr = head_;
    for (ptr = head_; ptr != nullptr; ptr = ptr->next_) {
        scan(ptr);
    }
}
void GcHeap::sweep() {
    if constexpr (is_debug) {
        std::println("starting sweep");
    }
    GcObjectContainer *prev = nullptr;
    auto ptr = head_;
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
}
void GcHeap::collect() {
    scan_roots();
    while (!work_list.empty()) {
        mark_some();
    }
    sweep();
}
}// namespace gc