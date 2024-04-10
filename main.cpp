#define MODULE_TAG "fpvue"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <inttypes.h>
#include <signal.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <linux/videodev2.h>
#include <rockchip/rk_mpi.h>

#include "linux/dma-buf.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "main.h"
#include "drm.h"
#include "osd.h"
#include "rtp.h"

#include "mavlink/common/mavlink.h"
#include "mavlink.h"
#include "time_util.h"
#include "copy_util.h"
#ifdef __cplusplus
}
#endif
#ifdef __cplusplus
#include "gstrtpreceiver.h"
#include "SchedulingHelper.hpp"
#endif

// This buffer size has no effect on the latency -
// 5MB should be enough, no matter how high bitrate the stream is.
#define READ_BUF_SIZE (1024*1024*5) // SZ_1M https://github.com/rockchip-linux/mpp/blob/ed377c99a733e2cdbcc457a6aa3f0fcd438a9dff/osal/inc/mpp_common.h#L179
#define MAX_FRAMES 16		// min 16 and 20+ recommended (mpp/readme.txt)

#define CODEC_ALIGN(x, a)   (((x)+(a)-1)&~((a)-1))

struct {
	MppCtx		  ctx;
	MppApi		  *mpi;
	
	struct timespec first_frame_ts;

	MppBufferGroup	frm_grp;
	struct {
		int prime_fd;
		uint32_t fb_id;
		uint32_t handle;
        // only used in copy mode
        void* memory_mmap;
        int memory_mmap_size;
	} frame_to_drm[MAX_FRAMES];
} mpi;

struct timespec frame_stats[1000];

struct modeset_output *output_list;
int frm_eos = 0;
int drm_fd = 0;
pthread_mutex_t video_mutex;
pthread_cond_t video_cond;

// OSD Vars
struct video_stats {
	int current_framerate;
	uint64_t current_latency;
	uint64_t max_latency;
	uint64_t min_latency;
};
struct video_stats osd_stats;
int bw_curr = 0;
long long bw_stats[10];
int video_zpos = 1;
int develop_rendering_mode=0;
bool decode_h265=false;
int gst_udp_port=-1;
struct TSAccumulator m_decoding_latency;
// NOTE: Does not track latency to end completely
struct TSAccumulator m_decode_and_handover_display_latency;
struct TSAccumulator m_drm_mode_set_plane_latency;
void start_sync(int fd,bool write){
    struct dma_buf_sync sync;
    sync.flags = DMA_BUF_SYNC_START | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ);

    int ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    if (ret) {
        printf("start_sync ioctl failed for %s %d\n", strerror(errno), fd);
    }
}
void end_sync(int fd,bool write){
    struct dma_buf_sync sync;
    sync.flags = DMA_BUF_SYNC_END | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ);

    int ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    if (ret) {
        printf("end_sync ioctl failed for %s %d\n", strerror(errno), fd);
    }
}

TSAccumulator m_map_copy_unmap_accumulator;
uint8_t test_buffer[1920*1080*2];
void map_copy_unmap(int fd_src,int fd_dst,int memory_size){
    //printf("map_copy_unmap\n");
    uint64_t map_copy_unmap_begin=get_time_ms();
    void * src_p=mmap(
            0, memory_size,    PROT_READ, MAP_PRIVATE,
            fd_src, 0);
    if (src_p == NULL || src_p == MAP_FAILED) {
        assert(false);
    }
    void * dst_p=mmap(
            0, memory_size,    PROT_WRITE, MAP_SHARED,
            fd_dst, 0);
    if (dst_p == NULL || dst_p == MAP_FAILED) {
        assert(false);
    }
    start_sync(fd_src,false);
    start_sync(fd_dst,true);
    // First, do the memset
    /*uint64_t before_memset=get_time_ms();
    char lol=get_time_ms() % 255;
    memset(dst_p,lol,memory_size);
    uint64_t elapsed_memset=get_time_ms()-before_memset;
    print_time_ms("memset took",elapsed_memset);*/
    //void* big_buff= malloc(memory_size);

    //memcpy(dst_p,src_p,memory_size);
    uint64_t before_memcpy=get_time_ms();
    printf("Memory size %d",memory_size);
    memcpy_threaded(&test_buffer,src_p,memory_size,3);
    end_sync(fd_src,false);
    end_sync(fd_dst,true);
    //uint64_t elapsed_memcpy=get_time_ms()-before_memcpy;
    //print_time_ms("memcpy took",elapsed_memcpy);
    uint64_t map_copy_unmap_elapsed=get_time_ms()-map_copy_unmap_begin;
    accumulate_and_print("map_copy_unmap",map_copy_unmap_elapsed,&m_map_copy_unmap_accumulator);
    //free(big_buff);
}

void copy_mpp_buff(MppBuffer* src,MppBuffer* dst){
    void *buf = mpp_buffer_get_ptr(*src);
}

