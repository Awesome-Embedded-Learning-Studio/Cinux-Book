/**
 * @file    loader.hpp
 * @brief   Placing one ELF image inside an address space: the mapping
 *          half of exec for both seats a program's world holds — the
 *          fixed-base program and the shared-object interpreter — plus
 *          the hosted entry picture a runtime wakes up to.
 *
 * The mapping half of exec: everything the parser accepted becomes
 * pages — each file-backed byte lands where its segment says, and the
 * gap between file bytes and memory bytes arrives as zeros, so bss
 * starts life clean without the image carrying it. Pages are mapped
 * whole even when a segment begins mid-page: the head of the first
 * page belongs to whatever the linker placed before it, and the tail
 * past the last file byte is zero, which is exactly the memory an
 * initialized-then-bss image expects.
 *
 * @author  Charliechen114514
 * @date    2026-10-12
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_loader
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "kernel/fs/vfs.hpp"
#include "kernel/loader/elf_layout.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/vma.hpp"

namespace cinux::loader {

/**
 * @brief   Where one image lands: the world, the register, the source,
 *          and the placement.
 * @note    One bundle because they travel together on every load call:
 *          pages go into the space, the segment report goes into the
 *          register, and the register's file-backed ranges name the
 *          node the image was read from. The node rides by value; the
 *          register keeps it as identity, never as authority. The face
 *          beside it travels too, so the register's promises stay
 *          serviceable after exec returns and the FsView they came
 *          from is gone. The placement pair closes the bundle: which
 *          seat the image fills decides the file type the parser
 *          accepts, and the base decides where relative addresses
 *          land.
 * @since   0.2.0
 * @ingroup kernel_loader
 */
struct LoadWorld {
    const mm::AddressSpace&      space;        ///< The freshly built world pages land in.
    mm::VmaStore&                vmas;         ///< The range register segments report to.
    fs::FsNode                   file{};       ///< The node the image bytes came from.
    const fs::FsOps*             file_ops{};   ///< The backend face serving file's bytes.
    void*                        file_self{};  ///< The backend instance behind file_ops.
    base::Shared<fs::MountLease> file_lifetime{nullptr};  ///< Runtime source backend ownership.
    ImageRole                    role{};  ///< Which seat the image fills; the default is the
                                          ///< program seat.
    std::uint64_t                base{};  ///< Where the image lands; the default zero serves the
                                          ///< program's absolute addresses, an interpreter brings
                                          ///< its fixed base.
};

/**
 * @brief    The numbers a hosted runtime reads out of the auxiliary
 *           vector, gathered where they are born.
 * @note     The loader fills the image half (entry, header table,
 *           header geometry) as part of mapping; the exec path adds the
 *           world half (interpreter base, page size, randomness source)
 *           before the entry picture writes every word. The identity
 *           words stay unwritten here and land as zeroes: one root
 *           credential is the whole truth this kernel knows.
 * @since    0.4.0
 * @ingroup  kernel_loader
 */
struct EntryFacts {
    std::uint64_t       program_entry{};       ///< AT_ENTRY: the main program's first instruction.
    std::uint64_t       program_headers{};     ///< AT_PHDR: where the main image's header table
                                               ///< lands; zero when nothing covers it.
    std::uint64_t       header_entry_bytes{};  ///< AT_PHENT: bytes per program-header entry.
    std::uint64_t       header_count{};        ///< AT_PHNUM: entries in the table.
    std::uint64_t       interpreter_base{};    ///< AT_BASE: the interpreter's load base, zero when
                                               ///< the program is static.
    std::uint64_t       page_bytes{};          ///< AT_PAGESZ: the machine's page size.
    const char*         platform{};      ///< AT_PLATFORM payload source; null omits the entry.
    std::uint64_t       hwcap{};         ///< AT_HWCAP word; zero advertises no features.
    const std::uint8_t* randomness{};    ///< AT_RANDOM payload source; null omits the entry.
    std::size_t         random_bytes{};  ///< Bytes readable at randomness.
};

