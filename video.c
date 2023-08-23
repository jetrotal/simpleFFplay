#include "video.h"
#include "packet.h"
#include "frame.h"
#include "player.h"

static int queue_picture(player_stat_t *is, AVFrame *src_frame, double pts, double duration, int64_t pos)
{
    frame_t *vp;
    if (!(vp = frame_queue_peek_writable(&is->video_frm_queue)))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    //vp->serial = serial;

    // Copy AVFrame into the corresponding position in the queue
    av_frame_move_ref(vp->frame, src_frame);
    // Update queue count and write index
    frame_queue_push(&is->video_frm_queue);
    return 0;
}

// Decode a packet from the packet_queue and generate a frame
static int video_decode_frame(AVCodecContext *p_codec_ctx, packet_queue_t *p_pkt_queue, AVFrame *frame)
{
    int ret;
    
    while (1)
    {
        AVPacket pkt;

        while (1)
        {
            if (p_pkt_queue->abort_request) {
                av_log(NULL, AV_LOG_DEBUG, "video_decode_frame received quit\n");
                return -1;
            }
            // Step 3. Receive a frame from the decoder
            // 3.1 A video packet contains a video frame
            //     The decoder outputs frames after caching a certain number of packets
            //     Frames are output in the order of pts, such as IBBPBBP
            //     frame->pkt_pos holds the offset of the packet corresponding to this frame in the video file, same as pkt.pos
            ret = avcodec_receive_frame(p_codec_ctx, frame);
            if (ret < 0)
            {
                if (ret == AVERROR_EOF)
                {
                    av_log(NULL, AV_LOG_INFO, "video avcodec_receive_frame(): the decoder has been fully flushed\n");
                    avcodec_flush_buffers(p_codec_ctx);
                    return 0;
                }
                else if (ret == AVERROR(EAGAIN))
                {
                    // av_log(NULL, AV_LOG_INFO, "video avcodec_receive_frame(): output is not available in this state - user must try to send new input\n");
                    break;
                }
                else
                {
                    av_log(NULL, AV_LOG_ERROR, "video avcodec_receive_frame(): other errors\n");
                    continue;
                }
            }
            else
            {
                frame->pts = frame->best_effort_timestamp;
                //frame->pts = frame->pkt_dts;

                return 1;   // Successfully decoded a video frame or an audio frame, return
            }
        }
        // Step 1. Take a packet from the packet_queue. Assign the packet's serial to d->pkt_serial
        if (packet_queue_get(p_pkt_queue, &pkt, true) < 0)
        {
            av_log(NULL, AV_LOG_DEBUG, "get packet error\n");
            return -1;
        }

        if (pkt.data == NULL)
        {
            // Reset the decoder's internal state / flush internal buffers.
            avcodec_flush_buffers(p_codec_ctx);
        }
        else
        {
            // Step 2. Send the packet to the decoder
            //         Packets are sent in increasing dts order, such as IPBBPBB
            //         pkt.pos indicates the offset of the current packet in the video file
            if (avcodec_send_packet(p_codec_ctx, &pkt) == AVERROR(EAGAIN))
            {
                av_log(NULL, AV_LOG_ERROR, "receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
            }

            av_packet_unref(&pkt);
        }
    }
}

// Decode video packets to get video frames, then enqueue them to the picture queue
static int video_decode_thread(void *arg)
{
    player_stat_t *is = (player_stat_t *)arg;
    AVFrame *p_frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    int got_picture;
    AVRational tb = is->p_video_stream->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->p_fmt_ctx, is->p_video_stream, NULL);
    
    if (p_frame == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "av_frame_alloc() for p_frame failed\n");
        return AVERROR(ENOMEM);
    }

    while (1)
    {
        got_picture = video_decode_frame(is->p_vcodec_ctx, &is->video_pkt_queue, p_frame);
        if (got_picture < 0)
        {
            goto exit;
        }
        
        duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);   // Duration of the current frame
        pts = (p_frame->pts == AV_NOPTS_VALUE) ? NAN : p_frame->pts * av_q2d(tb);   // Presentation timestamp of the current frame
        ret = queue_picture(is, p_frame, pts, duration, p_frame->pkt_pos);   // Enqueue the current frame into the frame_queue
        av_frame_unref(p_frame);

        if (ret < 0)
        {
            goto exit;
        }

    }