void initialize_output_buffers(MppFrame  frame){
    int ret;
    int i;
    // new resolution
    assert(!mpi.frm_grp);
    output_list->video_frm_width = CODEC_ALIGN(mpp_frame_get_width(frame),16);
    output_list->video_frm_height = CODEC_ALIGN(mpp_frame_get_height(frame),16);
    RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
    RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
    MppFrameFormat fmt = mpp_frame_get_fmt(frame);
    assert((fmt == MPP_FMT_YUV420SP) || (fmt == MPP_FMT_YUV420SP_10BIT));

    printf("Frame info changed %d(%d)x%d(%d)\n", output_list->video_frm_width, hor_stride, output_list->video_frm_height, ver_stride);

    output_list->video_fb_x = 0;
    output_list->video_fb_y = 0;
    output_list->video_fb_width = output_list->mode.hdisplay;
    output_list->video_fb_height =output_list->mode.vdisplay;

    osd_vars.video_width = output_list->video_frm_width;
    osd_vars.video_height = output_list->video_frm_height;

    // create new external frame group and allocate (commit flow) new DRM buffers and DRM FB
    assert(!mpi.frm_grp);
    ret = mpp_buffer_group_get_external(&mpi.frm_grp, MPP_BUFFER_TYPE_DRM);
    assert(!ret);

    for (i=0; i<MAX_FRAMES; i++) {

        // new DRM buffer
        struct drm_mode_create_dumb dmcd;
        memset(&dmcd, 0, sizeof(dmcd));
        dmcd.bpp = fmt==MPP_FMT_YUV420SP?8:10;
        dmcd.width = hor_stride;
        dmcd.height = ver_stride*2; // documentation say not v*2/3 but v*2 (additional info included)
        do {
            ret = ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dmcd);
        } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
        assert(!ret);
        //assert(dmcd.pitch==(fmt==MPP_FMT_YUV420SP?hor_stride:hor_stride*10/8));
        //assert(dmcd.size==(fmt == MPP_FMT_YUV420SP?hor_stride:hor_stride*10/8)*ver_stride*2);
        mpi.frame_to_drm[i].handle = dmcd.handle;

        // commit DRM buffer to frame group
        struct drm_prime_handle dph;
        memset(&dph, 0, sizeof(struct drm_prime_handle));
        dph.handle = dmcd.handle;
        dph.fd = -1;
        // added for mmap
        dph.flags  = DRM_CLOEXEC | DRM_RDWR;
        do {
            ret = ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph);
        } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
        assert(!ret);
        //
        void * primed_framebuffer=mmap(
                0, dmcd.size,    PROT_READ | PROT_WRITE, MAP_SHARED,
                dph.fd, 0);
        if (primed_framebuffer == NULL || primed_framebuffer == MAP_FAILED) {
            printf(
                    "Could not map buffer exported through PRIME : %s (%d)\n"
                    "Buffer : %p\n",
                    strerror(ret), ret,
                    primed_framebuffer
            );
            assert(false);
        }
        mpi.frame_to_drm[i].memory_mmap=primed_framebuffer;
        mpi.frame_to_drm[i].memory_mmap_size=dmcd.size;
        printf("Buffer size bytes %d\n",(int)dmcd.size);
        //

        MppBufferInfo info;
        memset(&info, 0, sizeof(info));
        info.type = MPP_BUFFER_TYPE_DRM;
        info.size = dmcd.width*dmcd.height;
        info.fd = dph.fd;
        ret = mpp_buffer_commit(mpi.frm_grp, &info);
        assert(!ret);
        mpi.frame_to_drm[i].prime_fd = info.fd; // dups fd
        if (dph.fd != info.fd) {
            ret = close(dph.fd);
            assert(!ret);
        }

        // allocate DRM FB from DRM buffer
        uint32_t handles[4], pitches[4], offsets[4];
        memset(handles, 0, sizeof(handles));
        memset(pitches, 0, sizeof(pitches));
        memset(offsets, 0, sizeof(offsets));
        handles[0] = mpi.frame_to_drm[i].handle;
        offsets[0] = 0;
        pitches[0] = hor_stride;
        handles[1] = mpi.frame_to_drm[i].handle;
        offsets[1] = pitches[0] * ver_stride;
        pitches[1] = pitches[0];
        ret = drmModeAddFB2(drm_fd, output_list->video_frm_width, output_list->video_frm_height, DRM_FORMAT_NV12, handles, pitches, offsets, &mpi.frame_to_drm[i].fb_id, 0);
        assert(!ret);
    }

    // register external frame group
    ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_EXT_BUF_GROUP, mpi.frm_grp);
    ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);

    ret = modeset_perform_modeset(drm_fd, output_list, output_list->video_request, &output_list->video_plane, mpi.frame_to_drm[0].fb_id, output_list->video_frm_width, output_list->video_frm_height, video_zpos);
    assert(ret >= 0);
}

// 'Live buffer hack'
void initialize_output_buffers_ion(MppFrame  frame){
    int ret;
    int i;
    // new resolution
    assert(!mpi.frm_grp);

    output_list->video_frm_width = CODEC_ALIGN(mpp_frame_get_width(frame),16);
    output_list->video_frm_height = CODEC_ALIGN(mpp_frame_get_height(frame),16);
    RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
    RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
    MppFrameFormat fmt = mpp_frame_get_fmt(frame);
    assert((fmt == MPP_FMT_YUV420SP) || (fmt == MPP_FMT_YUV420SP_10BIT));

    printf("Frame info changed %d(%d)x%d(%d)\n", output_list->video_frm_width, hor_stride, output_list->video_frm_height, ver_stride);

    output_list->video_fb_x = 0;
    output_list->video_fb_y = 0;
    output_list->video_fb_width = output_list->mode.hdisplay;
    output_list->video_fb_height =output_list->mode.vdisplay;

    osd_vars.video_width = output_list->video_frm_width;
    osd_vars.video_height = output_list->video_frm_height;
    // create new external frame group and allocate (commit flow) new DRM buffers and DRM FB
    assert(!mpi.frm_grp);
    ret = mpp_buffer_group_get_external(&mpi.frm_grp,  MPP_BUFFER_TYPE_ION);
    assert(!ret);

    int lol_width=0;
    int lol_height=0;
    // Specify how many actual buffer(s) to create
    int n_drm_prime_buffers=1;
    for(i=0;i<n_drm_prime_buffers;i++){
        // new DRM buffer
        struct drm_mode_create_dumb dmcd;
        memset(&dmcd, 0, sizeof(dmcd));
        dmcd.bpp = fmt==MPP_FMT_YUV420SP?8:10;
        dmcd.width = hor_stride;
        dmcd.height = ver_stride*2; // documentation say not v*2/3 but v*2 (additional info included)
        do {
            ret = ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dmcd);
        } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
        assert(!ret);
        //assert(dmcd.pitch==(fmt==MPP_FMT_YUV420SP?hor_stride:hor_stride*10/8));
        //assert(dmcd.size==(fmt == MPP_FMT_YUV420SP?hor_stride:hor_stride*10/8)*ver_stride*2);
        mpi.frame_to_drm[i].handle = dmcd.handle;

        // commit DRM buffer to frame group
        struct drm_prime_handle dph;
        memset(&dph, 0, sizeof(struct drm_prime_handle));
        dph.handle = dmcd.handle;
        dph.fd = -1;
        dph.flags  = DRM_CLOEXEC | DRM_RDWR;
        do {
            ret = ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph);
        } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
        assert(!ret);
        //
        void * primed_framebuffer=mmap(
                0, dmcd.size,    PROT_READ | PROT_WRITE, MAP_SHARED,
                dph.fd, 0);
        if (primed_framebuffer == NULL || primed_framebuffer == MAP_FAILED) {
            printf(
                    "Could not map buffer exported through PRIME : %s (%d)\n"
                    "Buffer : %p\n",
                    strerror(ret), ret,
                    primed_framebuffer
            );
            assert(false);
        }
        mpi.frame_to_drm[i].memory_mmap=primed_framebuffer;
        //
        mpi.frame_to_drm[i].prime_fd = dph.fd; // dups fd
        lol_width=dmcd.width;
        lol_height=dmcd.height;
        // allocate DRM FB from DRM buffer
        uint32_t handles[4], pitches[4], offsets[4];
        memset(handles, 0, sizeof(handles));
        memset(pitches, 0, sizeof(pitches));
        memset(offsets, 0, sizeof(offsets));
        handles[0] = mpi.frame_to_drm[i].handle;
        offsets[0] = 0;
        pitches[0] = hor_stride;
        handles[1] = mpi.frame_to_drm[i].handle;
        offsets[1] = pitches[0] * ver_stride;
        pitches[1] = pitches[0];
        ret = drmModeAddFB2(drm_fd, output_list->video_frm_width, output_list->video_frm_height, DRM_FORMAT_NV12, handles, pitches, offsets, &mpi.frame_to_drm[i].fb_id, 0);
        assert(!ret);
    }
    // Can be different than n drm prime buffers.
    // If for example only one drm prime buffer was created, we pass the same buffer fd to mpp on each mpp buffer.
    for (i=0; i<16; i++) {
        MppBufferInfo info;
        memset(&info, 0, sizeof(info));
        info.type =  MPP_BUFFER_TYPE_ION;
        info.size = lol_width*lol_height;
        info.index = i;
        // We pass the same buffer multiple time(s) if needed
        int buffer_index = i % n_drm_prime_buffers;
        int this_drm_prime_fd=mpi.frame_to_drm[buffer_index].prime_fd;
        info.fd = this_drm_prime_fd;
        ret = mpp_buffer_commit(mpi.frm_grp, &info);
        assert(!ret);
        if (this_drm_prime_fd != info.fd) {
            // I have no idea why this happens ...
            printf("mpp changed buffer fd from %d to %d\n",this_drm_prime_fd,info.fd);
        }
    }

    // register external frame group
    ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_EXT_BUF_GROUP, mpi.frm_grp);
    ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);

    ret = modeset_perform_modeset(drm_fd, output_list, output_list->video_request, &output_list->video_plane, mpi.frame_to_drm[0].fb_id, output_list->video_frm_width, output_list->video_frm_height, video_zpos);
    drmModeSetCrtc(
            drm_fd, output_list->saved_crtc->crtc_id, mpi.frame_to_drm[0].fb_id,
            0, 0,
            &output_list->connector.id,
            1,
            &output_list->saved_crtc->mode);
    assert(ret >= 0);

}


