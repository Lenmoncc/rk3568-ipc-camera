/** @file test_packet_queue.c
 * @brief 真实 AVPacket 所有权、队列关闭/并发，以及 MPP 分片组帧边界检查。
 */
#define _POSIX_C_SOURCE 200809L
#include "packet_queue.h"
#include "h264_bridge.h"
#include <libavcodec/avcodec.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
typedef struct {IpcPacketQueue *q;unsigned int base;} Producer;
static atomic_uint seen[3000], consumed;
/** @brief 创建拥有独立 payload 的包，嵌入唯一编号供并发消费核验。 */
static IpcEncodedPacket *make_packet(unsigned int id)
{
    AVPacket *packet=av_packet_alloc();assert(packet && av_new_packet(packet,4)==0);
    memcpy(packet->data,&id,4);packet->pts=packet->dts=id;packet->duration=1;
    IpcEncodedPacket src={IPC_MEDIA_AUDIO,packet,{1,48000}},*copy=NULL;
    assert(ipc_encoded_packet_ref(&copy,&src)==0);av_packet_free(&packet);return copy;
}
/** @brief 并发生产 1000 个唯一包，满时保留所有权并重试同一包。 */
static void *produce(void *opaque)
{
    Producer *p=opaque;
    for(unsigned int i=0;i<1000;++i){
        IpcEncodedPacket *packet=make_packet(p->base+i);
        int result;
        while((result=ipc_packet_queue_try_push(p->q,packet))==-EAGAIN){
            struct timespec delay={0,100000};nanosleep(&delay,NULL);
        }
        assert(result==0);
    }
    return NULL;
}
/** @brief 出队至关闭且空，每个编号恰好被一个消费者接收。 */
static void *consume(void *opaque)
{
    IpcPacketQueue *q=opaque;IpcEncodedPacket *packet=NULL;int result;
    while((result=ipc_packet_queue_pop(q,&packet))==1){
        unsigned int id;memcpy(&id,packet->packet->data,4);assert(id<3000);
        assert(atomic_fetch_add(&seen[id],1)==0);atomic_fetch_add(&consumed,1);
        ipc_encoded_packet_free(&packet);
    }
    assert(result==0);return NULL;
}
/** @brief 检验 EOF 广播可唤醒每个空队列等待者。 */
static void *wait_closed(void *opaque)
{IpcEncodedPacket *packet=NULL;assert(ipc_packet_queue_pop(opaque,&packet)==0 && !packet);return NULL;}
/** @brief 检查复制引用的元数据独立及底层数据在源释放后仍有效。 */
static void test_ownership(void)
{
    IpcEncodedPacket *a=make_packet(17),*b=NULL;
    assert(ipc_encoded_packet_ref(&b,a)==0);b->packet->pts=99;b->packet->stream_index=1;
    assert(a->packet->pts==17 && a->packet->stream_index==0);
    assert(a->packet->data==b->packet->data);
    ipc_encoded_packet_free(&a);unsigned int value;memcpy(&value,b->packet->data,4);assert(value==17);
    ipc_encoded_packet_free(&b);ipc_encoded_packet_free(&b);
}
/** @brief 检查零容量、满队列所有权、超时、关闭后排空和残留包释放。 */
static void test_lifecycle(void)
{
    IpcPacketQueue *q=NULL;IpcEncodedPacket *out=NULL;
    assert(ipc_packet_queue_create(&q,0)==-EINVAL);
    assert(ipc_packet_queue_create(&q,SIZE_MAX)==-EOVERFLOW);
    assert(ipc_packet_queue_create(&q,1)==0);
    assert(ipc_packet_queue_create(&q,1)==-EINVAL);
    assert(ipc_packet_queue_pop_timed(q,&out,2)==-EAGAIN);
    IpcEncodedPacket *a=make_packet(1),*b=make_packet(2);
    assert(ipc_packet_queue_try_push(q,a)==0);
    assert(ipc_packet_queue_try_push(q,b)==-EAGAIN);assert(b->packet->pts==2);
    ipc_packet_queue_close(q);ipc_packet_queue_close(q);
    assert(ipc_packet_queue_try_push(q,b)==-EPIPE);ipc_encoded_packet_free(&b);
    assert(ipc_packet_queue_pop(q,&out)==1 && out==a);ipc_encoded_packet_free(&out);
    assert(ipc_packet_queue_pop(q,&out)==0);
    ipc_packet_queue_destroy(&q);ipc_packet_queue_destroy(&q);
    assert(ipc_packet_queue_create(&q,1)==0);assert(ipc_packet_queue_try_push(q,make_packet(3))==0);ipc_packet_queue_destroy(&q);
    assert(ipc_packet_queue_create(&q,1)==0);pthread_t t[3];
    for(unsigned int i=0;i<3;++i)assert(!pthread_create(&t[i],NULL,wait_closed,q));
    ipc_packet_queue_close(q);
    for(unsigned int i=0;i<3;++i)assert(!pthread_join(t[i],NULL));
    ipc_packet_queue_destroy(&q);
}
/** @brief 多生产者/消费者压力检查，每包仅转交一次，关闭后全部排空。 */
static void test_concurrent(void)
{
    IpcPacketQueue *q=NULL;assert(ipc_packet_queue_create(&q,7)==0);
    pthread_t producer[3],consumer[2];Producer args[3];
    for(unsigned int i=0;i<2;++i)assert(!pthread_create(&consumer[i],NULL,consume,q));
    for(unsigned int i=0;i<3;++i){args[i]=(Producer){q,i*1000};assert(!pthread_create(&producer[i],NULL,produce,&args[i]));}
    for(unsigned int i=0;i<3;++i)assert(!pthread_join(producer[i],NULL));
    ipc_packet_queue_close(q);
    for(unsigned int i=0;i<2;++i)assert(!pthread_join(consumer[i],NULL));
    assert(atomic_load(&consumed)==3000);ipc_packet_queue_destroy(&q);
}
/** @brief 检查头就绪、分片复制、帧时长推导、末帧排空和截断帧拒绝。 */
static void test_bridge(void)
{
    const uint8_t header[]={0,0,0,1,0x67,0x42,0,0,0,1,0x68,0x11};
    uint8_t bytes[]={0,0,0,1,0x65,0x12};
    IpcConfig config={.video_width=1280,.video_height=720,.video_bitrate=2000000};
    IpcPacketQueue *q=NULL;IpcH264Bridge *b=NULL;const IpcStreamParams *params=NULL;
    assert(ipc_packet_queue_create(&q,2)==0);assert(ipc_h264_bridge_init(&b,&config,25,q)==0);
    assert(ipc_h264_bridge_get_params(b,&params)<0);
    IpcH264Packet h={.data=header,.size=sizeof(header),.header=true};
    assert(ipc_h264_bridge_sink(b,&h)==0);assert(ipc_h264_bridge_get_params(b,&params)==0);
    assert(params->codecpar->extradata_size==(int)sizeof(header));
    IpcH264Packet f={.data=bytes,.size=3,.pts_us=100,.key_frame=true};
    assert(ipc_h264_bridge_sink(b,&f)==0);f.data=bytes+3;f.frame_end=true;
    assert(ipc_h264_bridge_sink(b,&f)==0);
    f.data=bytes;f.size=6;f.pts_us=200;assert(ipc_h264_bridge_sink(b,&f)==0);
    memset(bytes,0,sizeof(bytes));assert(ipc_h264_bridge_finish(b)==0);assert(ipc_h264_bridge_finish(b)==0);
    IpcEncodedPacket *packet=NULL;
    assert(ipc_packet_queue_pop(q,&packet)==1);assert(packet->packet->pts==100 && packet->packet->duration==100);
    assert(packet->packet->size==6 && packet->packet->data[4]==0x65 && (packet->packet->flags&AV_PKT_FLAG_KEY));
    ipc_encoded_packet_free(&packet);assert(ipc_packet_queue_pop(q,&packet)==1);assert(packet->packet->duration==40000);
    ipc_encoded_packet_free(&packet);ipc_h264_bridge_deinit(&b);ipc_packet_queue_destroy(&q);
    assert(ipc_packet_queue_create(&q,1)==0);assert(ipc_h264_bridge_init(&b,&config,25,q)==0);
    assert(ipc_h264_bridge_sink(b,&h)==0);f.frame_end=false;f.size=1;
    assert(ipc_h264_bridge_sink(b,&f)==0);assert(ipc_h264_bridge_finish(b)==-EBADMSG);
    ipc_h264_bridge_deinit(&b);ipc_packet_queue_destroy(&q);
}
/** @brief 执行真实 AVPacket 队列与桥接检查。 */
int main(void)
{
    test_ownership();test_lifecycle();test_concurrent();test_bridge();
    puts("PASS: packet ownership/lifecycle, 3000 concurrent packets and H264 fragment bridge.");return 0;
}