exit:
    av_frame_free(&p_frame);

    return 0;
}

// Adjust the delay value based on the difference between the video clock and the sync clock (e.g., audio clock)
// The input parameter "delay" is the duration of the previous frame, indicating how long to wait before playing the current frame after the previous frame
// The return value "delay" is the adjusted value after correction
static double compute_target_delay(double delay, player_stat_t *is)
{
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */

    /* if video is slave, we try to correct big delays by
       duplicating or deleting a frame */
    // Difference between the video clock and the sync clock (e.g., audio clock), where the clock value is the pts of the previous frame (in reality: pts of the previous frame + elapsed time since the previous frame)
    diff = get_clock(&is->video_clk) - get_clock(&is->audio_clk);
    // "delay" is the duration of the previous frame: the theoretical difference between the display time of the current frame and the display time of the previous frame
    // "diff" is the difference between the video clock and the sync clock

    /* skip or repeat frame. We take into account the
       delay to compute the threshold. I still don't know
       if it is the best guess */
    // If delay < AV_SYNC_THRESHOLD_MIN, then sync_threshold is set to AV_SYNC_THRESHOLD_MIN
    // If delay > AV_SYNC_THRESHOLD_MAX, then sync_threshold is set to AV_SYNC_THRESHOLD_MAX
    // If AV_SYNC_THRESHOLD_MIN < delay < AV_SYNC_THRESHOLD_MAX, then sync_threshold is set to delay
    sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
    if (!isnan(diff))
    {
        if (diff <= -sync_threshold)        // Video clock lags behind the sync clock and the difference exceeds the sync threshold
            delay = FFMAX(0, delay + diff); // If the current frame's display time is behind the sync clock (delay+diff<0), set delay to 0 (video catching up, play immediately), otherwise delay=delay+diff
        else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)  // Video clock is ahead of the sync clock, and the difference exceeds the sync threshold, but the duration of the previous frame is too long
            delay = delay + diff;           // Adjust only by delay=delay+diff, mainly due to the effect of the AV_SYNC_FRAMEDUP_THRESHOLD parameter
        else if (diff >= sync_threshold)    // Video clock is ahead of the sync clock and the difference exceeds the sync threshold
            delay = 2 * delay;              // Slow down video playback, double the delay
    }

    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);

    return delay;
}

static double vp_duration(player_stat_t *is, frame_t *vp, frame_t *nextvp) {
    if (vp->serial == nextvp->serial)
    {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0)
            return vp->duration;
        else
            return duration;
    } else {
        return 0.0;
    }
}

static void update_video_pts(player_stat_t *is, double pts, int64_t pos, int serial) {
    /* update current video pts */
    set_clock(&is->video_clk, pts, serial);            // Update vidclock
    //-sync_clock_to_slave(&is->extclk, &is->vidclk);  // Synchronize extclock to vidclock
}

static void video_display(player_stat_t *is)
{
    frame_t *vp;

    vp = frame_queue_peek_last(&is->video_frm_queue);

    // Image conversion: p_frm_raw->data ==> p_frm_yuv->data
    // Update a destination image region using a processed source image region; the processed region must be contiguous line by line
    // Plane: For example, YUV has Y, U, V planes, and RGB has R, G, B planes
    // Slice: A contiguous region of lines in the image, must be continuous, ordered from top to bottom or bottom to top
    // Stride/pitch: Number of bytes in one row of the image, Stride=BytesPerPixel*Width+Padding, pay attention to alignment
    // AVFrame.*data[]: Each array element points to the corresponding plane
    // AVFrame.linesize[]: Each array element represents the number of bytes in one row of the corresponding plane
    sws_scale(is->img_convert_ctx,                    // sws context
              (const uint8_t *const *)vp->frame->data,// src slice
              vp->frame->linesize,                    // src stride
              0,                                      // src slice y
              is->p_vcodec_ctx->height,               // src slice height
              is->p_frm_yuv->data,                    // dst planes
              is->p_frm_yuv->linesize                 // dst strides
             );
    
    // Update Texture with new YUV pixel data

    is->sdl_video.rect.w = is->sdl_video.width;
    is->sdl_video.rect.h = is->sdl_video.height;
    is->sdl_video.rect.x = (is->sdl_video.window_width - is->sdl_video.width) /2;
    is->sdl_video.rect.y = (is->sdl_video.window_height - is->sdl_video.height) /2;

    SDL_UpdateYUVTexture(is->sdl_video.texture,         // sdl texture
                         NULL,
                         is->p_frm_yuv->data[0],        // y plane
                         is->p_frm_yuv->linesize[0],    // y pitch
                         is->p_frm_yuv->data[1],        // u plane
                         is->p_frm_yuv->linesize[1],    // u pitch
                         is->p_frm_yuv->data[2],        // v plane
                         is->p_frm_yuv->linesize[2]     // v pitch
                        );

    // Clear the current rendering target with a specific color
    SDL_RenderClear(is->sdl_video.renderer);

    // Copy the updated Texture to the specified rectangular area of the window (is->sdl_video.rect)
    SDL_RenderCopy(is->sdl_video.renderer,              // sdl renderer
                   is->sdl_video.texture,               // sdl texture
                   NULL,                                // src rect, if NULL copy texture
                   &is->sdl_video.rect                  // dst rect
                  );
    
    // Perform rendering to update the screen display
    SDL_RenderPresent(is->sdl_video.renderer);
}