// First buffer is the one displayed, but not committed to mpp
void initialize_output_buffers_memcpy(MppFrame  frame){
    int ret;
    int i;
    // new resolution
    assert(!mpi.frm_grp);
    output_list->video_frm_width = CODEC_ALIGN(mpp_frame_get_width(frame),16);
    output_list->video_frm_height = CODEC_ALIGN(mpp_frame_get_height(frame),16);
    RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
    RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
    MppFrameFormat fmt = mpp_frame_get_fmt(frame);
    assert((fmt == MPP_FMT_YUV420SP) || (fmt == MPP_FMT_YUV420SP_10BIT));

    printf("Frame info changed %d(%d)x%d(%d)\n", output_list->video_frm_width, hor_stride, output_list->video_frm_height, ver_stride);

    output_list->video_fb_x = 0;
    output_list->video_fb_y = 0;
    output_list->video_fb_width = output_list->mode.hdisplay;
    output_list->video_fb_height =output_list->mode.vdisplay;

    osd_vars.video_width = output_list->video_frm_width;
    osd_vars.video_height = output_list->video_frm_height;

    // create new external frame group and allocate (commit flow) new DRM buffers and DRM FB
    assert(!mpi.frm_grp);
    ret = mpp_buffer_group_get_external(&mpi.frm_grp, MPP_BUFFER_TYPE_DRM);
    assert(!ret);

    for (i=0; i<MAX_FRAMES; i++) {

        // new DRM buffer
        struct drm_mode_create_dumb dmcd;
        memset(&dmcd, 0, sizeof(dmcd));
        dmcd.bpp = fmt==MPP_FMT_YUV420SP?8:10;
        dmcd.width = hor_stride;
        dmcd.height = ver_stride*2; // documentation say not v*2/3 but v*2 (additional info included)
        do {
            ret = ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dmcd);
        } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
        assert(!ret);
        //assert(dmcd.pitch==(fmt==MPP_FMT_YUV420SP?hor_stride:hor_stride*10/8));
        //assert(dmcd.size==(fmt == MPP_FMT_YUV420SP?hor_stride:hor_stride*10/8)*ver_stride*2);
        mpi.frame_to_drm[i].handle = dmcd.handle;

        // commit DRM buffer to frame group
        struct drm_prime_handle dph;
        memset(&dph, 0, sizeof(struct drm_prime_handle));
        dph.handle = dmcd.handle;
        dph.fd = -1;
        // added for mmap
        dph.flags  = DRM_CLOEXEC | DRM_RDWR;
        do {
            ret = ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph);
        } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
        assert(!ret);
        //
        void * primed_framebuffer=mmap(
                0, dmcd.size,    PROT_READ | PROT_WRITE, MAP_SHARED,
                dph.fd, 0);
        if (primed_framebuffer == NULL || primed_framebuffer == MAP_FAILED) {
            printf(
                    "Could not map buffer exported through PRIME : %s (%d)\n"
                    "Buffer : %p\n",
                    strerror(ret), ret,
                    primed_framebuffer
            );
            assert(false);
        }
        mpi.frame_to_drm[i].memory_mmap=primed_framebuffer;
        mpi.frame_to_drm[i].memory_mmap_size=dmcd.size;
        printf("Buffer size bytes %d\n",(int)dmcd.size);
        //
        if(i!=0){
            MppBufferInfo info;
            memset(&info, 0, sizeof(info));
            info.type = MPP_BUFFER_TYPE_DRM;
            info.size = dmcd.width*dmcd.height;
            info.fd = dph.fd;
            ret = mpp_buffer_commit(mpi.frm_grp, &info);
            assert(!ret);
            mpi.frame_to_drm[i].prime_fd = info.fd; // dups fd
            if (dph.fd != info.fd) {
                ret = close(dph.fd);
                assert(!ret);
            }
        }else{
            mpi.frame_to_drm[i].prime_fd =  dph.fd; // dups fd
        }
        // allocate DRM FB from DRM buffer
        uint32_t handles[4], pitches[4], offsets[4];
        memset(handles, 0, sizeof(handles));
        memset(pitches, 0, sizeof(pitches));
        memset(offsets, 0, sizeof(offsets));
        handles[0] = mpi.frame_to_drm[i].handle;
        offsets[0] = 0;
        pitches[0] = hor_stride;
        handles[1] = mpi.frame_to_drm[i].handle;
        offsets[1] = pitches[0] * ver_stride;
        pitches[1] = pitches[0];
        ret = drmModeAddFB2(drm_fd, output_list->video_frm_width, output_list->video_frm_height, DRM_FORMAT_NV12, handles, pitches, offsets, &mpi.frame_to_drm[i].fb_id, 0);
        assert(!ret);
    }

    // register external frame group
    ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_EXT_BUF_GROUP, mpi.frm_grp);
    ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);

    ret = modeset_perform_modeset(drm_fd, output_list, output_list->video_request, &output_list->video_plane, mpi.frame_to_drm[0].fb_id, output_list->video_frm_width, output_list->video_frm_height, video_zpos);
    assert(ret >= 0);
}

