/**
 * @file    shared.hpp
 * @brief   Shared and Weak: shared ownership over RefCount-carrying objects.
 *
 * Shared is intrusive on purpose: the object carries its RefCount, the
 * pointer carries nothing. Copying a Shared is one retain, dropping one
 * release, and the last owner's release deletes the object — the same
 * discipline the file-system nodes already live by, now spelled as a
 * type instead of scattered calls. Weak watches without owning: each
 * Weak parks in the object's watcher list, the dying object empties
 * every watcher it carries, and a Weak whose object died locks to
 * nothing instead of dangling.
 *
 * @author  Charliechen114514
 * @date    2026-10-12
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_ptr
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <utility>

// NOLINTNEXTLINE(misc-include-cleaner) consumed by the Sharedable constraint
#include "cinux/ref_count.hpp"

namespace cinux::base {

template <typename Stuff>
class Weak;

/**
 * @brief         The anchor Weak watches through; lives inside the shared object.
 * @note          The dying object calls vacate(), every Weak watching goes
 *                empty at once, and nobody reads freed memory. The anchor
 *                itself is plain storage: the object's lifetime covers it.
 * @since         0.1.0
 * @ingroup       base_ptr
 */
class WeakAnchor {
public:
    /// An anchor nobody watches yet.
    WeakAnchor() = default;

    /// Copies are refused: the anchor is part of one object.
    WeakAnchor(const WeakAnchor&)            = delete;
    WeakAnchor& operator=(const WeakAnchor&) = delete;

    /// Detaches every watcher; the dying object's last act.
    void vacate();

    /// Detaches every watcher by destruction, the RAII half.
    ~WeakAnchor() { vacate(); }

private:
    template <typename Stuff>
    friend class Weak;

    void* watched_       = nullptr;
    void* first_watcher_ = nullptr;
};

/**
 * @brief         One shared hold over a RefCount-carrying object.
 * @tparam        Stuff   The shared type; supplies retain/release.
 * @note          Copying retains, destructing releases, the last release
 *                deletes. A default Shared holds nothing. Conversion to
 *                Weak is always a borrow.
 * @since         0.1.0
 * @ingroup       base_ptr
 */
template <typename Stuff>
class Shared {
public:
    /// Holds nothing.
    Shared() = default;

    /// Adopts one live object whose count already includes this hold.
    explicit Shared(Stuff* held) : held_(held) {}

    /// Retains one more owner.
    Shared(const Shared& other) : held_(other.held_) {
        if (held_ != nullptr) {
            held_->retain();
        }
    }

    Shared& operator=(const Shared& other) {
        if (this != &other) {
            release_hold();
            held_ = other.held_;
            if (held_ != nullptr) {
                held_->retain();
            }
        }
        return *this;
    }

    /// Takes the hold; the source lets go without releasing.
    Shared(Shared&& other) noexcept : held_(other.held_) { other.held_ = nullptr; }

    Shared& operator=(Shared&& other) noexcept {
        if (this != &other) {
            release_hold();
            held_       = other.held_;
            other.held_ = nullptr;
        }
        return *this;
    }

    /// Releases this hold; the last release deletes the object.
    ~Shared() { release_hold(); }

    /// Whether anything is held.
    [[nodiscard]] bool holds() const { return held_ != nullptr; }

    /// The borrowed object; null when holding nothing.
    [[nodiscard]] Stuff* get() const { return held_; }

    /// Reaches the shared object.
    Stuff& operator*() const { return *held_; }

    /// Reaches the shared object.
    Stuff* operator->() const { return held_; }

    /**
     * @brief         Drops this hold early; destruction runs empty after.
     * @return        None.
     * @since         0.1.0
     * @ingroup       base_ptr
     */
    void reset() { release_hold(); }

private:
    template <typename Watcher>
    friend class Weak;

    void release_hold() {
        if (held_ != nullptr && held_->release()) {
            delete held_;
        }
        held_ = nullptr;
    }

    Stuff* held_ = nullptr;
};

/**
 * @brief         A watching hold that never keeps the object alive.
 * @tparam        Stuff   The watched type.
 * @note          Locking answers a Shared when the object still lives,
 *                an empty Shared when it does not. The dying object
 *                empties every watcher, so a stale lock fails safely.
 * @since         0.1.0
 * @ingroup       base_ptr
 */
template <typename Stuff>
class Weak {
public:
    /// Watches nothing.
    Weak() = default;

    /// Watches one live object through its anchor.
    explicit Weak(WeakAnchor& anchor, Stuff* watched);

    /// Copies watch the same object.
    Weak(const Weak& other);

    Weak& operator=(const Weak& other);

    /// Takes over the watch; the source stops watching.
    Weak(Weak&& other) noexcept;

    Weak& operator=(Weak&& other) noexcept;

    /// Stops watching.
    ~Weak();

    /// A Shared when the object lives, an empty one when it died.
    [[nodiscard]] Shared<Stuff> lock() const;

    /// Whether the watched object still lives.
    [[nodiscard]] bool watching() const { return watched_ != nullptr; }

private:
    friend class WeakAnchor;

    void unlink();

    WeakAnchor* anchor_  = nullptr;
    Stuff*      watched_ = nullptr;
    Weak*       next_    = nullptr;
    Weak*       prev_    = nullptr;
};

