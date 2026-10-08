#pragma once

#include "cinux/ref_count.hpp"

namespace cinux::test {

struct RefObject {
    explicit RefObject(unsigned int& destroyed) : destroyed_(destroyed) {}
    ~RefObject() { ++destroyed_; }

    void add_ref() { references_.retain(); }
    void release_ref() {
        if (references_.release()) {
            delete this;
        }
    }
    [[nodiscard]] unsigned long use_count() const { return references_.use_count(); }

private:
    base::RefCount<> references_;
    unsigned int&    destroyed_;
};

}  // namespace cinux::test