// __FRAME_THREAD__
//
// - allocate DRM buffers and DRM FB based on frame size
// - pick frame in blocking mode and output to screen overlay

void *__FRAME_THREAD__(void *param)
{
    SchedulingHelper::set_thread_params_max_realtime("FRAME_THREAD",SchedulingHelper::PRIORITY_REALTIME_MID);
	int ret;
	int i;	
	MppFrame  frame  = NULL;
	int frid = 0;

	while (!frm_eos) {
		struct timespec ts, ats;
		
		assert(!frame);
		ret = mpi.mpi->decode_get_frame(mpi.ctx, &frame);
		assert(!ret);
		clock_gettime(CLOCK_MONOTONIC, &ats);
		if (frame) {
			if (mpp_frame_get_info_change(frame)) {
				// NOTE: This assertion will fail if the resolution has changed during decode - right now that's what we want since
                // it is the easiest way to break out (and restart, with the new resolution)
				assert(!mpi.frm_grp);
                if(develop_rendering_mode==10){
                    initialize_output_buffers_ion(frame);
                }else if(develop_rendering_mode==6) {
                    initialize_output_buffers_memcpy(frame);
                }else {
                    initialize_output_buffers(frame);
                }
			} else {
				// regular frame received
				if (!mpi.first_frame_ts.tv_sec) {
					ts = ats;
					mpi.first_frame_ts = ats;
				}

				MppBuffer buffer = mpp_frame_get_buffer(frame);					
				if (buffer) {
                    //printf("Got frame\n");
					output_list->video_poc = mpp_frame_get_poc(frame);

                    uint64_t feed_data_ts=mpp_frame_get_pts(frame);
                    uint64_t decoding_latency=get_time_ms()-feed_data_ts;
                    accumulate_and_print("Decode",decoding_latency,&m_decoding_latency);
                    //print_time_ms("Decode",decoding_latency);

                    if(develop_rendering_mode==10){
                        // Never commit anything in the display thread
                    }else{
                        // find fb_id by frame prime_fd
                        MppBufferInfo info;
                        ret = mpp_buffer_info_get(buffer, &info);
                        assert(!ret);
                        for (i=0; i<MAX_FRAMES; i++) {
                            if (mpi.frame_to_drm[i].prime_fd == info.fd) break;
                        }
                        assert(i!=MAX_FRAMES);

                        ts = ats;
                        frid++;

                        // send DRM FB to display thread
                        ret = pthread_mutex_lock(&video_mutex);
                        assert(!ret);
                        if (output_list->video_fb_id) output_list->video_skipped_frames++;
                        output_list->video_fb_id = mpi.frame_to_drm[i].fb_id;
                        output_list->video_fb_index=i;
                        output_list->decoding_pts=feed_data_ts;
                        ret = pthread_mutex_unlock(&video_mutex);
                        assert(!ret);
                        ret = pthread_cond_signal(&video_cond);
                        assert(!ret);

                    }
				}
			}
			
			frm_eos = mpp_frame_get_eos(frame);
			mpp_frame_deinit(&frame);
			frame = NULL;
		} else assert(0);
	}
	printf("Frame thread done.\n");
}

