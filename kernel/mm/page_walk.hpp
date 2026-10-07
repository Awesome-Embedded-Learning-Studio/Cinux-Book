/**
 * @file    page_walk.hpp
 * @brief   The four-level page-table walk, shared by both worlds.
 *
 * Map, unmap, and translate as free templates over a TableWorld: the walk
 * logic lives here exactly once, while each world supplies how a table
 * page is reached and where fresh table pages come from. The kernel
 * world reaches tables through the direct map and draws new pages from
 * the PMM; the host test world points them at a plain memory pool, so
 * the one walk runs against real tables in both. Index and offset
 * arithmetic goes through the base bit-field vocabulary and the named
 * WalkLevel — no raw shifts, no bare level numbers. TLB maintenance is
 * deliberately outside: the walk edits tables, callers own translations.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <concepts>

#include "cinux/bit_ops/bitmask.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"

namespace cinux::mm::walk {

using cinux::arch::page::Entry;
using cinux::base::bit::BitRange;

/**
 * @brief   What a world must provide for the shared walk to run on it.
 * @note    table() turns a physical address into a writable table page;
 *          take_page() hands out one zeroed 4 KiB page as a fresh table,
 *          physical address 0 meaning refusal.
 * @since   0.1.0
 * @ingroup kernel_mm
 */
template <typename World>
concept TableWorld = requires(World& world, unsigned long physical) {
    { world.table(physical) } -> std::same_as<Entry*>;
    { world.take_page() } -> std::convertible_to<unsigned long>;
};

constexpr unsigned int kEntriesPerTable = 512;

/**
 * @brief   One step of the four-level descent, named.
 * @note    kPml4 reads the topmost nine index bits, kPageTable the
 *          lowest; large-page leaves may sit at kPdpt (1 GiB pages)
 *          and kPageDirectory (2 MiB pages).
 * @since   0.2.0
 * @ingroup kernel_mm
 */
enum class WalkLevel : unsigned char {
    kPml4          = 0,
    kPdpt          = 1,
    kPageDirectory = 2,
    kPageTable     = 3,
};

/**
 * @brief         Where one level reads its nine-bit index in an address.
 *
 * @param[in]     level   The walk level asked about.
 * @return        Bit range of that level's index.
 * @note          A large leaf's in-page offset spans exactly the low
 *                bits below its own index range, so the range's low
 *                doubles as the leaf's offset width.
 * @since         0.2.0
 * @ingroup       kernel_mm
 */
constexpr BitRange IndexRange(WalkLevel level) {
    switch (level) {
    case WalkLevel::kPml4:
        return {.low = 39, .width = 9};
    case WalkLevel::kPdpt:
        return {.low = 30, .width = 9};
    case WalkLevel::kPageDirectory:
        return {.low = 21, .width = 9};
    case WalkLevel::kPageTable:
        return {.low = 12, .width = 9};
    }
    return {.low = 0, .width = 0};
}

/// The levels the walk descends through before the page table.
constexpr WalkLevel kUpperLevels[3] = {
    WalkLevel::kPml4,
    WalkLevel::kPdpt,
    WalkLevel::kPageDirectory,
};

/**
 * @brief         Reads the table index one level consumes.
 *
 * @param[in]     virtual_address   The address being walked.
 * @param[in]     level             Which level's index to read.
 * @return        The nine-bit index for that level.
 * @since         0.2.0
 * @ingroup       kernel_mm
 */
inline unsigned long SlotIndex(unsigned long virtual_address, WalkLevel level) {
    return static_cast<unsigned long>(Entry{virtual_address}.extract(IndexRange(level)));
}

/**
 * @brief         Keeps the low bits of an address below a leaf's base.
 *
 * @param[in]     virtual_address   The address being resolved.
 * @param[in]     width             How many low bits the leaf offsets.
 * @return        The in-leaf offset.
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
inline unsigned long LowBits(unsigned long virtual_address, unsigned char width) {
    return static_cast<unsigned long>(
        Entry{virtual_address}.extract(BitRange{.low = 0, .width = width}));
}

/**
 * @brief         Reads the physical address a table-pointer entry names.
 *
 * @param[in]     slot   A table-pointer entry.
 * @return        The next table's physical address.
 * @since         0.2.0
 * @ingroup       kernel_mm
 */
