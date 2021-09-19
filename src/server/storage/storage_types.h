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

#ifndef _FDIR_STORAGE_TYPES_H
#define _FDIR_STORAGE_TYPES_H

#include "fastcommon/common_define.h"
#include "fastcommon/fc_list.h"
#include "sf/sf_types.h"
#include "diskallocator/binlog/common/binlog_types.h"
#include "fastdir/client/fdir_server_types.h"

#define FDIR_STORAGE_BINLOG_TYPE_INODE   0
#define FDIR_STORAGE_BINLOG_TYPE_TRUNK   1
#define FDIR_STORAGE_BINLOG_TYPE_COUNT   2

#define FDIR_STORAGE_BATCH_INODE_BITS   16
#define FDIR_STORAGE_BATCH_INODE_COUNT  (1 << FDIR_STORAGE_BATCH_INODE_BITS)

#define FDIR_STORAGE_INODE_STATUS_NORMAL   0
#define FDIR_STORAGE_INODE_STATUS_DELETED  1

#define FDIR_STORAGE_SEGMENT_STATUS_CLEAN    0
#define FDIR_STORAGE_SEGMENT_STATUS_LOADING  1
#define FDIR_STORAGE_SEGMENT_STATUS_READY    2

#define FDIR_INODE_BINLOG_RECORD_MAX_SIZE  128

typedef struct fdir_storage_inode_index_info {
    int64_t version;
    uint64_t inode;
    FDIRServerPieceStorage fields[FDIR_PIECE_FIELD_COUNT];
    int status;
} FDIRStorageInodeIndexInfo;

typedef struct fdir_storage_inode_index_array {
    FDIRStorageInodeIndexInfo *inodes;
    int alloc;
    struct {
        int total;
        int deleted;
        int adding;
    } counts;
} FDIRStorageInodeIndexArray;

typedef enum fdir_inode_binlog_id_op_type {
    inode_binlog_id_op_type_create = 'c',
    inode_binlog_id_op_type_remove = 'd'
} FDIRInodeBinlogIdOpType;

typedef struct fdir_inode_segment_index_info {
    DABinlogWriter writer;
    struct {
        uint64_t first;
        uint64_t last;
        FDIRStorageInodeIndexArray array;
        short status;
    } inodes;
    time_t last_access_time;
    pthread_lock_cond_pair_t lcp;
    struct fc_list_head dlink;  //for FIFO elimination algorithm
} FDIRInodeSegmentIndexInfo;

typedef struct fdir_inode_binlog_id_journal {
    uint64_t binlog_id;
    int64_t version;
    FDIRInodeBinlogIdOpType op_type;
} FDIRInodeBinlogIdJournal;

typedef struct fdir_inode_bid_journal_array {
    FDIRInodeBinlogIdJournal *records;
    int count;
} FDIRInodeBidJournalArray;

#endif
