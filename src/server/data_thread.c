/*
 * Copyright (c) 2020 YuQing <384681@qq.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

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
#include "fastcommon/sockopt.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/sched_thread.h"
#include "fastcommon/pthread_func.h"
#include "sf/sf_global.h"
#include "sf/sf_func.h"
#include "dentry.h"
#include "inode_index.h"
#include "service_handler.h"
#include "db/change_notify.h"
#include "db/dentry_serializer.h"
#include "data_thread.h"

#define DATA_THREAD_RUNNING_COUNT g_data_thread_vars.running_count

FDIRDataThreadVariables g_data_thread_vars = {{NULL, 0}, 0, 0};
static void *data_thread_func(void *arg);

void data_thread_sum_counters(FDIRDentryCounters *counters)
{
    FDIRDataThreadContext *context;
    FDIRDataThreadContext *end;

    counters->ns = 0;
    counters->dir = 0;
    counters->file = 0;
    end = g_data_thread_vars.thread_array.contexts +
        g_data_thread_vars.thread_array.count;
    for (context=g_data_thread_vars.thread_array.contexts;
            context<end; context++)
    {
        counters->ns += context->dentry_context.counters.ns;
        counters->dir += context->dentry_context.counters.dir;
        counters->file += context->dentry_context.counters.file;
    }
}

static inline void add_to_delay_free_queue(ServerDelayFreeContext *dfctx,
        ServerDelayFreeNode *node, const int delay_seconds)
{
    node->expires = g_current_time + delay_seconds;
    node->next = NULL;
    if (dfctx->queue.head == NULL) {
        dfctx->queue.head = node;
    } else {
        dfctx->queue.tail->next = node;
    }
    dfctx->queue.tail = node;
}

int server_add_to_delay_free_queue(ServerFreeContext *free_ctx, void *ptr,
        server_free_func free_func, const int delay_seconds)
{
    ServerDelayFreeNode *node;

    node = (ServerDelayFreeNode *)fast_mblock_alloc_object(
            &free_ctx->allocator);
    if (node == NULL) {
        return ENOMEM;
    }

    node->free_func = free_func;
    node->free_func_ex = NULL;
    node->ctx = NULL;
    node->ptr = ptr;
    add_to_delay_free_queue(&free_ctx->delay, node, delay_seconds);
    return 0;
}

int server_add_to_delay_free_queue_ex(ServerFreeContext *free_ctx,
        void *ctx, void *ptr, server_free_func_ex free_func_ex,
        const int delay_seconds)
{
    ServerDelayFreeNode *node;

    node = (ServerDelayFreeNode *)fast_mblock_alloc_object(
            &free_ctx->allocator);
    if (node == NULL) {
        return ENOMEM;
    }

    node->free_func = NULL;
    node->free_func_ex = free_func_ex;
    node->ctx = ctx;
    node->ptr = ptr;
    add_to_delay_free_queue(&free_ctx->delay, node, delay_seconds);
    return 0;
}

int server_add_to_immediate_free_queue_ex(ServerFreeContext *free_ctx,
        void *ctx, void *ptr, server_free_func_ex free_func_ex)
{
    ServerDelayFreeNode *node;

    node = (ServerDelayFreeNode *)fast_mblock_alloc_object(
            &free_ctx->allocator);
    if (node == NULL) {
        return ENOMEM;
    }

    node->free_func = NULL;
    node->free_func_ex = free_func_ex;
    node->ctx = ctx;
    node->ptr = ptr;
    __sync_add_and_fetch(&free_ctx->immediate.waiting_count, 1);
    fc_queue_push_silence(&free_ctx->immediate.queue, node);
    return 0;
}

int server_add_to_immediate_free_queue(ServerFreeContext *free_ctx,
        void *ptr, server_free_func free_func)
{
    ServerDelayFreeNode *node;

    node = (ServerDelayFreeNode *)fast_mblock_alloc_object(
            &free_ctx->allocator);
    if (node == NULL) {
        return ENOMEM;
    }

    node->free_func = free_func;
    node->free_func_ex = NULL;
    node->ctx = NULL;
    node->ptr = ptr;
    __sync_add_and_fetch(&free_ctx->immediate.waiting_count, 1);
    fc_queue_push_silence(&free_ctx->immediate.queue, node);
    return 0;
}

static void deal_delay_free_queue(FDIRDataThreadContext *thread_ctx)
{
    ServerDelayFreeContext *delay_context;
    ServerDelayFreeNode *node;
    ServerDelayFreeNode *tail;
    struct fast_mblock_chain chain;

    delay_context = &thread_ctx->free_context.delay;
    if (delay_context->last_check_time == g_current_time ||
            delay_context->queue.head == NULL)
    {
        return;
    }

    tail = NULL;
    delay_context->last_check_time = g_current_time;
    node = delay_context->queue.head;
    while ((node != NULL) && (node->expires < g_current_time)) {
        if (node->free_func != NULL) {
            node->free_func(node->ptr);
        } else {
            node->free_func_ex(node->ctx, node->ptr);
        }

        tail = node;
        node = node->next;
    }

    if (tail == NULL) {
        return;
    }

    tail->next = NULL;
    chain.head = fast_mblock_to_node_ptr(delay_context->queue.head);
    chain.tail = fast_mblock_to_node_ptr(tail);
    fast_mblock_batch_free(&thread_ctx->free_context.allocator, &chain);

    delay_context->queue.head = node;
    if (node == NULL) {
        delay_context->queue.tail = NULL;
    }
}

static void deal_immediate_free_queue(FDIRDataThreadContext *thread_ctx)
{
    struct fc_queue_info qinfo;
    struct fast_mblock_chain chain;
    ServerDelayFreeNode *node;
    int count;

    fc_queue_try_pop_to_queue(&thread_ctx->free_context.
            immediate.queue, &qinfo);
    if (qinfo.head == NULL) {
        return;
    }

    count = 0;
    node = qinfo.head;
    do {
        if (node->free_func != NULL) {
            node->free_func(node->ptr);
        } else {
            node->free_func_ex(node->ctx, node->ptr);
        }

        ++count;
        node = node->next;
    } while (node != NULL);

    chain.head = fast_mblock_to_node_ptr(qinfo.head);
    chain.tail = fast_mblock_to_node_ptr(qinfo.tail);
    fast_mblock_batch_free(&thread_ctx->free_context.allocator, &chain);

    logInfo("file: "__FILE__", line: %d, "
            "free count: %d, free_context.immediate.waiting_count: %d",
            __LINE__, count,  __sync_add_and_fetch(&thread_ctx->
                free_context.immediate.waiting_count, 0));

    __sync_sub_and_fetch(&thread_ctx->free_context.
            immediate.waiting_count, count);
}

static int init_thread_ctx(FDIRDataThreadContext *context)
{
    int result;
    if ((result=dentry_init_context(context)) != 0) {
        return result;
    }

    if ((result=fast_mblock_init_ex1(&context->free_context.allocator,
                    "delay_free_node", sizeof(ServerDelayFreeNode),
                    16 * 1024, 0, NULL, NULL, true)) != 0)
    {
        return result;
    }

    if ((result=fc_queue_init(&context->free_context.immediate.queue,
                    (long)(&((ServerDelayFreeNode *)NULL)->next))) != 0)
    {
        return result;
    }

    if ((result=fc_queue_init(&context->queue, (long)
                    (&((FDIRBinlogRecord *)NULL)->next))) != 0)
    {
        return result;
    }

    if (STORAGE_ENABLED) {
        if ((result=init_db_fetch_context(&context->db_fetch_ctx)) != 0) {
            return result;
        }
    }

    return 0;
}

static int init_data_thread_array()
{
    int result;
    int bytes;
    FDIRDataThreadContext *context;
    FDIRDataThreadContext *end;

    bytes = sizeof(FDIRDataThreadContext) * DATA_THREAD_COUNT;
    g_data_thread_vars.thread_array.contexts =
        (FDIRDataThreadContext *)fc_malloc(bytes);
    if (g_data_thread_vars.thread_array.contexts == NULL) {
        return ENOMEM;
    }
    memset(g_data_thread_vars.thread_array.contexts, 0, bytes);

    end = g_data_thread_vars.thread_array.contexts + DATA_THREAD_COUNT;
    for (context=g_data_thread_vars.thread_array.contexts;
            context<end; context++)
    {
        context->index = context - g_data_thread_vars.thread_array.contexts;
        if ((result=init_thread_ctx(context)) != 0) {
            return result;
        }
    }
    g_data_thread_vars.thread_array.count = DATA_THREAD_COUNT;
    return 0;
}

int data_thread_init()
{
    int alloc_elements_once;
    int64_t alloc_elements_limit;
    int result;
    int count;

    if (STORAGE_ENABLED) {
        if (BATCH_STORE_ON_MODIFIES < 1000) {
            alloc_elements_once = 1 * 1024;
            alloc_elements_limit = 8 * 1024;
        } else if (BATCH_STORE_ON_MODIFIES < 10 * 1000) {
            alloc_elements_once = 2 * 1024;
            alloc_elements_limit = BATCH_STORE_ON_MODIFIES * 8;
        } else if (BATCH_STORE_ON_MODIFIES < 100 * 1000) {
            alloc_elements_once = 4 * 1024;
            alloc_elements_limit = BATCH_STORE_ON_MODIFIES * 4;
        } else {
            alloc_elements_once = 8 * 1024;
            if (BATCH_STORE_ON_MODIFIES < 1000 * 1000) {
                alloc_elements_limit = BATCH_STORE_ON_MODIFIES * 2;
            } else {
                alloc_elements_limit = BATCH_STORE_ON_MODIFIES;
            }
        }

        if ((result=fast_mblock_init_ex1(&NOTIFY_EVENT_ALLOCATOR,
                        "chg-event", sizeof(FDIRChangeNotifyEvent),
                        alloc_elements_once, alloc_elements_limit,
                        NULL, NULL, true)) != 0)
        {
            return result;
        }
        fast_mblock_set_need_wait(&NOTIFY_EVENT_ALLOCATOR,
                true, (bool *)&SF_G_CONTINUE_FLAG);
    }

    if ((result=init_data_thread_array()) != 0) {
        return result;
    }

    g_data_thread_vars.error_mode = FDIR_DATA_ERROR_MODE_LOOSE;
    count = g_data_thread_vars.thread_array.count;
    if ((result=create_work_threads_ex(&count, data_thread_func,
            g_data_thread_vars.thread_array.contexts,
            sizeof(FDIRDataThreadContext), NULL,
            SF_G_THREAD_STACK_SIZE)) == 0)
    {
        count = 0;
        while (__sync_add_and_fetch(&DATA_THREAD_RUNNING_COUNT, 0) <
                g_data_thread_vars.thread_array.count && count++ < 100)
        {
            fc_sleep_ms(1);
        }
    }
    return result;
}

void data_thread_destroy()
{
    if (g_data_thread_vars.thread_array.contexts != NULL) {
        FDIRDataThreadContext *context;
        FDIRDataThreadContext *end;

        end = g_data_thread_vars.thread_array.contexts +
            g_data_thread_vars.thread_array.count;
        for (context=g_data_thread_vars.thread_array.contexts;
                context<end; context++)
        {
            fc_queue_destroy(&context->queue);
        }
        free(g_data_thread_vars.thread_array.contexts);
        g_data_thread_vars.thread_array.contexts = NULL;
    }
}

void data_thread_terminate()
{
    FDIRDataThreadContext *context;
    FDIRDataThreadContext *end;
    int count;

    end = g_data_thread_vars.thread_array.contexts +
        g_data_thread_vars.thread_array.count;
    for (context=g_data_thread_vars.thread_array.contexts;
            context<end; context++)
    {
        fc_queue_terminate(&context->queue);
    }

    count = 0;
    while (__sync_add_and_fetch(&DATA_THREAD_RUNNING_COUNT, 0) != 0 &&
            count++ < 100)
    {
        fc_sleep_ms(1);
    }
}

static inline int check_parent(FDIRBinlogRecord *record)
{
    if (record->me.pname.parent_inode == 0) {
        return 0;
    }

    record->me.parent = inode_index_get_dentry(record->
            me.pname.parent_inode);
    return record->me.parent != NULL ? 0 : ENOENT;
}

static int find_or_check_parent(FDIRDataThreadContext *thread_ctx,
        FDIRBinlogRecord *record)
{
    int result;
    bool is_create;

    if (record->dentry_type == fdir_dentry_type_pname) {
        return check_parent(record);
    }

    /*
    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ns: %.*s, path: %.*s", __LINE__, __FUNCTION__,
            fullname.ns.len, fullname.ns.str,
            fullname.path.len, fullname.path.str);
            */

    is_create = (record->operation == BINLOG_OP_CREATE_DENTRY_INT);
    if ((result=dentry_find_parent(&record->me.fullname, &record->
                    me.parent, &record->me.pname.name)) != 0)
    {
        if (!(result == ENOENT && is_create)) {
            return result;
        }
        if (!FDIR_IS_ROOT_PATH(record->me.fullname.path)) {
            return result;
        }
    } else if (is_create && FDIR_IS_ROOT_PATH(record->me.fullname.path)) {
        return EEXIST;
    }

    record->me.pname.parent_inode = (record->me.parent != NULL ?
            record->me.parent->inode : 0);
    record->me.dentry = NULL;
    return service_set_record_pname_info(record,
            (struct fast_task_info *)record->notify.args);
}

