/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

# ifdef __linux__
//# ifdef _WIN32

# include "hpc_aio_provider.h"
# include <fcntl.h>
# include <sys/types.h>
# include <sys/stat.h>
# include <io.h>
# include <stdio.h>
# include "mix_all_io_looper.h"

# ifdef __TITLE__
# undef __TITLE__
# endif
# define __TITLE__ "aio.provider.hpc"

namespace dsn { namespace tools {


hpc_aio_provider::hpc_aio_provider(disk_engine* disk, aio_provider* inner_provider)
    : aio_provider(disk, inner_provider), _callback(this)
{
    _looper = get_io_looper(node());

    memset(&_ctx, 0, sizeof(_ctx));
    auto ret = io_setup(128, &_ctx); // 128 concurrent events
    dassert(ret == 0, "io_setup error, ret = %d", ret);

    _event_fd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);

    if (_looper)
    {
        _looper->bind_io_handle((dsn_handle_t)_event_fd, &_callback, EPOLLIN | EPOLLET);
        _event_fd_registered = true;
    }
    else
    {
        _event_fd_registered = false;
    }
}

hpc_aio_provider::~hpc_aio_provider()
{
    auto ret = io_destroy(_ctx);
    dassert(ret == 0, "io_destroy error, ret = %d", ret);

    close(_event_fd);
}

dsn_handle_t hpc_aio_provider::open(const char* file_name, int oflag, int pmode)
{
    return (dsn_handle_t)(uintptr_t)::open(file_name, oflag, pmode);
}

error_code hpc_aio_provider::close(dsn_handle_t hFile)
{
    // TODO: handle failure
    ::close((int)(uintptr_t)(hFile));
    return ERR_OK;
}

struct linux_disk_aio_context : public disk_aio
{
    struct iocb cb;
    aio_task* tsk;
    hpc_aio_provider* this_;
    utils::notify_event* evt;
    error_code err;
    uint32_t bytes;
};

disk_aio* hpc_aio_provider::prepare_aio_context(aio_task* tsk)
{
    auto r = new linux_disk_aio_context;
    bzero((char*)&r->cb, sizeof(r->cb));
    r->tsk = tsk;
    r->evt = nullptr;
    return r;
}

void hpc_aio_provider::aio(aio_task* aio_tsk)
{
    auto err = aio_internal(aio_tsk, true);
    err.end_tracking();
}

error_code hpc_aio_provider::aio_internal(aio_task* aio_tsk, bool async, __out_param uint32_t* pbytes /*= nullptr*/)
{
    struct iocb *cbs[1];
    linux_disk_aio_context * aio;
    int ret;

    if (!_event_fd_registered)
    {
        get_looper()->bind_io_handle((dsn_handle_t)_event_fd, &_callback, EPOLLIN | EPOLLET);
        _event_fd_registered = true;
    }

    aio = (linux_disk_aio_context *)aio_tsk->aio();

    memset(&aio->cb, 0, sizeof(aio->cb));

    aio->this_ = this;

    switch (aio->type)
    {
    case AIO_Read:
        io_prep_pread(&aio->cb, static_cast<int>((ssize_t)aio->file), aio->buffer, aio->buffer_size, aio->file_offset);
        break;
    case AIO_Write:
        io_prep_pwrite(&aio->cb, static_cast<int>((ssize_t)aio->file), aio->buffer, aio->buffer_size, aio->file_offset);
        break;
    default:
        derror("unknown aio type %u", static_cast<int>(aio->type));
    }

    if (!async)
    {
        aio->evt = new utils::notify_event();
        aio->err = ERR_OK;
        aio->bytes = 0;
    }

    cbs[0] = &aio->cb;

    io_set_eventfd(&aio->cb, _event_fd);
    ret = io_submit(_ctx, 1, cbs);

    if (ret != 1)
    {
        if (ret < 0)
            derror("io_submit error, ret = %d", ret);
        else
            derror("could not sumbit IOs, ret = %d", ret);

        if (async)
        {
            complete_io(aio_tsk, ERR_FILE_OPERATION_FAILED, 0);
        }
        else
        {
            delete aio->evt;
            aio->evt = nullptr;
        }
        return ERR_FILE_OPERATION_FAILED;
    }
    else
    {
        if (async)
        {
            return ERR_IO_PENDING;
        }
        else
        {
            aio->evt->wait();
            delete aio->evt;
            aio->evt = nullptr;
            *pbytes = aio->bytes;
            return aio->err;
        }
    }
}

void hpc_aio_provider::hpc_aio_io_loop_callback::handle_event(
    int native_error,
    uint32_t io_size,
    uintptr_t lolp_or_events
    )
{
    _provider->on_aio_completed((uint32_t)lolp_or_events);
}

void hpc_aio_provider::on_aio_completed(uint32_t events)
{
    int finished_aio = 0;
    if (read(_event_fd, &finished_aio, sizeof(finished_aio)) != sizeof(finished_aio))
    {
        dassert(false, "read number of aio completion from eventfd failed, err = %s",
            strerror(errno)
            );
    }


    struct io_event events[1];
    int ret;
    linux_disk_aio_context * aio;

    while (finished_aio > 0) 
    {
        struct timespec tms;
        tms.tv_sec = 0;
        tms.tv_nsec = 0;

        ret = io_getevents(_ctx, 1, 1, events, &tms);
        dassert(ret == 1, "aio must return 1 event as we already got "
            "notification from eventfd");

        struct iocb *io = events[0].obj;
        complete_aio(io, static_cast<int>(events[0].res), static_cast<int>(events[0].res2));
    }
}

void hpc_aio_provider::complete_aio(struct iocb* io, int bytes, int err)
{
    linux_disk_aio_context* aio = CONTAINING_RECORD(io, linux_disk_aio_context, cb);
    if (err != 0)
    {
        derror("aio error, err = %d", err);
    }

    if (!aio->evt)
    {
        aio_task* aio_ptr(aio->tsk);
        aio->this_->complete_io(aio_ptr, (err == 0) ? ERR_OK : ERR_FILE_OPERATION_FAILED, bytes);
    }
    else
    {
        aio->err = (err == 0) ? ERR_OK : ERR_FILE_OPERATION_FAILED;
        aio->bytes = bytes;
        aio->evt->notify();
    }
}

}} // end namespace dsn::tools
#endif