/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "internal.h"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <pthread.h>
#include <sys/ucontext.h>

namespace
{
std::atomic<bool> installed{false};
std::atomic<uintptr_t> owner{0};
std::atomic<nce_direct::Frame *> active{nullptr};
static_assert(decltype(owner)::is_always_lock_free && decltype(active)::is_always_lock_free);
struct sigaction previous[2];
constexpr int signals[] = {SIGBUS, SIGSEGV};

void handler(int number, siginfo_t *info, void *opaque)
{
    int saved_errno = errno;
    auto context = static_cast<ucontext_t *>(opaque);
    auto &registers = context->uc_mcontext->__ss;
    unsigned esr = context->uc_mcontext->__es.__esr;
    unsigned fault_class = esr & 0x3c, exception_class = esr >> 26;
    // Check ownership before reading a stack frame that another thread may
    // release. The interrupted owner cannot return until this handler exits.
    if (owner.load(std::memory_order_acquire) == uintptr_t(pthread_self()) &&
        !(esr & (1U << 6)) && (exception_class == 0x24 || exception_class == 0x25) &&
        (fault_class == 0x04 || fault_class == 0x0c)) {
        auto frame = active.load(std::memory_order_acquire);
        if (frame && registers.__x[27] == uintptr_t(frame)) {
            uintptr_t offset = uintptr_t(info->si_addr) - uintptr_t(frame->read_view);
            uintptr_t pc = __darwin_arm_thread_state64_get_pc(registers);
            const auto &sites = frame->code->read_faults;
            size_t first = 0, last = sites.size();
            while (first < last) {
                size_t middle = first + (last - first) / 2;
                if (sites[middle].host_pc < pc)
                    first = middle + 1;
                else
                    last = middle;
            }
            if (offset < frame->page_table.user_size && first < sites.size() && sites[first].host_pc == pc) {
                frame->address = frame->page_table.user_base + offset;
                ++frame->read_view_faults;
                registers.__x[17] = (sites[first].guest_offset << 4) | nce_direct::ReadPage;
                __darwin_arm_thread_state64_set_pc_fptr(registers, nce_direct::nce_direct_exit_stub);
                errno = saved_errno;
                return;
            }
        }
    }
    auto &prior = previous[number == SIGSEGV];
    if (prior.sa_handler == SIG_DFL) {
        sigaction(number, &prior, nullptr);
        raise(number);
    } else if (prior.sa_handler != SIG_IGN) {
        if (prior.sa_flags & SA_SIGINFO)
            prior.sa_sigaction(number, info, opaque);
        else
            prior.sa_handler(number);
    }
    errno = saved_errno;
}
}  // namespace

namespace nce_direct
{
bool begin_read_view(Frame *frame)
{
    uintptr_t empty = 0;
    if (!installed.load(std::memory_order_acquire) ||
        !owner.compare_exchange_strong(empty, uintptr_t(pthread_self()), std::memory_order_acq_rel))
        return false;
    active.store(frame, std::memory_order_release);
    return true;
}
void end_read_view()
{
    active.store(nullptr, std::memory_order_release);
    owner.store(0, std::memory_order_release);
}
}  // namespace nce_direct

extern "C" int nce_direct_install_read_faults()
{
    if (installed.load(std::memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }
    struct sigaction action{};
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    for (int number : signals)
        sigaddset(&action.sa_mask, number);
    for (unsigned i = 0; i < 2; ++i) {
        if (sigaction(signals[i], &action, &previous[i])) {
            int error = errno;
            if (i)
                sigaction(signals[0], &previous[0], nullptr);
            errno = error;
            return -1;
        }
    }
    installed.store(true, std::memory_order_release);
    return 0;
}
extern "C" int nce_direct_remove_read_faults()
{
    if (!installed.load(std::memory_order_acquire) || owner.load(std::memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }
    // Query both signals before restoring either; a later owner must remove
    // its handlers first so that its saved forwarding target stays valid.
    for (int number : signals) {
        struct sigaction current{};
        if (sigaction(number, nullptr, &current))
            return -1;
        if (!(current.sa_flags & SA_SIGINFO) || current.sa_sigaction != handler) {
            errno = EBUSY;
            return -1;
        }
    }
    for (unsigned i = 0; i < 2; ++i)
        if (sigaction(signals[i], &previous[i], nullptr))
            return -1;
    installed.store(false, std::memory_order_release);
    return 0;
}
