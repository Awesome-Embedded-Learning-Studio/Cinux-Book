#include "../framework/framework.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/futex.hpp"
#include "kernel/proc/sched_class.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "test_assert.hpp"

namespace {

constexpr unsigned int kJoinSeed   = 777;  ///< The value the joiner parks on.
constexpr unsigned int kReadOnly   = 55;   ///< The word behind a read-only promise.
constexpr long long    kLeaverCode = 9;    ///< What every leaving task exits with.

unsigned int             g_join_word    = kJoinSeed;
unsigned int             g_ro_word      = kReadOnly;
cinux::base::KernelError g_join_answer  = cinux::base::KernelError::kOk;
unsigned int             g_join_seen    = kJoinSeed;
bool                     g_wait_answer  = false;
unsigned long long       g_receipt_pid  = 0;
long long                g_receipt_code = 0;

enum class Body : unsigned char {
    kJoiner,
    kMemberMaker,
    kProperMaker,
    kLeaver,
};

struct World;

World* g_world = nullptr;

class RecordingSink {
public:
    void switch_tasks(cinux::proc::Task& outgoing, cinux::proc::Task& incoming);
    void enter_from_main(cinux::proc::Task& incoming);
    void return_to_main(cinux::proc::Task& outgoing);
    void reclaim(cinux::proc::Task& /*dead*/) { reclaimed_count++; }

    unsigned int reclaimed_count = 0;

private:
    static void run_body(cinux::proc::Task& task);
};

struct World {
    RecordingSink     sink;
    cinux::proc::Task alpha;
    cinux::proc::Task beta;
    Body              body         = Body::kLeaver;
    bool              alpha_closed = false;

    explicit World(unsigned long long slot) {
        alpha.name       = "host";
        alpha.stack_base = 0x1000ULL * slot;
        beta.name        = "host";
        beta.stack_base  = 0x1000ULL * (slot + 1);
        cinux::proc::Scheduler::self().init(sink);
        cinux::proc::Scheduler::self().install_class(cinux::proc::DefaultFifoClass());
    }

    void launch(bool beta_first = false) {
        auto& scheduler = cinux::proc::Scheduler::self();
        if (beta_first) {
            scheduler.seat(beta);
            scheduler.seat(alpha);
            return;
        }
        scheduler.seat(alpha);
        scheduler.seat(beta);
    }

    ~World() {
        g_world = this;
        cinux::proc::Scheduler::self().run_until_done();
        g_world = nullptr;
    }

