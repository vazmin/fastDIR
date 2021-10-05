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

#ifndef _FDIR_SERVER_COMMON_TYPES_H
#define _FDIR_SERVER_COMMON_TYPES_H

#include "fastcommon/fast_buffer.h"

//piece storage field indexes
#define FDIR_PIECE_FIELD_INDEX_BASIC       0
#define FDIR_PIECE_FIELD_INDEX_CHILDREN    1
#define FDIR_PIECE_FIELD_INDEX_XATTR       2
#define FDIR_PIECE_FIELD_COUNT             3

//virtual field index for sort and check
#define FDIR_PIECE_FIELD_INDEX_FOR_REMOVE 10

#define FDIR_PIECE_FIELD_CLEAR(fields) \
    DA_PIECE_FIELD_DELETE(fields[FDIR_PIECE_FIELD_INDEX_BASIC]);    \
    DA_PIECE_FIELD_DELETE(fields[FDIR_PIECE_FIELD_INDEX_CHILDREN]); \
    DA_PIECE_FIELD_DELETE(fields[FDIR_PIECE_FIELD_INDEX_XATTR])

typedef struct fdir_db_update_message {
    int field_index;
    FastBuffer *buffer;
} FDIRDBUpdateMessage;

typedef struct fdir_dentry_merged_messages {
    FDIRDBUpdateMessage messages[FDIR_PIECE_FIELD_COUNT];
    int msg_count;
    int merge_count;
} FDIRDentryMergedMessages;

typedef struct fdir_db_update_dentry {
    int64_t version;
    int64_t inode;
    DABinlogOpType op_type;
    FDIRDentryMergedMessages mms;
    void *args;
    struct fdir_db_update_dentry *next;  //for queue
} FDIRDBUpdateDentry;

typedef struct fdir_db_update_dentry_array {
    FDIRDBUpdateDentry *entries;
    int count;
    int alloc;
} FDIRDBUpdateDentryArray;

#endif
