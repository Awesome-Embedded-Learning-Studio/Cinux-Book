/**
 * @file    pipe.hpp
 * @brief   One bounded pipe wearing the file-system's erased face.
 *
 * @author  Charliechen114514
 * @date    2026-10-18
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/container/ring_queue.hpp"
#include "cinux/ref_count.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::fs {

/// Bytes one pipe holds before writers wait.
inline constexpr unsigned long kPipeBytes = 4096;

/**
 * @brief   A byte pipe: bounded ring, two semaphores, a writer census.
 * @note    The pipe pretends to be a backend file: read and write on a
 *          descriptor land in pull and push through the same FsOps the
 *          volume's files ride, so nothing in the syscall layer knows
 *          a pipe exists. Readers block on empty until a byte or the
 *          last writer's farewell arrives; writers block on full until
 *          room opens. One reader and one writer is the served shape;
 *          the ring and semaphores tolerate more, the census only
 *          answers the end-of-file question.
 * @since   0.3.0
 * @ingroup kernel_fs
 */
class Pipe final : public base::RefCount<> {
public:
    Pipe();

    /**
     * @brief     Takes up to count bytes; blocks while empty and a
     *            writer lives.
     * @return    The bytes taken, zero at end-of-file, or an error.
     */
    base::Result<unsigned long> pull(void* sink, unsigned long count);

    /**
     * @brief     Puts count bytes; blocks while the ring is full.
     * @return    The bytes put, always count, or an error.
     */
    base::Result<unsigned long> push(const void* source, unsigned long count);

    /**
     * @brief     Records one writer arriving; pipe creation calls this
     *            per write end.
     * @since     0.3.0
     * @ingroup   kernel_fs
     */
    void add_writer();

    /**
     * @brief     Records one writer leaving; the last one wakes every
     *            reader.
     * @since     0.3.0
     * @ingroup   kernel_fs
     */
    void drop_writer();

    /// The poll faces: bytes waiting, farewell arrived, room to write.
    [[nodiscard]] bool has_bytes() const { return ring_.count() != 0; }
    [[nodiscard]] bool writers_gone() const { return writers_ == 0; }
    [[nodiscard]] bool has_room() const { return ring_.count() < kPipeBytes; }

private:
    base::container::RingQueue<unsigned char, kPipeBytes + 1> ring_;
    proc::Semaphore                                           readable_;
    proc::Semaphore                                           writable_;
    unsigned long                                             writers_ = 0;
};

/**
 * @brief     The erased face both pipe ends ride: pull, push, pin.
 * @return    The static FsOps every pipe shares.
 * @note      A descriptor's node says which end it is: opaque null is
 *            the read end, opaque set is the write end, so release
 *            knows whose farewell to record.
 * @since     0.3.0
 * @ingroup   kernel_fs
 */
const FsOps* PipeOps();

}  // namespace cinux::fs
