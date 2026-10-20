#include "cinux/memory.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/loader/loader.hpp"

namespace cinux::loader {

namespace {

using arch::page::kSize;

struct PicturePlane {
    unsigned char* page;
    unsigned long  page_base;
    unsigned long  strings_at;
    unsigned long  words_at;
};

bool plane_room(const PicturePlane& plane) {
    return plane.words_at + 16 < kSize - plane.strings_at;
}

void put_word(PicturePlane& plane, unsigned long word) {
    cinux::base::CopyBytes(plane.page + plane.words_at, &word, sizeof(word));
    plane.words_at += 8;
}

bool put_pair(PicturePlane& plane, unsigned long type, unsigned long value) {
    if (!plane_room(plane)) {
        return false;
    }
    put_word(plane, type);
    put_word(plane, value);
    return true;
}

bool put_string(PicturePlane& plane, const char* text) {
    if (text == nullptr || !plane_room(plane)) {
        return false;
    }
    unsigned long length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    cinux::base::CopyBytes(plane.page + plane.strings_at, text, length + 1);
    const unsigned long kLanded = plane.page_base + plane.strings_at;
    plane.strings_at += length + 1;
    cinux::base::CopyBytes(plane.page + plane.words_at, &kLanded, sizeof(kLanded));
    plane.words_at += 8;
    return true;
}

unsigned long land_randomness(PicturePlane& plane, const EntryFacts& facts) {
    if (facts.randomness == nullptr || facts.random_bytes == 0 ||
        plane.strings_at + facts.random_bytes + plane.words_at >= kSize) {
        return 0;
    }
    cinux::base::CopyBytes(plane.page + plane.strings_at, facts.randomness, facts.random_bytes);
    const unsigned long kLanded = plane.page_base + plane.strings_at;
    plane.strings_at += facts.random_bytes;
    return kLanded;
}

bool put_facts(PicturePlane& plane, const EntryFacts& facts, unsigned long first_run_landed,
               unsigned long random_landed) {
    if (!put_pair(plane, kAuxPhdr, static_cast<unsigned long>(facts.program_headers)) ||
        !put_pair(plane, kAuxPhent, static_cast<unsigned long>(facts.header_entry_bytes)) ||
        !put_pair(plane, kAuxPhnum, static_cast<unsigned long>(facts.header_count)) ||
        !put_pair(plane, kAuxPageSize, static_cast<unsigned long>(facts.page_bytes)) ||
        !put_pair(plane, kAuxBase, static_cast<unsigned long>(facts.interpreter_base)) ||
        !put_pair(plane, kAuxEntry, static_cast<unsigned long>(facts.program_entry)) ||
        !put_pair(plane, kAuxUid, 0) || !put_pair(plane, kAuxEuid, 0) ||
        !put_pair(plane, kAuxGid, 0) || !put_pair(plane, kAuxEgid, 0) ||
        !put_pair(plane, kAuxSecure, 0)) {
        return false;
    }
    if (random_landed != 0 && !put_pair(plane, kAuxRandom, random_landed)) {
        return false;
    }
    if (first_run_landed != 0 && !put_pair(plane, kAuxExecfn, first_run_landed)) {
        return false;
    }
    return true;
}

}  // namespace

unsigned long WriteEntryPicture(unsigned char* stack_page, unsigned long entry_rsp,
                                const EntryRuns& runs, const EntryFacts& facts) {
    const unsigned long kPageBase = entry_rsp - (entry_rsp % kSize);
    PicturePlane        plane{
        .page       = stack_page,
        .page_base  = kPageBase,
        .strings_at = 0,
        .words_at   = entry_rsp - kPageBase,
    };
    const unsigned long kRandomLanded = land_randomness(plane, facts);
    unsigned long       argc          = 0;
    while (runs.arguments[argc] != nullptr) {
        ++argc;
    }
    unsigned long envc = 0;
    if (runs.environment != nullptr) {
        while (runs.environment[envc] != nullptr) {
            ++envc;
        }
    }
    put_word(plane, argc);
    const unsigned long kFirstStringAt = plane.strings_at;
    for (unsigned long index = 0; index < argc; ++index) {
        if (!put_string(plane, runs.arguments[index])) {
            return 0;
        }
    }
    put_word(plane, 0);
    for (unsigned long index = 0; index < envc; ++index) {
        if (!put_string(plane, runs.environment[index])) {
            return 0;
        }
    }
    put_word(plane, 0);
    const unsigned long kFirstRunLanded = argc > 0 ? kPageBase + kFirstStringAt : 0;
    if (!put_facts(plane, facts, kFirstRunLanded, kRandomLanded)) {
        return 0;
    }
    put_word(plane, static_cast<unsigned long>(kAuxNull));
    put_word(plane, 0);
    return entry_rsp;
}

}  // namespace cinux::loader