static inline int set_pname_by_fullname_ex(FDIRRecordDEntry *entry,
        const bool allow_root_path)
{
    int result;

    if ((result=dentry_find_parent(&entry->fullname, &entry->parent,
                    &entry->pname.name)) != 0)
    {
        return result;
    }

    if (allow_root_path) {
        entry->pname.parent_inode = (entry->parent != NULL ?
                entry->parent->inode : 0);
    } else {
        if (entry->parent == NULL) {
            return EINVAL;
        }
        entry->pname.parent_inode = entry->parent->inode;
    }

    return 0;
}

#define set_pname_by_fullname(entry) set_pname_by_fullname_ex(entry, false)

static inline int set_hdlink_src_dentry(FDIRBinlogRecord *record)
{
    int result;

    if (record->dentry_type == fdir_dentry_type_pname) {
        if ((record->hdlink.src.dentry=inode_index_get_dentry(
                        record->hdlink.src.inode)) == NULL)
        {
            return ENOENT;
        }
    } else {
        if ((result=dentry_find(&record->hdlink.src.fullname,
                        &record->hdlink.src.dentry)) != 0)
        {
            return result;
        }
        record->hdlink.src.inode = record->hdlink.src.dentry->inode;
    }

    if (S_ISDIR(record->hdlink.src.dentry->stat.mode) ||
            FDIR_IS_DENTRY_HARD_LINK(record->hdlink.src.dentry->stat.mode))
    {
        return EPERM;
    }

    record->stat.mode |= (record->hdlink.src.dentry->stat.mode & S_IFMT);
    return 0;
}

