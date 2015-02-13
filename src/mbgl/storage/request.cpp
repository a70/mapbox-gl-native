#include <mbgl/storage/default/request.hpp>

#include <mbgl/storage/response.hpp>

#include <mbgl/util/util.hpp>
#include <mbgl/util/uv.hpp>

#include <uv.h>

#include <cassert>
#include <functional>

namespace mbgl {

// Note: This requires that loop is running in the current thread (or not yet running).
Request::Request(const Resource &resource_, uv_loop_t *loop, Callback callback_)
    : callback(callback_), resource(resource_) {
    // When there is no loop supplied (== nullptr), the callback will be fired in an arbitrary
    // thread (the thread notify() is called from) rather than kicking back to the calling thread.
    if (loop) {
        notify_async = new uv_async_t;
        notify_async->data = nullptr;
#if UV_VERSION_MAJOR == 0 && UV_VERSION_MINOR <= 10
        uv_async_init(loop, notify_async, [](uv_async_t *async, int) { notifyCallback(async); });
#else
        uv_async_init(loop, notify_async, notifyCallback);
#endif
    }
}

void Request::notifyCallback(uv_async_t *async) {
    auto request = reinterpret_cast<Request *>(async->data);
    uv::close(async);
    assert(request);
    MBGL_VERIFY_THREAD(request->tid)

    if (!request->destruct_async) {
        // Call the callback with the result data. This will also delete this object. We haven't
        // created a cancel request, so this is safe since it won't be accessed in the future.
        // It is up to the user to not call cancel() on this Request object after the response was
        // delivered.
        request->invoke();
    } else {
        // Otherwise, we're waiting for for the destruct notification to be delivered in order
        // to delete the Request object. We're doing this since we can't know whether the
        // DefaultFileSource is still sending a cancel event, which means this object must still
        // exist.
    }
}

void Request::invoke() {
    assert(response);
    // The user could supply a null pointer or empty std::function as a callback. In this case, we
    // still do the file request, but we don't need to deliver a result.
    if (callback) {
        callback(*response);
    }
    delete this;
}

Request::~Request() {
}

void Request::notify(const std::shared_ptr<const Response> &response_) {
    response = response_;
    assert(response);
    if (notify_async) {
        assert(!notify_async->data);
        notify_async->data = this;
        uv_async_send(notify_async);
    } else {
        // This request is not cancelable. This means that the callback will be executed in an
        // arbitrary thread (== FileSource thread).
        invoke();
    }
}

void Request::cancel() {
    MBGL_VERIFY_THREAD(tid)
    assert(notify_async);
    assert(!destruct_async);
    destruct_async = new uv_async_t;
    destruct_async->data = nullptr;
#if UV_VERSION_MAJOR == 0 && UV_VERSION_MINOR <= 10
    uv_async_init(notify_async->loop, destruct_async, [](uv_async_t *async, int) { cancelCallback(async); });
#else
    uv_async_init(notify_async->loop, destruct_async, cancelCallback);
#endif
}

void Request::cancelCallback(uv_async_t *async) {
    // The destruct_async will be invoked *after* the notify_async callback has already run.
    auto request = reinterpret_cast<Request *>(async->data);
    uv::close(async);
    assert(request);
    MBGL_VERIFY_THREAD(request->tid)
    delete request;
}

// This gets called from the FileSource thread, and will only ever be invoked after cancel() was called
// in the original requesting thread.
void Request::destruct() {
    assert(notify_async);
    assert(destruct_async);

    if (!notify_async->data) {
        // The async hasn't been triggered yet, but we need to so that it'll close the handle. The
        // callback will not delete this object since we have a destruct_async handle as well.
        notify_async->data = this;
        uv_async_send(notify_async);
    }

    // This will finally destruct this object.
    assert(!destruct_async->data);
    destruct_async->data = this;
    uv_async_send(destruct_async);
}

}