inline unsigned long EntryTarget(const Entry& slot) {
    Entry address{};
    address.deposit(cinux::arch::page::kTablePhys, slot.extract(cinux::arch::page::kTablePhys));
    return address.raw;
}

/**
 * @brief         Follows one table-pointer entry to the next table.
 *
 * @param[in,out] world   The table world the walk runs on.
 * @param[in]     slot    A present, non-large table entry.
 * @return        The next-level table.
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
template <TableWorld World>
Entry* FollowTable(World& world, const Entry& slot) {
    return world.table(EntryTarget(slot));
}

/// Where the walk ended: the entry it landed on, and the level that
/// entry belongs to.
struct SlotAt {
    Entry*    entry;
    WalkLevel level;
};

/**
 * @brief         Walks to the slot an address resolves to, read-only.
 *
 * @param[in,out] world            The table world the walk runs on.
 * @param[in]     root             Physical address of the PML4 to walk.
 * @param[in]     virtual_address   The address whose slot is wanted.
 * @return        The slot and its level; entry is null when the walk
 *                fell off before reaching one.
 * @note          Stops early on a large-page leaf: the entry and its
 *                level are reported for the caller to decode.
 * @since         0.2.0
 * @ingroup       kernel_mm
 */
template <TableWorld World>
SlotAt FindSlot(World& world, unsigned long root, unsigned long virtual_address) {
    Entry* table = world.table(root);
    for (const WalkLevel kLevel : kUpperLevels) {
        Entry& slot = table[SlotIndex(virtual_address, kLevel)];
        if (!slot.has(cinux::arch::page::kPresent)) {
            return SlotAt{.entry = nullptr, .level = kLevel};
        }
        if (slot.has(cinux::arch::page::kLarge)) {
            return SlotAt{.entry = &slot, .level = kLevel};
        }
        table = FollowTable(world, slot);
    }
    return SlotAt{.entry = &table[SlotIndex(virtual_address, WalkLevel::kPageTable)],
                  .level = WalkLevel::kPageTable};
}

/**
 * @brief         Walks to the page table an address maps through,
 *                creating missing intermediate tables on the way.
 *
 * @param[in,out] world            The table world the walk runs on.
 * @param[in]     root             Physical address of the PML4 to walk.
 * @param[in]     virtual_address   The address whose page table is wanted.
 * @param[in]     leaf_flags        Flags each freshly created table
 *                                  carries in its pointer entry.
 * @return        The final page table, or null on refusal or on meeting
 *                a large-page leaf that cannot host a 4 KiB slot.
 * @since         0.2.0
 * @ingroup       kernel_mm
 */
template <TableWorld World>
Entry* EnsureLeafTable(World& world, unsigned long root, unsigned long virtual_address,
                       Entry leaf_flags) {
    Entry* table = world.table(root);
    for (const WalkLevel kLevel : kUpperLevels) {
        Entry& slot = table[SlotIndex(virtual_address, kLevel)];
        if (!slot.has(cinux::arch::page::kPresent)) {
            unsigned long const kFresh = world.take_page();
            if (kFresh == 0) {
                return nullptr;
            }
            slot  = cinux::arch::page::MakeTableEntry(kFresh,
                                                      cinux::arch::page::kWritable | leaf_flags);
            table = world.table(kFresh);
        } else if (slot.has(cinux::arch::page::kLarge)) {
            return nullptr;
        } else {
            table = FollowTable(world, slot);
        }
    }
    return table;
}

