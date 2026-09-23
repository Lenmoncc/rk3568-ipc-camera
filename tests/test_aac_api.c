/** @file test_aac_api.c
 * @brief 使用真实 FFmpeg AAC 编码器测试非整帧尾部、借用所有权、错误状态与 ADTS 边界。
 */
#include "audio_encoder.h"
#include "audio_adts.h"
#include "timestamp.h"
#include <libavcodec/avcodec.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { IpcAudioAdts adts; int64_t last_pts; bool seen; } Sink;
/** @brief 检查真实包时间戳严格递增，再交由正式 ADTS 接收端保存。 */
static int save_packet(void *opaque, const IpcEncodedPacket *p)
{
    Sink *sink = opaque;
    assert(p->time_base.num == 1 && p->time_base.den == 48000);
    assert(p->packet->pts != AV_NOPTS_VALUE);
    assert(!sink->seen || p->packet->pts > sink->last_pts);
    sink->seen = true; sink->last_pts = p->packet->pts;
    return ipc_audio_adts_write(&sink->adts, p);
}
/** @brief 模拟输出设备写错，验证编码器返回原始错误并进入失败状态。 */
static int reject_packet(void *opaque, const IpcEncodedPacket *p)
{ (void)opaque; (void)p; return -ENOSPC; }
/** @brief 生成独立左右声道音调及精确采样 PTS，输入仍由调用者持有。 */
static IpcRawFrame make_frame(uint8_t *data, unsigned int count, uint64_t index, unsigned int channels)
{
    IpcRawFrame frame = {.type=IPC_MEDIA_AUDIO,.data=data,.size=(size_t)count*channels*2};
    frame.info.audio.sample_rate = 48000;
    frame.info.audio.channels = channels;
    frame.info.audio.samples_per_channel = count;
    frame.info.audio.sample_index = index;
    frame.info.audio.sample_format = IPC_AUDIO_FORMAT_S16_LE;
    assert(ipc_audio_pts_us(175001, index, 48000, &frame.pts_us) == 0);
    for (unsigned int i=0;i<count;++i) for (unsigned int c=0;c<channels;++c) {
        int16_t value=(int16_t)(12000*sin(2*3.141592653589793*(c ? 880:440)*(double)(index+i)/48000));
        size_t pos=((size_t)i*channels+c)*2;
        data[pos]=(uint8_t)value; data[pos+1]=(uint8_t)((uint16_t)value>>8);
    }
    return frame;
}
/** @brief 编码指定长度，覆盖多种输入块尺寸，输出供 Python 独立解析和解码检查。 */
static void encode_case(IpcConfig config, const char *directory, unsigned int total, unsigned int channels)
{
    IpcAudioEncoder *encoder=NULL;
    IpcAudioEncoderStats stats;
    const IpcStreamParams *params=NULL;
    char path[1024];
    uint8_t data[1703*4];
    config.audio_channels=channels;
    assert(ipc_audio_encoder_init(&encoder,&config)==0);
    assert(ipc_audio_encoder_init(&encoder,&config)==-EINVAL);
    assert(ipc_audio_encoder_get_params(encoder,&params)==0);
    assert(params->codecpar->extradata_size>=2);
    snprintf(path,sizeof(path),"%s/tone-%u-%u.aac",directory,total,channels);
    FILE *file=fopen(path,"wb"); assert(file);
    Sink sink={0}; assert(ipc_audio_adts_init(&sink.adts,file,params)==0);
    /* 多种块长度跨越编码帧边界；首次校验失败不得接管或消耗输入。 */
    unsigned int sizes[]={1,479,1703,512,17};
    for (unsigned int index=0,block=0;index<total;++block) {
        unsigned int n=sizes[block%5]; if (n>total-index) n=total-index;
        IpcRawFrame f=make_frame(data,n,index,channels);
        if (!index) {
            ++f.info.audio.sample_index;
            assert(ipc_audio_encoder_push(encoder,&f,save_packet,&sink)==-EINVAL);
            --f.info.audio.sample_index;
        }
        assert(ipc_audio_encoder_push(encoder,&f,save_packet,&sink)==0);
        memset(data,0,sizeof(data)); /* push 返回即可覆写，编码器不得引用外部 PCM。 */
        index+=n;
    }
    assert(ipc_audio_encoder_finish(encoder,save_packet,&sink)==0);
    assert(ipc_audio_encoder_finish(encoder,save_packet,&sink)==0);
    assert(ipc_audio_encoder_get_stats(encoder,&stats)==0);
    assert(stats.input_samples==total && stats.converted_samples==total && stats.drained);
    assert(stats.submitted_samples==total+stats.padding_samples);
    assert(stats.padding_samples<stats.frame_samples && stats.packets==sink.adts.packets);
    assert(total==0 || stats.packets>0);
    IpcRawFrame f=make_frame(data,1,total,channels);
    assert(ipc_audio_encoder_push(encoder,&f,save_packet,&sink)==-EINVAL);
    assert(fclose(file)==0);
    ipc_audio_encoder_deinit(&encoder); ipc_audio_encoder_deinit(&encoder); assert(!encoder);
}
/** @brief 执行尾部和错误状态测试；文件产物由上层临时目录统一清理。 */
int main(int argc,char **argv)
{
    IpcConfig config;
    assert(argc==3 && ipc_config_load(argv[1],&config)==0);
    const unsigned int sizes[]={0,1,1023,1024,1025,480000};
    for (unsigned int i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) encode_case(config,argv[2],sizes[i],2);
    encode_case(config,argv[2],48000,1);
    IpcAudioEncoder *encoder=NULL;
    uint8_t data[8192];
    assert(ipc_audio_encoder_init(&encoder,&config)==0);
    IpcRawFrame f=make_frame(data,2048,0,2);
    int result=ipc_audio_encoder_push(encoder,&f,reject_packet,NULL);
    if (!result) result=ipc_audio_encoder_finish(encoder,reject_packet,NULL);
    assert(result==-ENOSPC);
    assert(ipc_audio_encoder_finish(encoder,reject_packet,NULL)==-EINVAL);
    ipc_audio_encoder_deinit(&encoder);
    /* ADTS 边界校验必须发生在写入前，过长包不能截断其 13 位长度字段。 */
    assert(ipc_audio_encoder_init(&encoder,&config)==0);
    const IpcStreamParams *params=NULL;
    assert(ipc_audio_encoder_get_params(encoder,&params)==0);
    FILE *file=tmpfile(); assert(file);
    IpcAudioAdts writer={0};
    assert(ipc_audio_adts_init(&writer,file,params)==0);
    AVPacket packet={0};
    IpcEncodedPacket encoded={IPC_MEDIA_AUDIO,&packet,{1,48000}};
    packet.data=data; packet.size=8192;
    assert(ipc_audio_adts_write(&writer,&encoded)==-EMSGSIZE);
    packet.size=0; assert(ipc_audio_adts_write(&writer,&encoded)==-EINVAL);
    assert(ftell(file)==0 && writer.packets==0);
    AVCodecParameters wrong=*params->codecpar;
    IpcStreamParams bad=*params; bad.codecpar=&wrong; wrong.profile=FF_PROFILE_AAC_HE;
    assert(ipc_audio_adts_init(&writer,file,&bad)==-ENOTSUP);
    assert(fclose(file)==0);
    ipc_audio_encoder_deinit(&encoder);
    config.audio_channels=0;
    assert(ipc_audio_encoder_init(&encoder,&config)==-EINVAL && !encoder);
    puts("PASS: real AAC tail, timestamps, ownership, mono/stereo and sink failure API checks.");
    return 0;
}