    void run_body_of(cinux::proc::Task& task) {
        auto&      scheduler = cinux::proc::Scheduler::self();
        const bool kMaker    = body == Body::kMemberMaker || body == Body::kProperMaker;
        if (body == Body::kJoiner && &task == &alpha) {
            g_join_answer = cinux::proc::futex::WaitOnUserWord(
                reinterpret_cast<unsigned long long>(&g_join_word), kJoinSeed);
            if (alpha_closed) {
                return;
            }
            g_join_seen  = g_join_word;
            alpha_closed = true;
            scheduler.exit_current();
            return;
        }
        if (kMaker && &task == &alpha) {
            if (body == Body::kMemberMaker) {
                beta.tgid = alpha.pid;
            }
            cinux::proc::ChildReceipt receipt{};
            const bool                kAnswer =
                scheduler.wait_child(static_cast<long long>(beta.pid), false, receipt);
            if (kAnswer && !alpha_closed) {
                g_wait_answer  = true;
                g_receipt_pid  = receipt.pid;
                g_receipt_code = receipt.code;
            }
            if (alpha_closed) {
                return;
            }
            alpha_closed = true;
            scheduler.exit_current();
            return;
        }
        scheduler.exit_current(kLeaverCode);
    }
};

void RecordingSink::enter_from_main(cinux::proc::Task& incoming) {
    g_world->run_body_of(incoming);
}

void RecordingSink::switch_tasks(cinux::proc::Task& /*outgoing*/, cinux::proc::Task& incoming) {
    g_world->run_body_of(incoming);
}

void RecordingSink::return_to_main(cinux::proc::Task& /*outgoing*/) {}

TEST("thread exit: death zeroes the join word and wakes the parked joiner") {
    World world(700);
    world.body                 = Body::kJoiner;
    const unsigned long  kPage = cinux::arch::page::kSize;
    const auto           kWord = reinterpret_cast<unsigned long>(&g_join_word);
    cinux::mm::VmaRegion cover{};
    cover.start     = kWord & ~(kPage - 1UL);
    cover.end       = cover.start + (2UL * kPage);
    cover.kind      = cinux::mm::VmaKind::kAnonymous;
    cover.readable  = true;
    cover.writable  = true;
    world.beta.vmas = cinux::base::Shared<cinux::mm::VmaStore>(new cinux::mm::VmaStore{});
    ASSERT_TRUE(world.beta.vmas.holds());
    ASSERT_TRUE(world.beta.vmas->register_range(cover).ok());
    world.beta.clear_child_tid = static_cast<unsigned long long>(kWord);
    g_join_answer              = cinux::base::KernelError::kWouldBlock;
    g_join_seen                = kJoinSeed;
    world.launch();
    g_world = &world;
    cinux::proc::Scheduler::self().run_until_done();
    g_world = nullptr;
    ASSERT_TRUE(g_join_answer == cinux::base::KernelError::kOk);
    ASSERT_EQ(g_join_seen, 0U);
    ASSERT_EQ(g_join_word, 0U);
    ASSERT_TRUE(world.alpha.state == cinux::proc::TaskState::kDead);
    ASSERT_TRUE(world.beta.state == cinux::proc::TaskState::kDead);
    ASSERT_EQ(world.sink.reclaimed_count, 2U);
}

TEST("thread exit: a word no writable promise covers is left alone") {
    World world(710);
    world.body                  = Body::kLeaver;
    world.alpha.clear_child_tid = cinux::syscall::kUserAddressLimit;
    const unsigned long  kPage  = cinux::arch::page::kSize;
    const auto           kWord  = reinterpret_cast<unsigned long>(&g_ro_word);
    cinux::mm::VmaRegion cover{};
    cover.start     = kWord & ~(kPage - 1UL);
    cover.end       = cover.start + (2UL * kPage);
    cover.kind      = cinux::mm::VmaKind::kAnonymous;
    cover.readable  = true;
    cover.writable  = false;
    world.beta.vmas = cinux::base::Shared<cinux::mm::VmaStore>(new cinux::mm::VmaStore{});
    ASSERT_TRUE(world.beta.vmas.holds());
    ASSERT_TRUE(world.beta.vmas->register_range(cover).ok());
    world.beta.clear_child_tid = static_cast<unsigned long long>(kWord);
    world.launch();
    g_world = &world;
    cinux::proc::Scheduler::self().run_until_done();
    g_world = nullptr;
    ASSERT_EQ(g_ro_word, kReadOnly);
    ASSERT_TRUE(world.alpha.state == cinux::proc::TaskState::kDead);
    ASSERT_TRUE(world.beta.state == cinux::proc::TaskState::kDead);
    ASSERT_EQ(world.sink.reclaimed_count, 2U);
}

TEST("thread exit: a group member leaves the roll and answers no wait") {
    World world(720);
    world.body        = Body::kMemberMaker;
    world.beta.parent = &world.alpha;
    g_wait_answer     = false;
    world.launch();
    g_world = &world;
    cinux::proc::Scheduler::self().run_until_done();
    g_world = nullptr;
    ASSERT_TRUE(!g_wait_answer);
    ASSERT_TRUE(world.beta.state == cinux::proc::TaskState::kDead);
    ASSERT_TRUE(world.alpha.state == cinux::proc::TaskState::kDead);
    ASSERT_EQ(world.sink.reclaimed_count, 2U);
    ASSERT_EQ(cinux::proc::Scheduler::self().offspring_count(), 0ULL);
}

TEST("thread exit: a proper child turns zombie and answers its wait") {
    World world(730);
    world.body        = Body::kProperMaker;
    world.beta.parent = &world.alpha;
    g_wait_answer     = false;
    world.launch(true);
    g_world = &world;
    cinux::proc::Scheduler::self().run_until_done();
    g_world = nullptr;
    ASSERT_TRUE(g_wait_answer);
    ASSERT_EQ(g_receipt_pid, world.beta.pid);
    ASSERT_EQ(g_receipt_code, kLeaverCode);
    ASSERT_TRUE(world.beta.state == cinux::proc::TaskState::kZombie);
    ASSERT_TRUE(world.alpha.state == cinux::proc::TaskState::kDead);
    ASSERT_EQ(world.sink.reclaimed_count, 2U);
}

}  // namespace

int main() {
    return cinux::test::RunAll();
}