/* called to display each frame */
static void video_refresh(void *opaque, double *remaining_time)
{
    player_stat_t *is = (player_stat_t *)opaque;
    double time;
    static bool first_frame = true;
    // av_log(NULL, AV_LOG_INFO, "frame_time=%f, video=%f, audio=%f\n", is->frame_timer, is->video_clk.pts, is->audio_clk.pts);
retry:
    if (frame_queue_nb_remaining(&is->video_frm_queue) == 0)
    {
        // nothing to do, no picture to display in the queue
        return;
    }

    double last_duration, duration, delay;
    frame_t *vp, *lastvp;

    /* dequeue the picture */
    lastvp = frame_queue_peek_last(&is->video_frm_queue);     // Previous frame: the last displayed frame
    vp = frame_queue_peek(&is->video_frm_queue);              // Current frame: the frame to be displayed

    // If lastvp and vp are not from the same playback sequence (a seek starts a new playback sequence), update frame_timer to the current time
    if (first_frame)
    {
        is->frame_timer = av_gettime_relative() / 1000000.0;
        first_frame = false;
    }

    // Pause handling: keep displaying the last displayed image
    if (is->paused)
        goto display;

    /* compute nominal last_duration */
    last_duration = vp_duration(is, lastvp, vp);        // Duration of the last frame displayed: vp->pts - lastvp->pts
    delay = compute_target_delay(last_duration, is);    // Calculate delay based on video clock and synchronization clock difference
    // av_log(NULL, AV_LOG_DEBUG, "%f,%f\n", last_duration, delay);

    time = av_gettime_relative() / 1000000.0;
    // If the current frame's playback time (is->frame_timer + delay) is greater than the current time (time), it's not time to play yet
    if (time < is->frame_timer + delay) {
        // If playback time has not arrived, update remaining_time to the time difference between current time and the next playback time
        *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
        // If playback time hasn't arrived, don't play the frame, just return
        return;
    }

    // Update frame_timer value
    is->frame_timer += delay;
    // Adjust frame_timer value: if frame_timer lags too far behind the current system time (more than AV_SYNC_THRESHOLD_MAX), update it to the current system time
    if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
    {
        is->frame_timer = time;
    }

    SDL_LockMutex(is->video_frm_queue.mutex);
    if (!isnan(vp->pts))
    {
        // av_log(NULL, AV_LOG_DEBUG, "update ts from %f to %f\n", is->video_clk.pts, vp->pts);
        update_video_pts(is, vp->pts, vp->pos, vp->serial); // Update video clock: timestamp, clock time
    }
    SDL_UnlockMutex(is->video_frm_queue.mutex);

    // Determine if we need to discard frames that couldn't be displayed in time
    if (frame_queue_nb_remaining(&is->video_frm_queue) > 1)  // Number of undisplayed frames in the queue > 1 (no need to drop frames if only one frame is present)
    {
        frame_t *nextvp = frame_queue_peek_next(&is->video_frm_queue);  // Next frame: the next frame to be displayed
        duration = vp_duration(is, vp, nextvp);             // Duration of the current frame vp: nextvp->pts - vp->pts
        // If the current frame vp couldn't be displayed in time, i.e., the playback time of the next frame (is->frame_timer + duration) is smaller than the current system time (time)
        if (time > is->frame_timer + duration)
        {
            frame_queue_next(&is->video_frm_queue);   // Remove the last displayed frame (lastvp), move the read pointer by 1 (from lastvp to vp)
            goto retry;
        }
    }

    // Remove the current read pointer element, move the read pointer by 1. If no frames are dropped, the read pointer goes from lastvp to vp. If frames are dropped, the read pointer goes from vp to nextvp
    frame_queue_next(&is->video_frm_queue);

display:
    video_display(is);                      // Display the current frame vp (nextvp if frames were dropped)
}