void *__DISPLAY_THREAD__(void *param)
{
    // With the proper rendering mode(s) this thread
    // doesn't hog the CPU
    SchedulingHelper::set_thread_params_max_realtime("DisplayThread",SchedulingHelper::PRIORITY_REALTIME_LOW);
	int ret;	
	int frame_counter = 0;
	uint64_t latency_avg[200];
	uint64_t min_latency = 1844674407370955161; // almost MAX_uint64_t
	uint64_t max_latency = 0;
    struct timespec fps_start, fps_end;
	clock_gettime(CLOCK_MONOTONIC, &fps_start);

	while (!frm_eos) {
		int fb_id;
        int fb_index;
		
		ret = pthread_mutex_lock(&video_mutex);
		assert(!ret);
		while (output_list->video_fb_id==0) {
			pthread_cond_wait(&video_cond, &video_mutex);
			assert(!ret);
			if (output_list->video_fb_id == 0 && frm_eos) {
				ret = pthread_mutex_unlock(&video_mutex);
				assert(!ret);
				goto end;
			}
		}
		struct timespec ts, ats;
		clock_gettime(CLOCK_MONOTONIC, &ats);
		fb_id = output_list->video_fb_id;
        fb_index=output_list->video_fb_index;
        uint64_t decoding_pts=output_list->decoding_pts;
		if (output_list->video_skipped_frames) 
			printf("Display skipped %d frame.\n", output_list->video_skipped_frames);
		output_list->video_fb_id=0;
		output_list->video_skipped_frames=0;
		ret = pthread_mutex_unlock(&video_mutex);
		assert(!ret);

        if(develop_rendering_mode==0){
            // show DRM FB in plane
            drmModeAtomicSetCursor(output_list->video_request, 0);
            ret = set_drm_object_property(output_list->video_request, &output_list->video_plane, "FB_ID", fb_id);
            assert(ret>0);

            //ret = pthread_mutex_lock(&osd_mutex);
            //assert(!ret);
            //ret = set_drm_object_property(output_list->video_request, &output_list->osd_plane, "FB_ID", output_list->osd_bufs[output_list->osd_buf_switch].fb);
            //assert(ret>0);
            //  DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_ASYNC
            drmModeAtomicCommit(drm_fd, output_list->video_request, DRM_MODE_ATOMIC_NONBLOCK |  DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
        }else if(develop_rendering_mode==1){
            static bool logged_once=false;
            if(!logged_once){
                printf("Not calling drmModeAtomicCommit\n");
                logged_once=true;
            }
        }else if(develop_rendering_mode==2){
            uint64_t before=get_time_ms();
            drmModeSetCrtc(
                    drm_fd, output_list->saved_crtc->crtc_id, fb_id,
                    0, 0,
                    &output_list->connector.id,
                    1,
                    &output_list->saved_crtc->mode);
            uint64_t elapsed_crtc=get_time_ms()-before;
            print_time_ms("drmModeSetCrtc took",elapsed_crtc);
        }else if (develop_rendering_mode==3){
            uint64_t before=get_time_ms();
            bool waiting=true;
            //DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_ATOMIC_ALLOW_MODESET
            // ... breaks running qopenhd ...
            drmModePageFlip(drm_fd, output_list->saved_crtc->crtc_id, fb_id,
                            DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC,&waiting);
            uint64_t elapsed_crtc=get_time_ms()-before;
            drmEventContext ev;
            memset(&ev, 0, sizeof(ev));
            ev.version = DRM_EVENT_CONTEXT_VERSION;
            ev.page_flip_handler = NULL;
            ret = drmHandleEvent(drm_fd, &ev);
            print_time_ms("drmModePageFlip took",elapsed_crtc);
        }else if(develop_rendering_mode==4){
            // Unmodded kernel: dual show, 30fps video
            uint64_t before=get_time_ms();
            ret = modeset_perform_modeset(drm_fd,
                output_list, output_list->video_request, &output_list->video_plane,
                fb_id,
                output_list->video_frm_width,
                output_list->video_frm_height,
                video_zpos);
                assert(ret >= 0);
            uint64_t elapsed_modeset=get_time_ms()-before;
            print_time_ms("modeset_perform_modeset took",elapsed_modeset);
	    }else if(develop_rendering_mode==5){
            // Unmodded kernel: dual show, 30fps video
            // maybe a bit more than 30fps
            uint64_t before=get_time_ms();
            ret = drmModeSetPlane(
                    drm_fd,
                    output_list->video_plane.id,
                    output_list->saved_crtc->crtc_id,
                    fb_id,
                    0,
                    0, 0,
                    output_list->video_crtc_width, output_list->video_crtc_height,
                    0, 0,
                    ((uint16_t) output_list->video_frm_width) << 16, ((uint16_t) output_list->video_frm_height) << 16
            );
            uint64_t elapsed_modeset=get_time_ms()-before;
            accumulate_and_print("drmModeSetPlane",elapsed_modeset,&m_drm_mode_set_plane_latency);
            //print_time_ms("drmModeSetPlane took",elapsed_modeset);
        }else if(develop_rendering_mode==6){
            // memcpy
            uint64_t before=get_time_ms();
            if(fb_index!=0){
                /*void* in_buffer_p=mpi.frame_to_drm[i].memory_mmap;
                void* out_buffer_p=mpi.frame_to_drm[0].memory_mmap;
                memcpy(out_buffer_p,in_buffer_p,mpi.frame_to_drm[0].memory_mmap_size/2);*/
                int fd_src=mpi.frame_to_drm[fb_index].prime_fd;
                int fd_dst=mpi.frame_to_drm[0].prime_fd;
                map_copy_unmap(fd_src,fd_dst,mpi.frame_to_drm[0].memory_mmap_size);
            }
            uint64_t elapsed_memcpy=get_time_ms()-before;
            //print_time_ms("memcpy took",elapsed_memcpy);
        }else if(develop_rendering_mode==7){
            // FPS - ?, no qopenhd
            uint64_t before=get_time_ms();
            extra_modeset_set_fb(drm_fd, output_list,&output_list->video_plane,
                                 fb_id);
            uint64_t elapsed_modeset=get_time_ms()-before;
            print_time_ms("extra_modeset_perform_modeset took",elapsed_modeset);
        }
        else{
            printf("Unknown rendering mdoe\n");
        }
		//ret = pthread_mutex_unlock(&osd_mutex);

        uint64_t decode_and_handover_display_ms=get_time_ms()-decoding_pts;
        accumulate_and_print("D&Display",decode_and_handover_display_ms,&m_decode_and_handover_display_latency);
        
		//assert(!ret);
		frame_counter++;

		clock_gettime(CLOCK_MONOTONIC, &fps_end);
		uint64_t time_us=(fps_end.tv_sec - fps_start.tv_sec)*1000000ll + ((fps_end.tv_nsec - fps_start.tv_nsec)/1000ll) % 1000000ll;
		if (time_us >= osd_vars.refresh_frequency_ms*1000) {
			uint64_t sum = 0;
			for (int i = 0; i < frame_counter; ++i) {
				sum += latency_avg[i];
				if (latency_avg[i] > max_latency) {
					max_latency = latency_avg[i];
				}
				if (latency_avg[i] < min_latency) {
					min_latency = latency_avg[i];
				}
			}
			osd_vars.latency_avg = sum / (frame_counter);
			osd_vars.latency_max = max_latency;
			osd_vars.latency_min = min_latency;
			osd_vars.current_framerate = frame_counter*(1000/osd_vars.refresh_frequency_ms);

			// printf("decoding decoding latency=%.2f ms (%.2f, %.2f), framerate=%d fps\n", osd_vars.latency_avg/1000.0, osd_vars.latency_max/1000.0, osd_vars.latency_min/1000.0, osd_vars.current_framerate);
			
			fps_start = fps_end;
			frame_counter = 0;
			max_latency = 0;
			min_latency = 1844674407370955161;
		}
        // NOTE: BUG video_poc out of bounds
		//struct timespec rtime = frame_stats[output_list->video_poc];
		//latency_avg[frame_counter] = (fps_end.tv_sec - rtime.tv_sec)*1000000ll + ((fps_end.tv_nsec - rtime.tv_nsec)/1000ll) % 1000000ll;
		//printf("decoding current_latency=%.2f ms\n",  latency_avg[frame_counter]/1000.0);
		
	}
end:	
	printf("Display thread done.\n");
}

// signal

int signal_flag = 0;

void sig_handler(int signum)
{
	printf("Received signal %d\n", signum);
	signal_flag++;
	mavlink_thread_signal++;
	osd_thread_signal++;
}

int mpp_split_mode = 0;

int enable_dvr = 0;
const char * dvr_file;

int read_rtp_stream(int port, MppPacket *packet, uint8_t* nal_buffer) {
	// Create socket
  	int socketFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in address;
	memset(&address, 0x00, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	bind(socketFd, (struct sockaddr*)&address, sizeof(struct sockaddr_in));

	if (fcntl(socketFd, F_SETFL, O_NONBLOCK) == -1) {
		printf("ERROR: Unable to set non-blocking mode\n");
		return 1;
	}

	printf("listening on socket %d\n", port);

	uint8_t* rx_buffer = (uint8_t*)malloc(READ_BUF_SIZE);
    
	int nalStart = 0;
	int poc = 0;
	int ret = 0;
	struct timespec recv_ts;
	long long bytesReceived = 0; 
	uint8_t* nal;

	FILE *out_h265 = NULL;
	if (enable_dvr) {
		if ((out_h265 = fopen(dvr_file,"w")) == NULL){
			printf("ERROR: unable to open %s\n", dvr_file);
		}
	}

	struct timespec bw_start, bw_end;
	clock_gettime(CLOCK_MONOTONIC, &bw_start);
	while (!signal_flag) {
		ssize_t rx = recv(socketFd, rx_buffer+8, 4096, 0);
		clock_gettime(CLOCK_MONOTONIC, &bw_end);
		uint64_t time_us=(bw_end.tv_sec - bw_start.tv_sec)*1000000ll + ((bw_end.tv_nsec - bw_start.tv_nsec)/1000ll) % 1000000ll;
		if (rx > 0) {
			bytesReceived += rx;
		}
		if (time_us >= 1000000) {
			bw_start = bw_end;
			osd_vars.bw_curr = (osd_vars.bw_curr + 1) % 10;
			osd_vars.bw_stats[osd_vars.bw_curr] = bytesReceived;
			bytesReceived = 0;
		}
		if (rx <= 0) {
			usleep(1);
			continue;
		}
		if (nal) {
			clock_gettime(CLOCK_MONOTONIC, &recv_ts);
		}
		uint32_t rtp_header = 0;
		if (rx_buffer[8] & 0x80 && rx_buffer[9] & 0x60) {
			rtp_header = 12;
		}

		// Decode frame
		uint32_t nal_size = 0;
		nal = decode_frame(rx_buffer + 8, rx, rtp_header, nal_buffer, &nal_size);
		if (!nal) {
			continue;
		}

		if (nal_size < 5) {
			printf("> Broken frame\n");
			break;
		}

	
		uint8_t nal_type_hevc = (nal[4] >> 1) & 0x3F;
		if (nalStart==0 && nal_type_hevc == 1) { //hevc
			continue;
		}
		nalStart = 1;
		if (nal_type_hevc == 19) {
			poc = 0;
		}
		frame_stats[poc]=recv_ts;

		mpp_packet_set_pos(packet, nal); // only needed once
		mpp_packet_set_length(packet, nal_size);

		// send packet until it success
		while (!signal_flag && MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) {
				usleep(10000);
		}
		poc ++;

		if (out_h265 != NULL) {
			fwrite(nal, nal_size, 1, out_h265);
		}
	};
	mpp_packet_set_eos(packet);
	mpp_packet_set_pos(packet, nal_buffer);
	mpp_packet_set_length(packet, 0);
	while (MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) {
		usleep(10000);
	}

	if (out_h265 != NULL) {
		fclose(out_h265);
	}
}


void read_gstreamerpipe_stream(MppPacket *packet){
    assert(gst_udp_port!=-1);
    GstRtpReceiver receiver{gst_udp_port,decode_h265 ? 1 : 0};
    int decoder_stalled_count=0;
    auto cb=[&packet,&decoder_stalled_count](std::shared_ptr<std::vector<uint8_t>> frame){
        //printf("Got data \n");
        // Let the gst pull thread run at quite high priority
        static bool first= false;
        if(first){
            SchedulingHelper::set_thread_params_max_realtime("DisplayThread",SchedulingHelper::PRIORITY_REALTIME_LOW);
            first= false;
        }
        void* data_p=frame->data();
        int data_len=frame->size();
        mpp_packet_set_data(packet, data_p);
        mpp_packet_set_size(packet, data_len);
        mpp_packet_set_pos(packet, data_p);
        mpp_packet_set_length(packet, data_len);
        mpp_packet_set_pts(packet,(RK_S64) get_time_ms());
        // Feed the data to mpp until either timeout (in which case the decoder might have stalled)
        // or success
        uint64_t data_feed_begin = get_time_ms();
        int ret=0;
        while (!signal_flag && MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) {
            uint64_t elapsed = get_time_ms() - data_feed_begin;
            if (elapsed > 100) {
                decoder_stalled_count++;
                printf("Cannot feed decoder, stalled %d ?\n",decoder_stalled_count);
                break;
            }
            usleep(2 * 1000);
        }
    };
    receiver.start_receiving(cb);
    while (!signal_flag){
        sleep(1);
    }
    receiver.stop_receiving();
    printf("Feeding eos\n");
    mpp_packet_set_eos(packet);
    //mpp_packet_set_pos(packet, nal_buffer);
    mpp_packet_set_length(packet, 0);
    int ret=0;
    while (MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) {
        usleep(10000);
    }
};


int read_filesrc_stream(MppPacket *packet) {
    /*FILE* fp = fopen("urghs.h264", "rb");
    if(!fp){
        printf("File not found\n");
        return 0;
    }*/
    FILE* fp=stdin;
    int filedesc=fileno(fp);
    assert(filedesc==0);
    //fcntl(filedesc, F_SETFL, fcntl(filedesc, F_GETFL) | O_NONBLOCK);
    /*fd_set set;
    struct timeval timeout;
    FD_ZERO(&set);
    FD_SET(filedesc, &set);
    timeout.tv_sec = 0;
    timeout.tv_usec = 20*1000;*/
    uint8_t data[READ_BUF_SIZE];
    void* data_p=&data;
    int data_len=0;
    int ret = 0;
    while (!signal_flag){
        /*int rv = select(filedesc + 1, &set, NULL, NULL, &timeout);
        if(rv == -1){
            perror("select\n");
        }else if(rv == 0) {
            //printf("timeout\n");
        }else {*/
            //printf("Data\n");
            // data to read
            //data_len = fread(data_p, 1, READ_BUF_SIZE, fp);
            //data_len = read(filedesc,data_p,READ_BUF_SIZE);
            data_len = read(STDIN_FILENO,data_p,READ_BUF_SIZE);
            if (data_len > 0) {
                //printf("Got %d\n",data_len);
                mpp_packet_set_data(packet, data_p);
                mpp_packet_set_size(packet, data_len);
                mpp_packet_set_pos(packet, data_p);
                mpp_packet_set_length(packet, data_len);
                mpp_packet_set_pts(packet,get_time_ms());
                // Feed the data to mpp until either timeout (in which case the decoder might have stalled)
                // or success
                uint64_t data_feed_begin = get_time_ms();
                while (!signal_flag && MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) {
                    uint64_t elapsed = get_time_ms() - data_feed_begin;
                    if (elapsed > 100) {
                        printf("Cannot feed decoder, stalled ?\n");
                        break;
                    }
                    usleep(2 * 1000);
                }
            }else{
                usleep(2 * 1000);
                //printf("fread should not fail after successfull select\n");
            }
        //}
    }
    printf("Feeding eos\n");
    mpp_packet_set_eos(packet);
    //mpp_packet_set_pos(packet, nal_buffer);
    mpp_packet_set_length(packet, 0);
    while (MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) {
        usleep(10000);
    }
    return 0;
}

void printHelp() {
  printf(
    "\n\t\tFPVue FPV Decoder for Rockchip (%s)\n"
    "\n"
    "  Usage:\n"
    "    fpvue [Arguments]\n"
    "\n"
    "  Arguments:\n"
    "    -p [Port]         	- Listen port                           (Default: 5600)\n"
    "\n"
    "    --osd          	- Enable OSD\n"
    "\n"
    "    --osd-elements 	- Customize osd elements   			    (Default: video,wfbng)\n"
    "\n"
    "    --osd-refresh  	- Defines the delay between osd refresh (Default: 1000 ms)\n"
    "\n"
    "    --dvr             	- Save the video feed (no osd) to the provided filename\n"
    "\n"
    "    --mpp-split-mode  	- Enable rockchip MPP_DEC_SET_PARSER_SPLIT_MODE, required when the video stream uses slices\n"
    "\n"
    "    --screen-mode      - Override default screen mode. ex:1920x1080@120\n"
    "\n"
    "    --h265      - Decode h265. H264 is default. \n"
    "\n"
    "    --gst-udp-port      - use internal gst for decoding, specifies the udp port for rtp in. Otherwise, fd needs to be provided. \n"
    "\n"
    "    --rmode      - different rendering modes for development \n"
    "\n", __DATE__
  );
}

void set_control_verbose(MppApi * mpi,  MppCtx ctx,MpiCmd control,RK_U32 enable){
    RK_U32 res = mpi->control(ctx, control, &enable);
    if(res){
        printf("Could not set control %d %d\n",control,enable);
        assert(false);
    }
}

void set_mpp_decoding_parameters(MppApi * mpi,  MppCtx ctx) {
    // config for runtime mode
    MppDecCfg cfg       = NULL;
    mpp_dec_cfg_init(&cfg);
    // get default config from decoder context
    int ret = mpi->control(ctx, MPP_DEC_GET_CFG, cfg);
    if (ret) {
        printf("%p failed to get decoder cfg ret %d\n", ctx, ret);
        assert(false);
    }
    // split_parse is to enable mpp internal frame spliter when the input
    // packet is not aplited into frames.
    RK_U32 need_split   = 1;
    ret = mpp_dec_cfg_set_u32(cfg, "base:split_parse", need_split);
    if (ret) {
        printf("%p failed to set split_parse ret %d\n", ctx, ret);
        assert(false);
    }
    ret = mpi->control(ctx, MPP_DEC_SET_CFG, cfg);
    if (ret) {
        printf("%p failed to set cfg %p ret %d\n", ctx, cfg, ret);
        assert(false);
    }
    set_control_verbose(mpi,ctx,MPP_DEC_SET_PARSER_SPLIT_MODE, mpp_split_mode ? 0xffff : 0);
    set_control_verbose(mpi,ctx,MPP_DEC_SET_DISABLE_ERROR, 0xffff);
    set_control_verbose(mpi,ctx,MPP_DEC_SET_IMMEDIATE_OUT, 0xffff);
    set_control_verbose(mpi,ctx,MPP_DEC_SET_ENABLE_FAST_PLAY, 0xffff);
    //set_control_verbose(mpi,ctx,MPP_DEC_SET_ENABLE_DEINTERLACE, 0xffff);
    // Docu fast mode:
    // and improve the
    // parallelism of decoder hardware and software
    // we probably don't want that, since we don't need pipelining to hit our bitrate(s)
    int fast_mode = 0;
    set_control_verbose(mpi,ctx,MPP_DEC_SET_PARSER_FAST_MODE,fast_mode);
}

// main

int main(int argc, char **argv)
{
	int ret;	
	int i, j;
	int enable_osd = 0;
	int enable_mavlink = 0;
	uint16_t listen_port = 5600;
	uint16_t mavlink_port = 14550;
	uint16_t mode_width = 0;
	uint16_t mode_height = 0;
	uint32_t mode_vrefresh = 0;
	// Load console arguments
	__BeginParseConsoleArguments__(printHelp) 
	
	__OnArgument("-p") {
		listen_port = atoi(__ArgValue);
		continue;
	}

	__OnArgument("--mavlink-port") {
		mavlink_port = atoi(__ArgValue);
		continue;
	}

	__OnArgument("--dvr") {
		enable_dvr = 1;
		dvr_file = __ArgValue;
		continue;
	}

	__OnArgument("--osd") {
		enable_osd = 1;
		osd_vars.plane_zpos = 2;
		osd_vars.enable_latency = mpp_split_mode == 1 ? 0 : 1;
		if (osd_vars.refresh_frequency_ms == 0 ){
			osd_vars.refresh_frequency_ms = 1000;
		} 
		osd_vars.enable_video = 1;
		osd_vars.enable_wfbng = 1;
		enable_mavlink = 1;
		continue;
	}

	__OnArgument("--osd-refresh") {
		osd_vars.refresh_frequency_ms = atoi(__ArgValue);
		continue;
	}

	__OnArgument("--osd-elements") {
		osd_vars.enable_video = 0;
		osd_vars.enable_wfbng = 0;
		const char* elements = __ArgValue;
		const char* element = strtok((char*)elements, ",");
		while( element != NULL ) {
			if (!strcmp(element, "video")) {
				osd_vars.enable_video = 1;
			} else if (!strcmp(element, "wfbng")) {
				osd_vars.enable_wfbng = 1;
				enable_mavlink = 1;
			}
			element = strtok(NULL, ",");
		}
		continue;
	}

	__OnArgument("--mpp-split-mode") {
		mpp_split_mode = 1;
		osd_vars.enable_latency = 0;
		continue;
	}
	
	__OnArgument("--screen-mode") {
		const char* mode = __ArgValue;
		mode_width = atoi(strtok((char*)mode, "x"));
		mode_height = atoi(strtok(NULL, "@"));
		mode_vrefresh = atoi(strtok(NULL, "@"));
		continue;
	}

    __OnArgument("--h265") {
        decode_h265=true;
        continue;
    }
    __OnArgument("--gst-udp-port") {
        gst_udp_port=atoi(__ArgValue);
        continue;
    }
    __OnArgument("--rmode") {
        const char* mode = __ArgValue;
        develop_rendering_mode= atoi((char*)mode);
        continue;
    }

	__EndParseConsoleArguments__

	if (enable_osd == 0 ) {
		video_zpos = 4;
	}

    // H264 or H265
    MppCodingType mpp_type = MPP_VIDEO_CodingAVC;
    if(decode_h265){
        printf("Decoding h265\n");
        mpp_type = MPP_VIDEO_CodingHEVC;
    }else{
        printf("Decoding h264 (default)\n");
    }
    printf("Rendering mode %d\n",develop_rendering_mode);
	//MppCodingType mpp_type = MPP_VIDEO_CodingHEVC;
    //MppCodingType mpp_type = MPP_VIDEO_CodingAVC;
	ret = mpp_check_support_format(MPP_CTX_DEC, mpp_type);
	assert(!ret);

	////////////////////////////////// SIGNAL SETUP

	signal(SIGINT, sig_handler);
	signal(SIGPIPE, sig_handler);
	
	//////////////////////////////////  DRM SETUP
	ret = modeset_open(&drm_fd, "/dev/dri/card0");
	if (ret < 0) {
		printf("modeset_open() =  %d\n", ret);
	}
	assert(drm_fd >= 0);
	output_list = (struct modeset_output *)malloc(sizeof(struct modeset_output));
	ret = modeset_prepare(drm_fd, output_list, mode_width, mode_height, mode_vrefresh);
	assert(!ret);
	
	////////////////////////////////// MPI SETUP
	MppPacket packet;

	uint8_t* nal_buffer = (uint8_t*)malloc(READ_BUF_SIZE);
	assert(nal_buffer);
	ret = mpp_packet_init(&packet, nal_buffer, READ_BUF_SIZE);
	assert(!ret);

	ret = mpp_create(&mpi.ctx, &mpi.mpi);
	assert(!ret);
    set_mpp_decoding_parameters(mpi.mpi,mpi.ctx);
	ret = mpp_init(mpi.ctx, MPP_CTX_DEC, mpp_type);
    assert(!ret);
    set_mpp_decoding_parameters(mpi.mpi,mpi.ctx);

	// blocked/wait read of frame in thread
	int param = MPP_POLL_BLOCK;
	ret = mpi.mpi->control(mpi.ctx, MPP_SET_OUTPUT_BLOCK, &param);
	assert(!ret);

 	//////////////////// THREADS SETUP
	
	ret = pthread_mutex_init(&video_mutex, NULL);
	assert(!ret);
	ret = pthread_cond_init(&video_cond, NULL);
	assert(!ret);

	pthread_t tid_frame, tid_display, tid_osd, tid_mavlink;
	ret = pthread_create(&tid_frame, NULL, __FRAME_THREAD__, NULL);
	assert(!ret);
	ret = pthread_create(&tid_display, NULL, __DISPLAY_THREAD__, NULL);
	assert(!ret);
	if (enable_osd) {
		if (enable_mavlink) {
			ret = pthread_create(&tid_mavlink, NULL, __MAVLINK_THREAD__, &signal_flag);
			assert(!ret);
		}
		osd_thread_params *args = (osd_thread_params *)malloc(sizeof *args);
        args->fd = drm_fd;
        args->out = output_list;
		ret = pthread_create(&tid_osd, NULL, __OSD_THREAD__, args);
		assert(!ret);
	}

	////////////////////////////////////////////// MAIN LOOP
	
	//read_rtp_stream(listen_port, packet, nal_buffer);
    if(gst_udp_port==-1){
        read_filesrc_stream((void**)packet);
    }else{
        read_gstreamerpipe_stream((void**)packet);
    }

	////////////////////////////////////////////// MPI CLEANUP

	ret = pthread_join(tid_frame, NULL);
	assert(!ret);
	
	ret = pthread_mutex_lock(&video_mutex);
	assert(!ret);	
	ret = pthread_cond_signal(&video_cond);
	assert(!ret);	
	ret = pthread_mutex_unlock(&video_mutex);
	assert(!ret);	

	ret = pthread_join(tid_display, NULL);
	assert(!ret);	
	
	ret = pthread_cond_destroy(&video_cond);
	assert(!ret);
	ret = pthread_mutex_destroy(&video_mutex);
	assert(!ret);

	if (enable_mavlink) {
		ret = pthread_join(tid_mavlink, NULL);
		assert(!ret);
	}
	if (enable_osd) {
		ret = pthread_join(tid_osd, NULL);
		assert(!ret);
	}

	ret = mpi.mpi->reset(mpi.ctx);
	assert(!ret);

	if (mpi.frm_grp) {
		ret = mpp_buffer_group_put(mpi.frm_grp);
		assert(!ret);
		mpi.frm_grp = NULL;
		for (i=0; i<MAX_FRAMES; i++) {
			ret = drmModeRmFB(drm_fd, mpi.frame_to_drm[i].fb_id);
			assert(!ret);
			struct drm_mode_destroy_dumb dmdd;
			memset(&dmdd, 0, sizeof(dmdd));
			dmdd.handle = mpi.frame_to_drm[i].handle;
			do {
				ret = ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmdd);
			} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
			assert(!ret);
		}
	}
		
	mpp_packet_deinit(&packet);
	mpp_destroy(mpi.ctx);
	free(nal_buffer);
	
	////////////////////////////////////////////// DRM CLEANUP
	restore_planes_zpos(drm_fd, output_list);
	drmModeSetCrtc(drm_fd,
			       output_list->saved_crtc->crtc_id,
			       output_list->saved_crtc->buffer_id,
			       output_list->saved_crtc->x,
			       output_list->saved_crtc->y,
			       &output_list->connector.id,
			       1,
			       &output_list->saved_crtc->mode);
	drmModeFreeCrtc(output_list->saved_crtc);
	drmModeAtomicFree(output_list->video_request);
	drmModeAtomicFree(output_list->osd_request);
	modeset_cleanup(drm_fd, output_list);
	close(drm_fd);

	return 0;
}