static inline int deal_record_rename_op(FDIRDataThreadContext *thread_ctx,
        FDIRBinlogRecord *record)
{
    int result;
    char *src_name;

    if (record->dentry_type == fdir_dentry_type_pname) {
        if ((record->rename.src.parent=inode_index_get_dentry(record->
                        rename.src.pname.parent_inode)) == NULL)
        {
            return ENOENT;
        }

        if ((record->rename.dest.parent=inode_index_get_dentry(record->
                        rename.dest.pname.parent_inode)) == NULL)
        {
            return ENOENT;
        }
    } else {
        if ((result=set_pname_by_fullname(&record->rename.src)) != 0) {
            return result;
        }
        if ((result=set_pname_by_fullname(&record->rename.dest)) != 0) {
            return result;
        }

        if ((result=service_set_record_pname_info(record,
                        (struct fast_task_info *)record->notify.args)) != 0)
        {
            return result;
        }
    }

    src_name = record->rename.dest.pname.name.str +
        record->rename.dest.pname.name.len;
    memcpy(src_name, record->rename.src.pname.name.str,
            record->rename.src.pname.name.len);
    record->rename.src.pname.name.str = src_name;

    return dentry_rename(thread_ctx, record);
}

static inline int xattr_update_prepare(FDIRDataThreadContext *thread_ctx,
        FDIRBinlogRecord *record)
{
    int result;

    if (record->dentry_type == fdir_dentry_type_inode) {
        return 0;
    }

    if ((result=dentry_find(&record->me.fullname, &record->me.dentry)) != 0) {
        return result;
    }

    record->inode = record->me.dentry->inode;
    record->me.pname.name = record->me.dentry->name;
    record->me.pname.parent_inode = (record->me.dentry->parent != NULL ?
            record->me.dentry->parent->inode : 0);
    return 0;
}