/**
 * @brief         Maps one 4 KiB page, creating missing intermediate
 *                tables from the world's supply.
 *
 * @param[in,out] world            The table world the walk runs on.
 * @param[in]     root             Physical address of the PML4 to walk.
 * @param[in]     virtual_address   Target address; the low bits of the
 *                                  physical side are dropped by the entry.
 * @param[in]     physical          Physical page to map.
 * @return        true when the final entry was written.
 * @note          Writes present-plus-writable entries throughout; the
 *                kernel-side face. Refuses to split a large-page leaf it
 *                meets mid-walk; that collision is the caller's policy
 *                problem.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
template <TableWorld World>
bool MapPage(World& world, unsigned long root, unsigned long virtual_address,
             unsigned long physical) {
    Entry* const kTable = EnsureLeafTable(world, root, virtual_address, Entry{});
    if (kTable == nullptr) {
        return false;
    }
    kTable[SlotIndex(virtual_address, WalkLevel::kPageTable)] =
        cinux::arch::page::MakeTableEntry(physical, cinux::arch::page::kWritable);
    return true;
}

/**
 * @brief         Maps one 4 KiB page ring 3 may reach.
 *
 * @param[in,out] world            The table world the walk runs on.
 * @param[in]     root             Physical address of the PML4 to walk.
 * @param[in]     virtual_address   Target address; the low bits of the
 *                                  physical side are dropped by the entry.
 * @param[in]     physical          Physical page to map.
 * @return        true when the final entry was written.
 * @note          Same walk with kUser on every level it touches —
 *                including each freshly created table, because a
 *                clearance lost at any one level denies the walk below
 *                it.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       kernel_mm
 */
template <TableWorld World>
bool MapUserPage(World& world, unsigned long root, unsigned long virtual_address,
                 unsigned long physical) {
    Entry* const kTable = EnsureLeafTable(world, root, virtual_address, cinux::arch::page::kUser);
    if (kTable == nullptr) {
        return false;
    }
    kTable[SlotIndex(virtual_address, WalkLevel::kPageTable)] = cinux::arch::page::MakeTableEntry(
        physical, cinux::arch::page::kWritable | cinux::arch::page::kUser);
    return true;
}

/**
 * @brief         Clears one 4 KiB mapping and reports what was there.
 *
 * @param[in,out] world            The table world the walk runs on.
 * @param[in]     root             Physical address of the PML4 to walk.
 * @param[in]     virtual_address   The page to unmap.
 * @return        The entry that occupied the slot, or a zero entry when
 *                the walk fell off before reaching one.
 * @note          Table-only: flushing the translation is the caller's
 *                duty, because only the caller knows which world ran.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
template <TableWorld World>
Entry UnmapPage(World& world, unsigned long root, unsigned long virtual_address) {
    SlotAt const kSlot = FindSlot(world, root, virtual_address);
    if (kSlot.entry == nullptr || kSlot.level != WalkLevel::kPageTable) {
        return Entry{};
    }
    Entry const kOld = *kSlot.entry;
    *kSlot.entry     = Entry{};
    return kOld;
}

/**
 * @brief         Translates a virtual address to its physical byte.
 *
 * @param[in,out] world            The table world the walk runs on.
 * @param[in]     root             Physical address of the PML4 to walk.
 * @param[in]     virtual_address   The byte to resolve.
 * @return        Physical address of the same byte, or 0 when unmapped.
 * @note          Reads through 1 GiB, 2 MiB, and 4 KiB leaves alike:
 *                whichever level turns present-with-large ends the walk
 *                and its field supplies the physical base.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       kernel_mm
 */
template <TableWorld World>
unsigned long TranslatePage(World& world, unsigned long root, unsigned long virtual_address) {
    SlotAt const kSlot = FindSlot(world, root, virtual_address);
    if (kSlot.entry == nullptr || !kSlot.entry->has(cinux::arch::page::kPresent)) {
        return 0;
    }
    if (kSlot.level != WalkLevel::kPageTable) {
        if (kSlot.level == WalkLevel::kPml4) {
            return 0;
        }
        auto const kField = kSlot.level == WalkLevel::kPdpt ? cinux::arch::page::kHugePagePhys
                                                            : cinux::arch::page::kLargePagePhys;
        Entry      physical{};
        physical.deposit(kField, kSlot.entry->extract(kField));
        return physical.raw | LowBits(virtual_address, IndexRange(kSlot.level).low);
    }
    Entry physical{};
    physical.deposit(cinux::arch::page::kTablePhys,
                     kSlot.entry->extract(cinux::arch::page::kTablePhys));
    return physical.raw | LowBits(virtual_address, IndexRange(WalkLevel::kPageTable).low);
}

}  // namespace cinux::mm::walk
