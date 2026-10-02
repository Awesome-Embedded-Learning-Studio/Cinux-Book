#include "kernel/boot/image_header.hpp"

extern "C" unsigned char g_kernel_load_paddr[];
extern "C" unsigned char g_kernel_file_size[];
extern "C" unsigned char g_kernel_mem_size[];
extern "C" unsigned char g_kernel_entry[];

namespace {

[[gnu::used, gnu::section(".image_header")]] cinux::boot::ImageHeader const kImageHeader = {
    .magic       = cinux::boot::kImageMagic,
    .version     = cinux::boot::kImageVersion,
    .header_size = sizeof(cinux::boot::ImageHeader),
    .load_paddr  = reinterpret_cast<unsigned long long>(g_kernel_load_paddr),
    .file_size   = reinterpret_cast<unsigned long long>(g_kernel_file_size),
    .mem_size    = reinterpret_cast<unsigned long long>(g_kernel_mem_size),
    .entry       = reinterpret_cast<unsigned long long>(g_kernel_entry),
};

}  // namespace