#define GENERATE_ADD_TO_PARENT_MESSAGE(msg, dentry, op_type)  \
    if ((dentry)->parent != NULL) {  \
        FDIR_CHANGE_NOTIFY_FILL_MESSAGE(msg, (dentry)->parent, \
                op_type, FDIR_PIECE_FIELD_INDEX_CHILDREN, 0);  \
        (msg)->child.id = (dentry)->inode;  \
        if ((result=dentry_strdup((dentry)->context, &(msg)-> \
                        child.name, &(dentry)->name)) != 0) \
        {  \
            return result; \
        }  \
        (msg)++; \
    }

#define GENERATE_REMOVE_FROM_PARENT_MESSAGE(msg, parent, inode)  \
    if (parent != NULL) {  \
        FDIR_CHANGE_NOTIFY_FILL_MESSAGE(msg, parent, \
                da_binlog_op_type_remove, \
                FDIR_PIECE_FIELD_INDEX_CHILDREN, 0); \
        (msg)->child.id = inode;  \
        FC_SET_STRING_NULL((msg)->child.name);  \
        (msg)++; \
    }

#define GENERATE_REOMVE_DENTRY_MESSAGES(msg, dentry) \
    GENERATE_REMOVE_FROM_PARENT_MESSAGE(msg,    \
            (dentry)->parent, (dentry)->inode); \
    FDIR_CHANGE_NOTIFY_FILL_MSG_AND_INC_PTR(msg, dentry, \
            da_binlog_op_type_remove, FDIR_PIECE_FIELD_INDEX_FOR_REMOVE, \
            ((dentry)->stat.alloc > 0 ? -1 * (dentry)->stat.alloc : 0))

