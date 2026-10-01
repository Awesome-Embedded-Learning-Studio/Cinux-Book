#include "gdt.hpp"

extern "C" cinux::boot::gdt::BootGdt const kBootGdt = cinux::boot::gdt::kTemplate;

extern "C" cinux::boot::gdt::DescriptorTablePointer const kGdtr = {
    .limit = sizeof(kBootGdt) - 1,
    .base  = reinterpret_cast<unsigned int>(&kBootGdt),
};
