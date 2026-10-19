#include <mutex>
#include <string>
#include <thread>
#include <condition_variable>

#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/thread.h>

struct event_cpp11_t {
    std::condition_variable cond;
    std::mutex              mutex;
    bool                    state = false;
};

extern "C" {

thread_t *
thread_create_named(void (*thread_rout)(void *param), void *param, const char *name)
{
    /* NULL when the thread cannot be made, as on every platform; a
       std::system_error must not leave C code. A caller that can carry on
       without the thread checks for it; most do not, and the log says why
       they then wait for a thread that never ran. */
    try {
        /* The new thread names itself only once it runs, and by then the
           caller's name may be gone (a buffer of its own): it takes a copy. */
        const std::string own = name ? name : "";

        return new std::thread([thread_rout, param, own] {
            plat_set_thread_name(NULL, own.c_str());
            thread_rout(param);
        });
    } catch (...) {
        pclog("Thread %s could not be started\n", name ? name : "");
        return nullptr;
    }
}

int
thread_wait(thread_t *arg)
{
    if (!arg)
        return 0;
    auto thread = reinterpret_cast<std::thread *>(arg);
    thread->join();
    delete thread;
    return 0;
}

mutex_t *
thread_create_mutex(void)
{
    auto mutex = new std::mutex;
    return mutex;
}

int
thread_test_mutex(mutex_t *_mutex)
{
    if (_mutex == nullptr)
        return 0;

    auto mutex = reinterpret_cast<std::mutex *>(_mutex);
    return mutex->try_lock() ? 1 : 0;
}

int
thread_wait_mutex(mutex_t *_mutex)
{
    if (_mutex == nullptr)
        return 0;

    auto mutex = reinterpret_cast<std::mutex *>(_mutex);
    mutex->lock();
    return 1;
}

int
thread_release_mutex(mutex_t *_mutex)
{
    if (_mutex == nullptr)
        return 0;

    auto mutex = reinterpret_cast<std::mutex *>(_mutex);
    mutex->unlock();
    return 1;
}

void
thread_close_mutex(mutex_t *_mutex)
{
    auto mutex = reinterpret_cast<std::mutex *>(_mutex);
    delete mutex;
}

event_t *
thread_create_event()
{
    auto ev = new event_cpp11_t;
    return ev;
}

void
thread_yield(void)
{
    std::this_thread::yield();
}

int
thread_wait_event(event_t *handle, int timeout)
{
    auto event = reinterpret_cast<event_cpp11_t *>(handle);
    auto lock  = std::unique_lock<std::mutex>(event->mutex);

    if (timeout < 0) {
        event->cond.wait(lock, [event] { return event->state; });
    } else {
        auto to = std::chrono::system_clock::now() + std::chrono::milliseconds(timeout);

        /* Events are manual-reset: a set latches state until the reset,
           and its notify reaches only waiters already blocked. The
           predicate form tests state before it sleeps and again on every
           wake, so a wait entered on a set event returns at once; a bare
           wait_until would sleep out the whole timeout for a notify that
           never comes. */
        if (!event->cond.wait_until(lock, to, [event] { return event->state; }))
            return 1;
    }
    return 0;
}

void
thread_set_event(event_t *handle)
{
    auto event = reinterpret_cast<event_cpp11_t *>(handle);
    {
        auto lock    = std::unique_lock<std::mutex>(event->mutex);
        event->state = true;
    }
    event->cond.notify_all();
}

void
thread_reset_event(event_t *handle)
{
    auto event   = reinterpret_cast<event_cpp11_t *>(handle);
    auto lock    = std::unique_lock<std::mutex>(event->mutex);
    event->state = false;
}

void
thread_destroy_event(event_t *handle)
{
    auto event = reinterpret_cast<event_cpp11_t *>(handle);
    delete event;
}
}
