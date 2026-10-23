// GOW-Port: host threads with the stack the runtime was sized for. The recompiled game nests host calls as deep as its
// own call chains and the VU1/GS workers keep large frames; Windows gives every thread 1 MiB, iOS gives std::thread
// 512 KiB. ps2xLargeStack(fn) wraps a thread body so that, on Apple platforms, it runs on a pthread with an 8 MiB stack
// (the std::thread then only waits for it); elsewhere it returns fn unchanged.
#pragma once
#include <utility>

#if defined(__APPLE__)
#include <pthread.h>
#include <functional>
#include <memory>

namespace ps2x
{
    inline void runWithLargeStack(std::function<void()> body)
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8u << 20);
        pthread_t thread;
        auto *heap = new std::function<void()>(std::move(body));
        const int rc = pthread_create(&thread, &attr, [](void *arg) -> void * {
            std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()> *>(arg));
            (*fn)();
            return nullptr;
        }, heap);
        pthread_attr_destroy(&attr);
        if (rc != 0)
        {
            std::unique_ptr<std::function<void()>> fn(heap);
            (*fn)(); // no thread: run on the caller's stack
            return;
        }
        pthread_join(thread, nullptr);
    }
}

template <typename F>
auto ps2xLargeStack(F &&fn)
{
    return [body = std::forward<F>(fn)]() mutable { ps2x::runWithLargeStack(std::function<void()>(std::move(body))); };
}
#else
template <typename F>
F &&ps2xLargeStack(F &&fn)
{
    return std::forward<F>(fn);
}
#endif
