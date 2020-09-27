#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <pthread.h>
#include "fastcommon/logger.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/sched_thread.h"
#include "sf/sf_nio.h"
#include "sf/sf_global.h"
#include "push_result_ring.h"

int push_result_ring_check_init(FDIRBinlogPushResultContext *ctx,
        const int alloc_size)
{
    int bytes;

    if (ctx->ring.entries != NULL) {
        return 0;
    }

    bytes = sizeof(FDIRBinlogPushResultEntry) * alloc_size;
    ctx->ring.entries = (FDIRBinlogPushResultEntry *)fc_malloc(bytes);
    if (ctx->ring.entries == NULL) {
        return ENOMEM;
    }
    memset(ctx->ring.entries, 0, bytes);

    ctx->ring.start = ctx->ring.end = ctx->ring.entries;
    ctx->ring.size = alloc_size;

    ctx->queue.head = ctx->queue.tail = NULL;
    return fast_mblock_init_ex1(&ctx->queue.rentry_allocator,
        "push_result", sizeof(FDIRBinlogPushResultEntry), 4096,
        0, NULL, NULL, false);
}

static inline void desc_task_waiting_rpc_count(
        FDIRBinlogPushResultEntry *entry)
{
    FDIRServerTaskArg *task_arg;

    if (entry->waiting_task == NULL) {
        return;
    }

    task_arg = (FDIRServerTaskArg *)entry->waiting_task->arg;
    if (entry->task_version != __sync_add_and_fetch(
                &task_arg->task_version, 0))
    {
        logWarning("file: "__FILE__", line: %d, "
                "task %p already cleanup",
                __LINE__, entry->waiting_task);
        return;
    }

    if (__sync_sub_and_fetch(&((FDIRServerTaskArg *)
                    entry->waiting_task->arg)->context.
                service.waiting_rpc_count, 1) == 0)
    {
        sf_nio_notify(entry->waiting_task, SF_NIO_STAGE_CONTINUE);
    }
}

static void push_result_ring_clear_queue_all(FDIRBinlogPushResultContext *ctx)
{
    FDIRBinlogPushResultEntry *current;
    FDIRBinlogPushResultEntry *deleted;

    if (ctx->queue.head == NULL) {
        return;
    }

    current = ctx->queue.head;
    while (current != NULL) {
        deleted = current;
        current = current->next;

        desc_task_waiting_rpc_count(deleted);
        fast_mblock_free_object(&ctx->queue.rentry_allocator, deleted);
    }

    ctx->queue.head = ctx->queue.tail = NULL;
}

void push_result_ring_clear_all(FDIRBinlogPushResultContext *ctx)
{
    int index;

    if (ctx->ring.start == ctx->ring.end) {
        push_result_ring_clear_queue_all(ctx);
        return;
    }

    index = ctx->ring.start - ctx->ring.entries;
    while (ctx->ring.start != ctx->ring.end) {
        desc_task_waiting_rpc_count(ctx->ring.start);
        ctx->ring.start->data_version = 0;
        ctx->ring.start->waiting_task = NULL;

        ctx->ring.start = ctx->ring.entries +
            (++index % ctx->ring.size);
    }

    push_result_ring_clear_queue_all(ctx);
}

static int  push_result_ring_clear_queue_timeouts(
        FDIRBinlogPushResultContext *ctx)
{
    FDIRBinlogPushResultEntry *current;
    FDIRBinlogPushResultEntry *deleted;
    int count;

    if (ctx->queue.head == NULL) {
        return 0;
    }

    if (ctx->queue.head->expires >= g_current_time) {
        return 0;
    }

    count = 0;
    current = ctx->queue.head;
    while (current != NULL && current->expires < g_current_time) {
        deleted = current;
        current = current->next;

        logWarning("file: "__FILE__", line: %d, "
                "waiting push response timeout, "
                "data_version: %"PRId64", task: %p",
                __LINE__, deleted->data_version,
                deleted->waiting_task);
        desc_task_waiting_rpc_count(deleted);
        fast_mblock_free_object(&ctx->queue.rentry_allocator, deleted);
        ++count;
    }

    ctx->queue.head = current;
    if (current == NULL) {
        ctx->queue.tail = NULL;
    }

    return count;
}

void push_result_ring_clear_timeouts(FDIRBinlogPushResultContext *ctx)
{
    int index;
    int clear_count;

    if (ctx->last_check_timeout_time == g_current_time) {
        return;
    }

    clear_count = 0;
    ctx->last_check_timeout_time = g_current_time;
    if (ctx->ring.start != ctx->ring.end) {
        index = ctx->ring.start - ctx->ring.entries;
        while (ctx->ring.start != ctx->ring.end &&
                ctx->ring.start->expires < g_current_time)
        {
            logWarning("file: "__FILE__", line: %d, "
                    "waiting push response from server %s:%u timeout, "
                    "data_version: %"PRId64"", __LINE__,
                    (ctx->ring.start->waiting_task != NULL ?
                    ctx->ring.start->waiting_task->server_ip : ""),
                    (ctx->ring.start->waiting_task != NULL ?
                    ctx->ring.start->waiting_task->port : 0),
                    ctx->ring.start->data_version);

            desc_task_waiting_rpc_count(ctx->ring.start);
            ctx->ring.start->data_version = 0;
            ctx->ring.start->waiting_task = NULL;

            ctx->ring.start = ctx->ring.entries +
                (++index % ctx->ring.size);
            ++clear_count;
        }
    }

    clear_count += push_result_ring_clear_queue_timeouts(ctx);
    if (clear_count > 0) {
        logWarning("file: "__FILE__", line: %d, "
                "clear timeout push response waiting entries count: %d",
                __LINE__, clear_count);
    }
}