/** @brief End of the auxiliary vector. */
inline constexpr std::uint64_t kAuxNull     = 0;
/** @brief Program-headers address of the main image. */
inline constexpr std::uint64_t kAuxPhdr     = 3;
/** @brief Bytes per program-header entry. */
inline constexpr std::uint64_t kAuxPhent    = 4;
/** @brief Count of program-header entries. */
inline constexpr std::uint64_t kAuxPhnum    = 5;
/** @brief Page size in bytes. */
inline constexpr std::uint64_t kAuxPageSize = 6;
/** @brief Load base of the dynamic interpreter. */
inline constexpr std::uint64_t kAuxBase     = 7;
/** @brief Entry point of the main program. */
inline constexpr std::uint64_t kAuxEntry    = 9;
/** @brief Real user id. */
inline constexpr std::uint64_t kAuxUid      = 11;
/** @brief Effective user id. */
inline constexpr std::uint64_t kAuxEuid     = 12;
/** @brief Real group id. */
inline constexpr std::uint64_t kAuxGid      = 13;
/** @brief Effective group id. */
inline constexpr std::uint64_t kAuxEgid     = 14;
/** @brief Nonzero only for secure execution. */
inline constexpr std::uint64_t kAuxSecure   = 23;
/** @brief Address of sixteen randomness bytes. */
inline constexpr std::uint64_t kAuxRandom   = 25;
/** @brief Path of the executed file. */
inline constexpr std::uint64_t kAuxExecfn   = 31;
/** @brief Pointer to the platform string; the one machine served. */
inline constexpr std::uint64_t kAuxPlatform = 15;
/** @brief Hardware capability bits, zero when none are advertised. */
inline constexpr std::uint64_t kAuxHwcap    = 16;

/** @brief The string an AT_PLATFORM consumer reads: the one machine served. */
inline constexpr char kPlatformName[] = "x86_64";

/**
 * @brief    The argument and environment runs a hosted runtime wakes
 *           up to, gathered because they travel together to the one
 *           writer that lays them onto the entry stack.
 * @since    0.4.0
 * @ingroup  kernel_loader
 */
struct EntryRuns {
    const char* const* arguments{};    ///< NUL-terminated runs, ended by a null entry.
    const char* const* environment{};  ///< Ended by a null entry; null itself for none.
};

/**
 * @brief         Maps every loadable segment of one image into a world
 *                and reports each to the range register.
 *
 * @param[in]     data    The image bytes.
 * @param[in]     bytes   How many are readable at data.
 * @param[in]     world   The space to fill, the register to report to,
 *                        the source node, and the placement: which
 *                        seat the image fills and where it lands. The
 *                        bundle's own words never move; the space and
 *                        register it names do the changing.
 * @param[out]    facts   The image half of the entry facts: entry,
 *                        header-table address, and header geometry,
 *                        all with the base applied.
 * @return        true when the image parsed, every page mapped, and
 *                every segment registered; false when the parser refused
 *                or a segment's range collided with one already recorded.
 * @note          Allocates one frame per page the segments cover and
 *                writes through the direct map, so the live CR3 never
 *                matters here. A segment running past the file stops
 *                the machine loudly rather than mapping garbage.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       kernel_loader
 */
[[nodiscard]] bool LoadElfIntoSpace(const std::uint8_t* data, std::size_t bytes,
                                    const LoadWorld& world, EntryFacts& facts);

/**
 * @brief Loads a named interpreter beside the program.
 * @param[in] path Absolute interpreter path; null returns empty facts.
 * @param[in] space Address space receiving the interpreter.
 * @param[in,out] ranges Range register receiving its segments.
 * @return Interpreter entry and base, or the read and image errors.
 * @since 0.8.0
 * @ingroup kernel_loader
 */
[[nodiscard]] base::Result<EntryFacts> LoadInterpreter(const char*             path,
                                                       const mm::AddressSpace& space,
                                                       mm::VmaStore&           ranges);

/**
 * @brief         Writes the hosted entry picture onto one zeroed stack
 *                page and hands back the entry stack pointer.
 *
 * @param[in,out] stack_page    The page's bytes through the direct map;
 *                              its low end receives the argument and
 *                              environment strings, NUL included, and
 *                              the randomness payload when there is one.
 * @param[in]     entry_rsp     Where the first instruction finds the
 *                              stack pointer; the pointer words start
 *                              here and climb.
 * @param[in]     runs          The argument and environment runs the
 *                              picture lays down first.
 * @param[in]     facts         The auxiliary-vector numbers: the image
 *                              half from the loader, the world half
 *                              from the exec path.
 * @return        The entry stack pointer, the same value handed in.
 * @note          The picture is the hosted shape: argc, argv with its
 *              null end, envp with its null end, then the auxiliary
 *              vector the facts describe, closed by AT_NULL — the
 *              layout musl's first instructions already know how to
 *              walk. AT_EXECFN reuses argv[0]'s landing when there is
 *              one; the identity words answer zero. Strings land at
 *              the page's low end and climb toward the pointers; the
 *              caller owns keeping both halves inside the one page.
 * @warning       None
 * @throws        None
 * @since         0.3.0
 * @ingroup       kernel_loader
 */
unsigned long WriteEntryPicture(unsigned char* stack_page, unsigned long entry_rsp,
                                const EntryRuns& runs, const EntryFacts& facts);

}  // namespace cinux::loader
