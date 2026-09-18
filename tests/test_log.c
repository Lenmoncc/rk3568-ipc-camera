/** @file test_log.c @brief 并发日志行为测试，不涉及媒体线程。 */
#include "log.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

static void *writer(void *arg)
{
    unsigned int i;
    int id = (int)(intptr_t)arg;
    for (i = 0; i < 100; ++i)
        ipc_log_write(IPC_LOG_INFO, "test", "thread=%d item=%u", id, i);
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    int i;
    ipc_log_set_level(IPC_LOG_WARN);
    ipc_log_write(IPC_LOG_INFO, "test", "THIS_MUST_NOT_APPEAR");
    ipc_log_set_level(IPC_LOG_INFO);
    for (i = 0; i < 4; ++i)
        if (pthread_create(&threads[i], NULL, writer, (void *)(intptr_t)i) != 0)
            return 1;
    for (i = 0; i < 4; ++i)
        if (pthread_join(threads[i], NULL) != 0)
            return 1;
    return 0;
}