#define GENERATE_DENTRY_MESSAGES(msg, dentry, op_type) \
    GENERATE_ADD_TO_PARENT_MESSAGE(msg, dentry, da_binlog_op_type_create); \
    FDIR_CHANGE_NOTIFY_FILL_MSG_AND_INC_PTR(msg, dentry, \
            op_type, FDIR_PIECE_FIELD_INDEX_BASIC, 0)


#define GENERATE_MOVE_DENTRY_MESSAGES(msg, old_parent, dentry)  \
        GENERATE_REMOVE_FROM_PARENT_MESSAGE(msg, old_parent, (dentry)->inode); \
        GENERATE_DENTRY_MESSAGES(msg, dentry, da_binlog_op_type_update)

static void generate_affected_messages(FDIRChangeNotifyMessage **msg,
        FDIRBinlogRecord *record)
{
    FDIRAffectedDentry *current;
    FDIRAffectedDentry *end;

    end = record->affected.entries + record->affected.count;
    for (current=record->affected.entries; current<end; current++) {
        if (current->op_type == da_binlog_op_type_remove) {
            GENERATE_REOMVE_DENTRY_MESSAGES(*msg, current->dentry);
        } else {  //update
            FDIR_CHANGE_NOTIFY_FILL_MSG_AND_INC_PTR(*msg,
                    current->dentry, da_binlog_op_type_update,
                    FDIR_PIECE_FIELD_INDEX_BASIC, 0);
        }
    }
}

static void generate_remove_messages(FDIRChangeNotifyMessage **msg,
        FDIRBinlogRecord *record)
{
    FDIRAffectedDentry *current;
    FDIRAffectedDentry *end;
    bool removed;

    removed = false;
    end = record->affected.entries + record->affected.count;
    for (current=record->affected.entries; current<end; current++) {
        if (current->op_type == da_binlog_op_type_remove) {
            if (current->dentry == record->me.dentry) {
                removed = true;
            }
            GENERATE_REOMVE_DENTRY_MESSAGES(*msg, current->dentry);
        } else {  //update
            FDIR_CHANGE_NOTIFY_FILL_MSG_AND_INC_PTR(*msg,
                    current->dentry, da_binlog_op_type_update,
                    FDIR_PIECE_FIELD_INDEX_BASIC, 0);
        }
    }

    if (!removed) {
        GENERATE_REMOVE_FROM_PARENT_MESSAGE(*msg, record->me.
                dentry->parent, record->me.dentry->inode);
    }
}

