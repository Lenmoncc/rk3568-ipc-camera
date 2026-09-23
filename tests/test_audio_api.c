/** @file test_audio_api.c
 * @brief 样本时间戳边界与 ALSA 所有权/短块/状态检查；不访问硬件。
 */
#define _POSIX_C_SOURCE 200809L
#include "alsa_capture.h"
#include "frame_queue.h"
#include "timestamp.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>

/** @brief 核对累计样本换算的精确值、溢出保护与错误时不修改输出。 */
static void test_timestamps(void)
{
    int64_t pts=77;
    assert(ipc_audio_pts_us(0,480000,48000,&pts)==0 && pts==10000000);
    assert(ipc_audio_pts_us(42,44100,44100,&pts)==0 && pts==1000042);
    assert(ipc_audio_pts_us(0,1,48000,&pts)==0 && pts==20);
    assert(ipc_audio_pts_us(INT64_MAX,0,48000,&pts)==0 && pts==INT64_MAX);
    assert(ipc_audio_pts_us(INT64_MAX,1,48000,&pts)==-EOVERFLOW && pts==INT64_MAX);
    assert(ipc_audio_pts_us(0,UINT64_MAX,1,&pts)==-EOVERFLOW);
    assert(ipc_audio_pts_us(0,UINT64_MAX,UINT_MAX,&pts)==0);
    assert(ipc_audio_pts_us(-1,0,48000,&pts)==-EINVAL);
    assert(ipc_audio_pts_us(0,0,0,&pts)==-EINVAL);
    assert(ipc_audio_pts_us(0,0,48000,NULL)==-EINVAL);
}
/** @brief 验证创建/启动/读取/停止契约，前一块在下一次读取后仍独立有效。 */
int main(int argc,char **argv)
{
    IpcConfig config;
    IpcAudioCapture *capture=NULL;
    IpcAudioCaptureFormat format;
    IpcRawFrame *one=NULL,*two=NULL;
    IpcAudioCaptureStats stats;
    test_timestamps();
    assert(argc==2 && ipc_config_load(argv[1],&config)==0);
    assert(ipc_audio_capture_init(NULL,&config)==-EINVAL);
    assert(ipc_audio_capture_init(&capture,&config)==0);
    assert(ipc_audio_capture_init(&capture,&config)==-EINVAL);
    assert(ipc_audio_capture_get_format(capture,&format)==0 && format.channels==2);
    assert(ipc_audio_capture_read(capture,&one,100,0)==-EINVAL);
    assert(ipc_audio_capture_start(capture,ipc_monotonic_us())==0);
    assert(ipc_audio_capture_start(capture,0)==-EINVAL);
    assert(ipc_audio_capture_read(capture,&one,0,0)==-EINVAL);
    assert(ipc_audio_capture_read(capture,&one,7,0)==1 && one->size==28 && one->info.audio.sample_index==0);
    assert(ipc_audio_capture_read(capture,&one,7,0)==-EINVAL);
    assert(ipc_audio_capture_read(capture,&two,9,0)==1 && two->info.audio.sample_index==7);
    assert(one->data[0]==1 && one->data[2]==255 && two->data[0]==8);
    assert(two->pts_us==one->pts_us+145);
    assert(ipc_audio_capture_get_stats(capture,&stats)==0 && stats.samples==16 && stats.blocks==2);
    ipc_raw_frame_free(&one); ipc_raw_frame_free(&two);
    assert(ipc_audio_capture_stop(capture)==0 && ipc_audio_capture_stop(capture)==0);
    assert(ipc_audio_capture_start(capture,0)==-EINVAL);
    assert(ipc_audio_capture_read(capture,&one,7,0)==-EINVAL);
    assert(ipc_audio_capture_deinit(&capture)==0 && !capture);
    assert(ipc_audio_capture_deinit(&capture)==0 && ipc_audio_capture_deinit(NULL)==0);
    puts("PASS: audio timestamp overflow, state and ownership API checks.");
    return 0;
}
