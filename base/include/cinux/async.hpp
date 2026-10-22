/**
 * @file    async.hpp
 * @brief   A one-shot completion callback, typed on its owner.
 *
 * The smallest unit of asynchronous machinery an operation can hand
 * its owner: a handler word and the owner it belongs to. The handler
 * receives the owner by reference, so nothing between arming and
 * firing ever touches an untyped word — the cast discipline lives in
 * the type system. Operations that complete later arm one before
 * submitting, the completer invokes it once, and the shot disarms
 * itself on the way out so a stale completion cannot fire twice.
 * Nothing here knows what an interrupt, a transfer or a scheduler is:
 * both worlds compile it, and host tests drive it with plain mocks.
 *
 * @author  Charliechen114514
 * @date    2026-10-22
 * @version 0.2
 * @since   0.6.0
 * @ingroup base_async
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::base {

/**
 * @brief   One armed completion: a handler and the owner it serves.
 * @note    Value-shaped on purpose — compose it into whatever object
 *          owns the operation's lifetime, no base to derive from and
 *          no heap required. Invoking fires once and disarms, so a
 *          late or duplicated completion lands on empty air. The bus
 *          that carries completions erases at its own edge, the way
 *          every routing seam in this kernel does.
 * @since   0.6.0
 * @ingroup base_async
 */
template <typename Owner>
class AsyncCallback {
public:
    /// What a completed operation hands its owner: status and length.
    using Handler = void (*)(Owner& owner, unsigned long status, unsigned long length);

    /// @brief   An empty callback, armed by nothing yet.
    AsyncCallback() = default;

    AsyncCallback(const AsyncCallback&)            = delete;
    AsyncCallback& operator=(const AsyncCallback&) = delete;

    /**
     * @brief         Seats one handler and its owner.
     *
     * @param[in]     handler   The word to call on completion.
     * @param[in]     owner     The object the handler serves.
     * @return        None
     * @note          Arming over an armed seat replaces it — re-arming
     *                per submission is the intended rhythm.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       base_async
     */
    void arm(Handler handler, Owner* owner) {
        handler_ = handler;
        owner_   = owner;
    }

    /**
     * @brief         Fires the armed handler once, disarming it first.
     *
     * @param[in]     status   The completion's status word, caller's meaning.
     * @param[in]     length   How much the operation moved, in bytes.
     * @return        None
     * @note          Disarming before the call keeps a handler that
     *                re-arms itself from firing twice on one shot. An
     *                empty seat takes nothing.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       base_async
     */
    void invoke(unsigned long status, unsigned long length) {
        const Handler kHandler = handler_;
        handler_               = nullptr;
        if (kHandler != nullptr) {
            kHandler(*owner_, status, length);
        }
    }

    /**
     * @brief         Whether one handler waits armed.
     *
     * @return        True after arm, false before and after invoke.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       base_async
     */
    [[nodiscard]] bool armed() const { return handler_ != nullptr; }

    /**
     * @brief         Clears the seat without firing it.
     *
     * @return        None
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       base_async
     */
    void disarm() { handler_ = nullptr; }

private:
    Handler handler_ = nullptr;
    Owner*  owner_   = nullptr;
};

}  // namespace cinux::base