static int generate_rename_messages(FDIRChangeNotifyMessage **msg,
        FDIRBinlogRecord *record)
{
    int result;

    if ((record->flags & RENAME_EXCHANGE)) {
        if (record->rename.src.parent == record->rename.dest.parent) {
            GENERATE_ADD_TO_PARENT_MESSAGE(*msg, record->rename.src.dentry,
                    da_binlog_op_type_update);
            GENERATE_ADD_TO_PARENT_MESSAGE(*msg, record->rename.dest.dentry,
                    da_binlog_op_type_update);

            FDIR_CHANGE_NOTIFY_FILL_MSG_AND_INC_PTR(*msg, record->
                    rename.src.dentry, da_binlog_op_type_update,
                    FDIR_PIECE_FIELD_INDEX_BASIC, 0);
            FDIR_CHANGE_NOTIFY_FILL_MSG_AND_INC_PTR(*msg, record->
                    rename.dest.dentry, da_binlog_op_type_update,
                    FDIR_PIECE_FIELD_INDEX_BASIC, 0);
        } else {
            GENERATE_MOVE_DENTRY_MESSAGES(*msg, record->rename.
                    src.parent, record->rename.src.dentry);
            GENERATE_MOVE_DENTRY_MESSAGES(*msg, record->rename.
                    dest.parent, record->rename.dest.dentry);
        }
        return 0;
    }

    if (record->affected.count > 0) {
        generate_affected_messages(msg, record);
    }
    if (record->rename.src.dentry->parent ==
            record->rename.src.parent)
    {
        GENERATE_ADD_TO_PARENT_MESSAGE(*msg, record->rename.src.dentry,
                da_binlog_op_type_update);
        FDIR_CHANGE_NOTIFY_FILL_MSG_AND_INC_PTR(*msg, record->
                rename.src.dentry, da_binlog_op_type_update,
                FDIR_PIECE_FIELD_INDEX_BASIC, 0);
    } else {  //parent changed
        GENERATE_MOVE_DENTRY_MESSAGES(*msg, record->rename.
                src.parent, record->rename.src.dentry);
    }
    return 0;
}

static inline int pack_messages(FDIRChangeNotifyEvent *event)
{
    int result;
    FDIRChangeNotifyMessage *msg;
    FDIRChangeNotifyMessage *end;

    end = event->marray.messages + event->marray.count;
    for (msg=event->marray.messages; msg<end; msg++) {
        msg->id = __sync_add_and_fetch(&g_data_thread_vars.
                current_event_id, 1);
        if (msg->op_type == da_binlog_op_type_remove ||
                msg->field_index == FDIR_PIECE_FIELD_INDEX_CHILDREN)
        {
            msg->buffer = NULL;
        } else if ((result=dentry_serializer_pack(msg->dentry,
                        msg->field_index, &msg->buffer)) != 0)
        {
            return result;
        }

        dentry_hold(msg->dentry);
    }

    return 0;
}

int push_to_db_update_queue(FDIRBinlogRecord *record)
{
    int result;
    FDIRChangeNotifyEvent *event;
    FDIRChangeNotifyMessage *msg;

    event = (FDIRChangeNotifyEvent *)fast_mblock_alloc_object(
            &NOTIFY_EVENT_ALLOCATOR);
    if (event == NULL) {
        return ENOMEM;
    }

    event->version = record->data_version;
    msg = event->marray.messages;

    switch (record->operation) {
        case BINLOG_OP_CREATE_DENTRY_INT:
            if (record->affected.count > 0) {
                generate_affected_messages(&msg, record);
            }
            GENERATE_DENTRY_MESSAGES(msg, record->me.dentry,
                    da_binlog_op_type_create);
            break;
        case BINLOG_OP_UPDATE_DENTRY_INT:
            FDIR_CHANGE_NOTIFY_FILL_MSG_AND_INC_PTR(msg, record->me.dentry,
                    da_binlog_op_type_update, FDIR_PIECE_FIELD_INDEX_BASIC,
                    (record->options.inc_alloc ? record->stat.alloc : 0));
            break;
        case BINLOG_OP_SET_XATTR_INT:
        case BINLOG_OP_REMOVE_XATTR_INT:
            FDIR_CHANGE_NOTIFY_FILL_MSG_AND_INC_PTR(msg, record->
                    me.dentry, da_binlog_op_type_update,
                    FDIR_PIECE_FIELD_INDEX_XATTR, 0);
            break;
        case BINLOG_OP_REMOVE_DENTRY_INT:
            generate_remove_messages(&msg, record);
            break;
        case BINLOG_OP_RENAME_DENTRY_INT:
            if ((result=generate_rename_messages(&msg, record)) != 0) {
                return result;
            }
            break;
        default:
            break;
    }

    event->marray.count = msg - event->marray.messages;
    if ((result=pack_messages(event)) != 0) {
        return result;
    }

    change_notify_push_to_queue(event);
    return 0;
}