inline void WeakAnchor::vacate() {
    watched_      = nullptr;
    // NOLINTNEXTLINE(misc-const-correctness) the walker writes every field it passes
    auto* watcher = static_cast<Weak<void>*>(first_watcher_);
    while (watcher != nullptr) {
        auto* const kNext = watcher->next_;
        watcher->watched_ = nullptr;
        watcher->anchor_  = nullptr;
        watcher->next_    = nullptr;
        watcher->prev_    = nullptr;
        watcher           = static_cast<Weak<void>*>(kNext);
    }
    first_watcher_ = nullptr;
}

template <typename Stuff>
Weak<Stuff>::Weak(WeakAnchor& anchor, Stuff* watched)
    : anchor_(&anchor), watched_(watched), next_(nullptr), prev_(nullptr) {
    static_assert(sizeof(Weak<Stuff>) == sizeof(Weak<void>), "watcher nodes share one shape");
    if (watched_ == nullptr) {
        anchor_ = nullptr;
        return;
    }
    void* const kHead = anchor.first_watcher_;
    next_             = static_cast<Weak<Stuff>*>(kHead);
    if (next_ != nullptr) {
        next_->prev_ = this;
    }
    anchor.first_watcher_ = this;
    if (anchor.watched_ == nullptr) {
        anchor.watched_ = watched_;
    }
}

template <typename Stuff>
Weak<Stuff>::Weak(const Weak& other)
    : anchor_(other.anchor_), watched_(other.watched_), next_(nullptr), prev_(nullptr) {
    if (anchor_ == nullptr) {
        return;
    }
    void* const kHead = anchor_->first_watcher_;
    next_             = static_cast<Weak<Stuff>*>(kHead);
    if (next_ != nullptr) {
        next_->prev_ = this;
    }
    anchor_->first_watcher_ = this;
}

template <typename Stuff>
Weak<Stuff>& Weak<Stuff>::operator=(const Weak& other) {
    if (this != &other) {
        unlink();
        anchor_  = other.anchor_;
        watched_ = other.watched_;
        next_    = nullptr;
        prev_    = nullptr;
        if (anchor_ != nullptr) {
            void* const kHead = anchor_->first_watcher_;
            next_             = static_cast<Weak<Stuff>*>(kHead);
            if (next_ != nullptr) {
                next_->prev_ = this;
            }
            anchor_->first_watcher_ = this;
        }
    }
    return *this;
}

template <typename Stuff>
Weak<Stuff>::Weak(Weak&& other) noexcept
    : anchor_(other.anchor_), watched_(other.watched_), next_(other.next_), prev_(other.prev_) {
    other.anchor_  = nullptr;
    other.watched_ = nullptr;
    other.next_    = nullptr;
    other.prev_    = nullptr;
    if (anchor_ == nullptr) {
        return;
    }
    if (prev_ != nullptr) {
        prev_->next_ = this;
    } else {
        anchor_->first_watcher_ = this;
    }
    if (next_ != nullptr) {
        next_->prev_ = this;
    }
}

template <typename Stuff>
Weak<Stuff>& Weak<Stuff>::operator=(Weak&& other) noexcept {
    if (this != &other) {
        unlink();
        anchor_        = other.anchor_;
        watched_       = other.watched_;
        next_          = other.next_;
        prev_          = other.prev_;
        other.anchor_  = nullptr;
        other.watched_ = nullptr;
        other.next_    = nullptr;
        other.prev_    = nullptr;
        if (anchor_ != nullptr) {
            if (prev_ != nullptr) {
                prev_->next_ = this;
            } else {
                anchor_->first_watcher_ = this;
            }
            if (next_ != nullptr) {
                next_->prev_ = this;
            }
        }
    }
    return *this;
}

template <typename Stuff>
Weak<Stuff>::~Weak() {
    unlink();
}

template <typename Stuff>
void Weak<Stuff>::unlink() {
    if (anchor_ == nullptr) {
        return;
    }
    if (prev_ != nullptr) {
        prev_->next_ = next_;
    } else {
        anchor_->first_watcher_ = next_;
    }
    if (next_ != nullptr) {
        next_->prev_ = prev_;
    }
    anchor_  = nullptr;
    watched_ = nullptr;
    next_    = nullptr;
    prev_    = nullptr;
}

template <typename Stuff>
Shared<Stuff> Weak<Stuff>::lock() const {
    if (watched_ == nullptr || anchor_ == nullptr || anchor_->watched_ == nullptr) {
        return Shared<Stuff>();
    }
    watched_->retain();
    return Shared<Stuff>(watched_);
}

/**
 * @brief         Makes one object and hands back its first shared hold.
 * @tparam        Stuff   The type to build.
 * @tparam        Args    Constructor argument types.
 * @param[in]     args    Constructor arguments.
 * @return        A Shared holding the new object, empty when the
 *                allocation refused.
 * @note          The fresh count of one belongs to the returned Shared.
 * @since         0.1.0
 * @ingroup       base_ptr
 */
template <typename Stuff, typename... Args>
[[nodiscard]] Shared<Stuff> MakeShared(Args&&... args) {
    return Shared<Stuff>(new Stuff(std::forward<Args>(args)...));
}

}  // namespace cinux::base