void push_result_ring_destroy(FDIRBinlogPushResultContext *ctx)
{
    if (ctx->ring.entries != NULL) {
        free(ctx->ring.entries);
        ctx->ring.start = ctx->ring.end = ctx->ring.entries = NULL;
        ctx->ring.size = 0;
    }

    fast_mblock_destroy(&ctx->queue.rentry_allocator);
}

static int add_to_queue(FDIRBinlogPushResultContext *ctx,
            const uint64_t data_version, struct fast_task_info *waiting_task,
            const int64_t task_version)
{
    FDIRBinlogPushResultEntry *entry;
    FDIRBinlogPushResultEntry *previous;
    FDIRBinlogPushResultEntry *current;

    entry = (FDIRBinlogPushResultEntry *)fast_mblock_alloc_object(
            &ctx->queue.rentry_allocator);
    if (entry == NULL) {
        return ENOMEM;
    }

    entry->data_version = data_version;
    entry->waiting_task = waiting_task;
    entry->task_version = task_version;
    entry->expires = g_current_time + SF_G_NETWORK_TIMEOUT;

    if (ctx->queue.tail == NULL) {  //empty queue
        entry->next = NULL;
        ctx->queue.head = ctx->queue.tail = entry;
        return 0;
    }

    if (data_version > ctx->queue.tail->data_version) {
        entry->next = NULL;
        ctx->queue.tail->next = entry;
        ctx->queue.tail = entry;
        return 0;
    }

    if (data_version < ctx->queue.head->data_version) {
        entry->next = ctx->queue.head;
        ctx->queue.head = entry;
        return 0;
    }

    previous = ctx->queue.head;
    current = ctx->queue.head->next;
    while (current != NULL && data_version > current->data_version) {
        previous = current;
        current = current->next;
    }

    entry->next = previous->next;
    previous->next = entry;
    return 0;
}

int push_result_ring_add(FDIRBinlogPushResultContext *ctx,
        const uint64_t data_version, struct fast_task_info *waiting_task,
        const int64_t task_version)
{
    FDIRBinlogPushResultEntry *entry;
    FDIRBinlogPushResultEntry *previous;
    FDIRBinlogPushResultEntry *next;
    int index;
    bool matched;

    matched = false;
    index = data_version % ctx->ring.size;
    entry = ctx->ring.entries + index;
    if (ctx->ring.end == ctx->ring.start) {  //empty
        ctx->ring.start = entry;
        ctx->ring.end = ctx->ring.entries + (index + 1) % ctx->ring.size;
        matched = true;
    } else if (entry == ctx->ring.end) {
        previous = ctx->ring.entries + (index + ctx->ring.size - 1) %
            ctx->ring.size;
        next = ctx->ring.entries + (index + 1) % ctx->ring.size;
        if ((next != ctx->ring.start) &&
                data_version == previous->data_version + 1)
        {
            ctx->ring.end = next;
            matched = true;
        }
    }

    if (matched) {
        entry->data_version = data_version;
        entry->waiting_task = waiting_task;
        entry->task_version = task_version;
        entry->expires = g_current_time + SF_G_NETWORK_TIMEOUT;
        return 0;
    }

    logWarning("file: "__FILE__", line: %d, "
            "can't found data version %"PRId64", in the ring",
            __LINE__, data_version);
    return add_to_queue(ctx, data_version, waiting_task, task_version);
}

static int remove_from_queue(FDIRBinlogPushResultContext *ctx,
        const uint64_t data_version)
{
    FDIRBinlogPushResultEntry *entry;
    FDIRBinlogPushResultEntry *previous;
    FDIRBinlogPushResultEntry *current;

    if (ctx->queue.head == NULL) {  //empty queue
        return ENOENT;
    }

    if (data_version == ctx->queue.head->data_version) {
        entry = ctx->queue.head;
        ctx->queue.head = entry->next;
        if (ctx->queue.head == NULL) {
            ctx->queue.tail = NULL;
        }
    } else {
        previous = ctx->queue.head;
        current = ctx->queue.head->next;
        while (current != NULL && data_version > current->data_version) {
            previous = current;
            current = current->next;
        }

        if (current == NULL || data_version != current->data_version) {
            return ENOENT;
        }

        entry = current;
        previous->next = current->next;
        if (ctx->queue.tail == current) {
            ctx->queue.tail = previous;
        }
    }

    desc_task_waiting_rpc_count(entry);
    fast_mblock_free_object(&ctx->queue.rentry_allocator, entry);
    return 0;
}

int push_result_ring_remove(FDIRBinlogPushResultContext *ctx,
        const uint64_t data_version)
{
    FDIRBinlogPushResultEntry *entry;
    int index;

    if (ctx->ring.end != ctx->ring.start) {
        index = data_version % ctx->ring.size;
        entry = ctx->ring.entries + index;

        if (entry->data_version == data_version) {
            if (ctx->ring.start == entry) {
                ctx->ring.start = ctx->ring.entries +
                    (++index % ctx->ring.size);
                while (ctx->ring.start != ctx->ring.end &&
                        ctx->ring.start->data_version == 0)
                {
                    ctx->ring.start = ctx->ring.entries +
                        (++index % ctx->ring.size);
                }
            }

            desc_task_waiting_rpc_count(entry);
            entry->data_version = 0;
            entry->waiting_task = NULL;
            return 0;
        }
    }

    return remove_from_queue(ctx, data_version);
}