static int deal_update_record(FDIRDataThreadContext *thread_ctx,
        FDIRBinlogRecord *record)
{
    int result;
    int ignore_errno;
    bool set_data_verson;
    bool is_error;

    record->affected.count = 0;
    switch (record->operation) {
        case BINLOG_OP_CREATE_DENTRY_INT:
        case BINLOG_OP_REMOVE_DENTRY_INT:
            if ((result=find_or_check_parent(thread_ctx, record)) != 0) {
                ignore_errno = 0;
                break;
            }
            if (record->operation == BINLOG_OP_CREATE_DENTRY_INT) {
                if (FDIR_IS_DENTRY_HARD_LINK(record->stat.mode)) {
                    if ((result=set_hdlink_src_dentry(record)) != 0) {
                        ignore_errno = 0;
                        break;
                    }
                } else if (S_ISLNK(record->stat.mode) && record->dentry_type
                        == fdir_dentry_type_fullname)
                {
                    if ((result=service_set_record_link(record,
                                    (struct fast_task_info *)
                                    record->notify.args)) != 0)
                    {
                        ignore_errno = 0;
                        break;
                    }
                }
                result = dentry_create(thread_ctx, record);
                ignore_errno = EEXIST;
            } else {
                result = dentry_remove(thread_ctx, record);
                ignore_errno = ENOENT;
            }
            break;
        case BINLOG_OP_RENAME_DENTRY_INT:
            ignore_errno = 0;
            result = deal_record_rename_op(thread_ctx, record);
            break;
        case BINLOG_OP_UPDATE_DENTRY_INT:
            record->me.dentry = inode_index_update_dentry(record);
            result = (record->me.dentry != NULL) ? 0 : ENOENT;
            ignore_errno = 0;
            break;
        case BINLOG_OP_SET_XATTR_INT:
            if ((result=xattr_update_prepare(thread_ctx, record)) == 0) {
                result = inode_index_set_xattr(record);
            }
            ignore_errno = 0;
            break;
        case BINLOG_OP_REMOVE_XATTR_INT:
            if ((result=xattr_update_prepare(thread_ctx, record)) == 0) {
                result = inode_index_remove_xattr(record->inode,
                        &record->xattr.key);
            }
            ignore_errno = ENODATA;
            break;
        case SERVICE_OP_SET_DSIZE_INT:
            ignore_errno = EEXIST;
            if ((result=inode_index_check_set_dentry_size(
                            record, true)) == 0)
            {
                if (record->options.flags == 0) {
                    result = EEXIST;
                }
            }
            break;
        default:
            ignore_errno = 0;
            result = 0;
            break;
    }

    if (result == 0) {
        if (record->data_version == 0) {
            record->data_version = __sync_add_and_fetch(
                    &DATA_CURRENT_VERSION, 1);
            set_data_verson = false;
        } else {
            set_data_verson = true;
        }
        is_error = false;
    } else {
        set_data_verson = record->data_version > 0;
        is_error = !((result == ignore_errno) &&
                (g_data_thread_vars.error_mode ==
                 FDIR_DATA_ERROR_MODE_LOOSE));
    }

    if (set_data_verson && !is_error) {
        int64_t old_version;
        old_version = __sync_add_and_fetch(&DATA_CURRENT_VERSION, 0);
        if (record->data_version > old_version) {
            __sync_bool_compare_and_swap(&DATA_CURRENT_VERSION,
                    old_version, record->data_version);
        }
    }

    if (result == 0 && STORAGE_ENABLED) {
        if (record->data_version > thread_ctx->DATA_THREAD_LAST_VERSION) {
            thread_ctx->DATA_THREAD_LAST_VERSION = record->data_version;
        }

        if ((result=push_to_db_update_queue(record)) != 0) {
            logCrit("file: "__FILE__", line: %d, "
                    "push_to_db_update_queue fail, "
                    "program exit!", __LINE__);
            sf_terminate_myself();
        }
    }

    if (record->notify.func != NULL) {
        record->notify.func(record, result, is_error);
    }

    /*
    logInfo("file: "__FILE__", line: %d, record: %p, "
            "operation: %d, hash code: %u, inode: %"PRId64
             ", data_version: %"PRId64", result: %d, is_error: %d",
             __LINE__, record, record->operation, record->hash_code,
             record->inode, record->data_version, result, is_error);
             */

    return result;
}