static int video_playing_thread(void *arg)
{
    player_stat_t *is = (player_stat_t *)arg;
    double remaining_time = 0.0;

    while (1)
    {
        if (remaining_time > 0.0)
        {
            av_usleep((unsigned)(remaining_time * 1000000.0));
        }
        remaining_time = REFRESH_RATE;
        // Display the current frame immediately or after a delay of remaining_time
        video_refresh(is, &remaining_time);

        if (is->abort_request) {
            av_log(NULL, AV_LOG_DEBUG, "playing thread receive quit\n");
            break;
        }
    }

    return 0;
}

static int open_video_playing(void *arg)
{
    player_stat_t *is = (player_stat_t *)arg;
    int ret;
    int buf_size;
    uint8_t* buffer = NULL;

    is->p_frm_yuv = av_frame_alloc();
    if (is->p_frm_yuv == NULL)
    {
        av_log(NULL, AV_LOG_FATAL, "av_frame_alloc() for p_frm_raw failed\n");
        return -1;
    }

    // Manually allocate buffer for AVFrame.*data[] to store destination frame video data for sws_scale()
    buf_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, 
                                        is->p_vcodec_ctx->width, 
                                        is->p_vcodec_ctx->height, 
                                        1
                                        );
    // 'buffer' will be the video data buffer for 'p_frm_yuv'
    buffer = (uint8_t *)av_malloc(buf_size);
    if (buffer == NULL)
    {
        av_log(NULL, AV_LOG_FATAL, "av_malloc() for buffer failed\n");
        return -1;
    }
    // Set 'p_frm_yuv->data' and 'p_frm_yuv->linesize' using given parameters
    ret = av_image_fill_arrays(is->p_frm_yuv->data,     // dst data[]
                               is->p_frm_yuv->linesize, // dst linesize[]
                               buffer,                  // src buffer
                               AV_PIX_FMT_YUV420P,      // pixel format
                               is->p_vcodec_ctx->width, // width
                               is->p_vcodec_ctx->height,// height
                               1                        // align
                               );
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_FATAL, "av_image_fill_arrays() failed %d\n", ret);
        return -1;;
    }

    // A2. Initialize SWS context for future image conversion
    //     The 6th parameter here uses FFmpeg pixel formats, compared with comment B3
    //     Pixel format AV_PIX_FMT_YUV420P in FFmpeg corresponds to SDL pixel format SDL_PIXELFORMAT_IYUV
    //     If the decoded image format is not supported by SDL, image conversion is necessary for proper display
    //     If the decoded image format is supported by SDL, no need for image conversion
    //     Here, for simplicity, we convert all to a format supported by SDL: AV_PIX_FMT_YUV420P ==> SDL_PIXELFORMAT_IYUV
    is->img_convert_ctx = sws_getContext(is->p_vcodec_ctx->width,   // src width
                                         is->p_vcodec_ctx->height,  // src height
                                         is->p_vcodec_ctx->pix_fmt, // src format
                                         is->p_vcodec_ctx->width,   // dst width
                                         is->p_vcodec_ctx->height,  // dst height
                                         AV_PIX_FMT_YUV420P,        // dst format
                                         SWS_BICUBIC,               // flags
                                         NULL,                      // src filter
                                         NULL,                      // dst filter
                                         NULL                       // param
                                         );
    if (is->img_convert_ctx == NULL)
    {
        av_log(NULL, AV_LOG_FATAL, "sws_getContext() failed\n");
        return -1;
    }

    // Set SDL_Rect values
    is->sdl_video.rect.x = 0;
    is->sdl_video.rect.y = 0;
    is->sdl_video.rect.w = is->p_vcodec_ctx->width;
    is->sdl_video.rect.h = is->p_vcodec_ctx->height;

    // 1. Create SDL window, SDL 2.0 supports multiple windows
    //    SDL_Window is the video window that pops up when the program runs, analogous to SDL_Surface in SDL 1.x
    is->sdl_video.window = SDL_CreateWindow("simple ffplayer", 
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              is->sdl_video.rect.w, 
                              is->sdl_video.rect.h,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
                              );
    if (is->sdl_video.window == NULL)
    {  
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateWindow() failed: %s\n", SDL_GetError());  
        return -1;
    }

    // 2. Create SDL_Renderer
    //    SDL_Renderer: the renderer
    is->sdl_video.renderer = SDL_CreateRenderer(is->sdl_video.window, -1, 0);
    if (is->sdl_video.renderer == NULL)
    {  
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateRenderer() failed: %s\n", SDL_GetError());  
        return -1;
    }

    // 3. Create SDL_Texture
    //    An SDL_Texture corresponds to a frame of YUV data, similar to SDL_Overlay in SDL 1.x
   is->sdl_video.texture = SDL_CreateTexture(is->sdl_video.renderer, 
                                    SDL_PIXELFORMAT_IYUV, 
                                    SDL_TEXTUREACCESS_STREAMING,
                                    is->sdl_video.rect.w,
                                    is->sdl_video.rect.h
                                    );
    if (is->sdl_video.texture == NULL)
    {  
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateTexture() failed: %s\n", SDL_GetError());  
        return -1;
    }

    is->video_ply_tid = SDL_CreateThread(video_playing_thread, "video playing thread", is);

    return 0;
}

