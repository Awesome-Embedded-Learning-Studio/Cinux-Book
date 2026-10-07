#include "kernel/mm/heap.hpp"

#include <stdint.h>

#include "cinux/math.hpp"
#include "cinux/ptr.hpp"

namespace cinux::mm {

namespace {

constexpr unsigned long kHeaderBytes  = 32;
constexpr unsigned long kFooterBytes  = 16;
constexpr unsigned long kPayloadAlign = 16;
constexpr unsigned long kMinPayload   = 16;

enum class BlockState : unsigned char {
    kFree,
    kLive
};

struct Header {
    unsigned long payload;
    unsigned long state;
    unsigned long next;
    unsigned long spare;
};
static_assert(sizeof(Header) == kHeaderBytes);

unsigned long span_bytes(unsigned long payload) {
    return kHeaderBytes + payload + kFooterBytes;
}

Header* header_at(unsigned long block) {
    return cinux::base::PtrAt<Header>(block);
}

void write_footer(unsigned long block, unsigned long payload) {
    *cinux::base::PtrAt<unsigned long>(block + kHeaderBytes + payload) = span_bytes(payload);
}

unsigned long read_prev_span(unsigned long block) {
    return *cinux::base::PtrAt<unsigned long>(block - kFooterBytes);
}

unsigned long round_payload(unsigned long bytes) {
    unsigned long const kUnits   = cinux::base::math::Ceil(bytes, kPayloadAlign);
    unsigned long const kRounded = kUnits * kPayloadAlign;
    return kRounded < kMinPayload ? kMinPayload : kRounded;
}

void make_free_block(unsigned long block, unsigned long payload) {
    Header* const kHeader = header_at(block);
    kHeader->payload      = payload;
    kHeader->state        = static_cast<unsigned long>(BlockState::kFree);
    kHeader->next         = 0;
    kHeader->spare        = 0;
    write_footer(block, payload);
}

void link_sorted(unsigned long* head, unsigned long block) {
    if (*head == 0 || block < *head) {
        header_at(block)->next = *head;
        *head                  = block;
        return;
    }
    unsigned long scan = *head;
    while (header_at(scan)->next != 0 && header_at(scan)->next < block) {
        scan = header_at(scan)->next;
    }
    header_at(block)->next = header_at(scan)->next;
    header_at(scan)->next  = block;
}

void unlink(unsigned long* head, unsigned long block) {
    if (*head == block) {
        *head = header_at(block)->next;
        return;
    }
    unsigned long scan = *head;
    while (scan != 0 && header_at(scan)->next != block) {
        scan = header_at(scan)->next;
    }
    if (scan != 0) {
        header_at(scan)->next = header_at(block)->next;
    }
}

unsigned long pick_free_block(unsigned long head, unsigned long payload) {
    for (unsigned long scan = head; scan != 0; scan = header_at(scan)->next) {
        if (header_at(scan)->payload >= payload) {
            return scan;
        }
    }
    return 0;
}

}  // namespace

bool Heap::init(unsigned long base, unsigned long bytes) {
    if (base % kPayloadAlign != 0 || bytes < span_bytes(kMinPayload)) {
        return false;
    }
    base_                        = base;
    top_                         = base + bytes;
    unsigned long const kPayload = bytes - kHeaderBytes - kFooterBytes;
    make_free_block(base, kPayload);
    free_head_  = base;
    free_bytes_ = kPayload;
    return true;
}

void* Heap::allocate(unsigned long bytes) {
    unsigned long const kWanted = round_payload(bytes);
    unsigned long const kBlock  = pick_free_block(free_head_, kWanted);
    if (kBlock == 0) {
        return nullptr;
    }
    unlink(&free_head_, kBlock);
    Header* const       kHeader    = header_at(kBlock);
    unsigned long const kOld       = kHeader->payload;
    unsigned long const kRemainder = kOld - kWanted;
    if (kRemainder >= kHeaderBytes + kFooterBytes + kMinPayload) {
        unsigned long const kTailPayload = kRemainder - kHeaderBytes - kFooterBytes;
        unsigned long const kTail        = kBlock + span_bytes(kWanted);
        make_free_block(kTail, kTailPayload);
        link_sorted(&free_head_, kTail);
        kHeader->payload = kWanted;
        write_footer(kBlock, kWanted);
        free_bytes_ = free_bytes_ - kOld + kTailPayload;
    } else {
        free_bytes_ -= kOld;
    }
    kHeader->state = static_cast<unsigned long>(BlockState::kLive);
    return cinux::base::PtrAt<void>(kBlock + kHeaderBytes);
}

bool Heap::free(void* block) {
    unsigned long const kBlock = reinterpret_cast<unsigned long>(block) - kHeaderBytes;
    if (kBlock < base_ || kBlock >= top_) {
        return false;
    }
    Header* const kHeader = header_at(kBlock);
    if (kHeader->state != static_cast<unsigned long>(BlockState::kLive)) {
        return false;
    }
    unsigned long const kSpan = span_bytes(kHeader->payload);
    if (kBlock + kSpan > top_ || kBlock + kSpan <= kBlock) {
        return false;
    }
    unsigned long       merged = kHeader->payload;
    unsigned long const kNext  = kBlock + kSpan;
    if (kNext < top_ && header_at(kNext)->state == static_cast<unsigned long>(BlockState::kFree)) {
        unlink(&free_head_, kNext);
        merged += kHeaderBytes + kFooterBytes + header_at(kNext)->payload;
        free_bytes_ += kHeaderBytes + kFooterBytes;
    }
    unsigned long target = kBlock;
    if (kBlock > base_) {
        unsigned long const kPrev = kBlock - read_prev_span(kBlock);
        if (kPrev >= base_ &&
            header_at(kPrev)->state == static_cast<unsigned long>(BlockState::kFree)) {
            target = kPrev;
            merged += kHeaderBytes + kFooterBytes + header_at(kPrev)->payload;
            free_bytes_ += kHeaderBytes + kFooterBytes;
        }
    }
    unsigned long const kReturned = kHeader->payload;
    header_at(target)->payload    = merged;
    header_at(target)->state      = static_cast<unsigned long>(BlockState::kFree);
    write_footer(target, merged);
    if (target == kBlock) {
        link_sorted(&free_head_, target);
    } else {
        kHeader->state = static_cast<unsigned long>(BlockState::kFree);
    }
    free_bytes_ += kReturned;
    return true;
}

bool Heap::grow(unsigned long new_top) {
    if (new_top <= top_ || new_top % kPayloadAlign != 0 ||
        new_top - top_ < span_bytes(kMinPayload)) {
        return false;
    }
    unsigned long const kBlock   = top_;
    unsigned long const kPayload = new_top - top_ - kHeaderBytes - kFooterBytes;
    top_                         = new_top;
    make_free_block(kBlock, kPayload);
    if (kBlock > base_) {
        unsigned long const kPrev = kBlock - read_prev_span(kBlock);
        if (header_at(kPrev)->state == static_cast<unsigned long>(BlockState::kFree)) {
            Header* const kPrevHeader = header_at(kPrev);
            kPrevHeader->payload += kHeaderBytes + kFooterBytes + kPayload;
            write_footer(kPrev, kPrevHeader->payload);
            free_bytes_ += kHeaderBytes + kFooterBytes + kPayload;
            return true;
        }
    }
    link_sorted(&free_head_, kBlock);
    free_bytes_ += kPayload;
    return true;
}

unsigned long Heap::free_bytes() const {
    return free_bytes_;
}

unsigned long Heap::top() const {
    return top_;
}

}  // namespace cinux::mm