static int list_dentry(FDIRDataThreadContext *thread_ctx,
        FDIRBinlogRecord *record)
{
    int result;
    struct fast_task_info *task;

    task = (struct fast_task_info *)record->notify.args;
    if (record->dentry_type == fdir_dentry_type_inode) {
        if ((record->me.dentry=inode_index_get_dentry(
                        record->inode)) == NULL)
        {
            return ENOENT;
        }

        result = dentry_list(record->me.dentry, &DENTRY_LIST_CACHE.array);
    } else {
        result = dentry_list_by_path(&record->me.fullname,
                &DENTRY_LIST_CACHE.array);
    }

    return result;
}

static int deal_query_record(FDIRDataThreadContext *thread_ctx,
        FDIRBinlogRecord *record)
{
    int result;
    switch (record->operation) {
        case SERVICE_OP_STAT_DENTRY_INT:
        case SERVICE_OP_READ_LINK_INT:
        case SERVICE_OP_LOOKUP_INODE_INT:
        case SERVICE_OP_GET_XATTR_INT:
        case SERVICE_OP_LIST_XATTR_INT:
            if (record->dentry_type == fdir_dentry_type_inode) {
                record->me.dentry = inode_index_get_dentry(record->inode);
                result = (record->me.dentry != NULL ? 0 : ENOENT);
            } else if (record->dentry_type == fdir_dentry_type_pname) {
                record->me.dentry=inode_index_get_dentry_by_pname(record->
                        me.pname.parent_inode, &record->me.pname.name);
                result = (record->me.dentry != NULL ? 0 : ENOENT);
            } else {
                result = dentry_find(&record->me.fullname, &record->me.dentry);
            }

            if (result == 0) {
                if (record->operation == SERVICE_OP_GET_XATTR_INT) {
                    result = inode_index_get_xattr(record->me.dentry,
                            &record->xattr.key, &record->xattr.value);
                }
            }

            break;
        case SERVICE_OP_LIST_DENTRY_INT:
            result = list_dentry(thread_ctx, record);
            break;
        default:
            result = EPROTONOSUPPORT;
            break;
    }

    record->notify.func(record, result, result != 0);
    return result;
}

static void *data_thread_func(void *arg)
{
    FDIRBinlogRecord *record;
    FDIRBinlogRecord *current;
    FDIRDataThreadContext *thread_ctx;
    int count;

    __sync_add_and_fetch(&DATA_THREAD_RUNNING_COUNT, 1);
    thread_ctx = (FDIRDataThreadContext *)arg;

#ifdef OS_LINUX
    {
        char thread_name[16];
        snprintf(thread_name, sizeof(thread_name),
                "data[%d]", thread_ctx->index);
        prctl(PR_SET_NAME, thread_name);
    }
#endif

    while (SF_G_CONTINUE_FLAG) {
        record = (FDIRBinlogRecord *)fc_queue_pop_all(&thread_ctx->queue);
        if (record == NULL) {
            continue;
        }

        count = 0;
        do {
            current = record;
            record = record->next;
            if (record->is_update) {
                deal_update_record(thread_ctx, current);
            } else {
                deal_query_record(thread_ctx, current);
            }
            ++count;
        } while (record != NULL);

        if (STORAGE_ENABLED) {
            __sync_sub_and_fetch(&thread_ctx->update_notify.
                    waiting_records, count);
        }

        deal_delay_free_queue(thread_ctx);
        if (__sync_add_and_fetch(&thread_ctx->free_context.
                    immediate.waiting_count, 0) != 0)
        {
            logInfo("file: "__FILE__", line: %d, "
                    "free_context.immediate.waiting_count: %d",
                    __LINE__, __sync_add_and_fetch(&thread_ctx->
                        free_context.immediate.waiting_count, 0));

            deal_immediate_free_queue(thread_ctx);
        }
    }

    __sync_sub_and_fetch(&DATA_THREAD_RUNNING_COUNT, 1);
    return NULL;
}