static int open_video_stream(player_stat_t *is)
{
    AVCodecParameters* p_codec_par = NULL;
    AVCodec* p_codec = NULL;
    AVCodecContext* p_codec_ctx = NULL;
    AVStream *p_stream = is->p_video_stream;
    int ret;

    // 1. Build decoder AVCodecContext for the video stream
    // 1.1 Get codec parameters AVCodecParameters for decoding
    p_codec_par = p_stream->codecpar;

    // 1.2 Get the decoder
    p_codec = avcodec_find_decoder(p_codec_par->codec_id);
    if (p_codec == NULL)
    {
        av_log(NULL, AV_LOG_FATAL, "Cannot find codec!\n");
        return -1;
    }

    // 1.3 Build decoder AVCodecContext
    // 1.3.1 Initialize p_codec_ctx: allocate structure and initialize relevant members to default values using p_codec
    p_codec_ctx = avcodec_alloc_context3(p_codec);
    if (p_codec_ctx == NULL)
    {
        av_log(NULL, AV_LOG_FATAL, "avcodec_alloc_context3() failed\n");
        return -1;
    }
    // 1.3.2 Initialize p_codec_ctx: Copy parameters from p_codec_par to p_codec_ctx and initialize relevant members
    ret = avcodec_parameters_to_context(p_codec_ctx, p_codec_par);
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_FATAL, "avcodec_parameters_to_context() failed\n");
        return -1;
    }
    // 1.3.3 Initialize p_codec_ctx: Initialize p_codec_ctx using p_codec, completing initialization
    ret = avcodec_open2(p_codec_ctx, p_codec, NULL);
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_FATAL, "avcodec_open2() failed %d\n", ret);
        return -1;
    }

    is->p_vcodec_ctx = p_codec_ctx;
    
    // 2. Create a video decoding thread
    is->video_dec_tid = SDL_CreateThread(video_decode_thread, "video decode thread", is);

    // 3. Record default display size
    is->sdl_video.width  = p_codec_par->width;
    is->sdl_video.height = p_codec_par->height;
    is->sdl_video.height_width_ratio = (double)(1.0 * is->sdl_video.height) / is->sdl_video.width;
    is->sdl_video.window_width = p_codec_par->width;
    is->sdl_video.window_height = p_codec_par->height;
    return 0;
}

int open_video(player_stat_t *is)
{
    int ret;
    // Initialize video decoder and start video decoding thread
    ret = open_video_stream(is);
    if (ret < 0) {
        return ret;
    }
    // Initialize image conversion context and renderer, start video display thread
    ret = open_video_playing(is);
    if (ret < 0) {
        return ret;
    }
    return 0;
}

