#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include "fastcommon/logger.h"
#include "fastcommon/sockopt.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/pthread_func.h"
#include "binlog/binlog_producer.h"
#include "server_global.h"
#include "server_binlog.h"

int server_binlog_init()
{
    int result;

    if ((result=binlog_producer_init()) != 0) {
        return result;
    }

	return 0;
}

void server_binlog_destroy()
{
    binlog_producer_destroy();
}
