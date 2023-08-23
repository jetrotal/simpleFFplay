﻿/*******************************************************************************
 * player.c
 *
 * history:
 *   2023-05-18 - [piaodazhu]     Improve: progress bar
 *   2023-05-18 - [piaodazhu]     Fix: return value unchecked
 *   2023-05-18 - [piaodazhu]     Improve: better print message
 *   2023-05-17 - [piaodazhu]     Improve: support seek
 *   2023-05-17 - [piaodazhu]     Improve: support window resize
 *   2023-05-17 - [piaodazhu]     Fix: audio cannot be paused
 *   2023-05-16 - [piaodazhu]     Fix: cannot exit normally
 *   2023-05-16 - [piaodazhu]     Fix: AVPacketList is deprecated
 * 
 *   2018-11-27 - [lei]     Create file: a simplest ffmpeg player
 *   2018-12-01 - [lei]     Playing audio
 *   2018-12-06 - [lei]     Playing audio & video
 *   2019-01-06 - [lei]     Add audio resampling, fix bug of unsupported audio 
 *                          format(such as planar)
 *   2019-01-16 - [lei]     Sync video to audio.
 *
 * details:
 *   A simple ffmpeg player.
 *
 * reference:
 *   ffplay.c in FFmpeg 4.1 project.
 *******************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

#include "player.h"
#include "frame.h"
#include "packet.h"
#include "demux.h"
#include "video.h"
#include "audio.h"

static player_stat_t *player_init(const char *p_input_file);
static int player_deinit(player_stat_t *is);

// Function: get_clock
// Returns the updated pts value of the previous frame (previous frame pts + elapsed time)
double get_clock(play_clock_t *c)
{
    if (*c->queue_serial != c->serial)
    {
        return NAN;
    }
    if (c->paused)
    {
        return c->pts;
    }
    else
    {
        double time = av_gettime_relative() / 1000000.0;
        double ret = c->pts_drift + time;   // Expanded: c->pts + (time - c->last_updated)
        return ret;
    }
}

void set_clock_at(play_clock_t *c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

void set_clock(play_clock_t *c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

static void set_clock_speed(play_clock_t *c, double speed)
{
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

void init_clock(play_clock_t *c, int *queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

static void sync_play_clock_to_slave(play_clock_t *c, play_clock_t *slave)
{
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
}

static void do_exit(player_stat_t *is)
{
    if (is)
    {
        player_deinit(is);
    }

    if (is->sdl_video.renderer)
        SDL_DestroyRenderer(is->sdl_video.renderer);
    if (is->sdl_video.window)
        SDL_DestroyWindow(is->sdl_video.window);
    
    avformat_network_deinit();

    SDL_Quit();
    av_log(NULL, AV_LOG_INFO, "\nQUIT\n");
    exit(0);
}

static player_stat_t *player_init(const char *p_input_file)
{
    player_stat_t *is;

    is = av_mallocz(sizeof(player_stat_t));
    if (!is)
    {
        return NULL;
    }

    is->filename = av_strdup(p_input_file);
    if (is->filename == NULL)
    {
        goto fail;
    }

    /* start video display */
    if (frame_queue_init(&is->video_frm_queue, &is->video_pkt_queue, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0 ||
        frame_queue_init(&is->audio_frm_queue, &is->audio_pkt_queue, SAMPLE_QUEUE_SIZE, 1) < 0)
    {
        goto fail;
    }

    if (packet_queue_init(&is->video_pkt_queue) < 0 ||
        packet_queue_init(&is->audio_pkt_queue) < 0)
    {
        goto fail;
    }

    packet_queue_put_nullpacket(&is->video_pkt_queue, is->video_idx);
    packet_queue_put_nullpacket(&is->audio_pkt_queue, is->audio_idx);

    if (!(is->continue_read_thread = SDL_CreateCond()))
    {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
fail:
        player_deinit(is);
        exit(1);
    }

    init_clock(&is->video_clk, &is->video_pkt_queue.serial);
    init_clock(&is->audio_clk, &is->audio_pkt_queue.serial);

    is->abort_request = 0;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
    {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    return is;
}


static int player_deinit(player_stat_t *is)
{
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    packet_queue_abort(&is->video_pkt_queue);
    packet_queue_abort(&is->audio_pkt_queue);
    
    SDL_WaitThread(is->read_tid, NULL);
    avformat_close_input(&is->p_fmt_ctx);

    
    SDL_WaitThread(is->audio_dec_tid, NULL);
    SDL_WaitThread(is->video_dec_tid, NULL);
    SDL_WaitThread(is->video_ply_tid, NULL);
    
    packet_queue_destroy(&is->video_pkt_queue);
    packet_queue_destroy(&is->audio_pkt_queue);

    /* free all pictures */
    frame_queue_destory(&is->video_frm_queue);
    frame_queue_destory(&is->audio_frm_queue);

    SDL_DestroyCond(is->continue_read_thread);
    sws_freeContext(is->img_convert_ctx);
    av_free(is->filename);
    if (is->sdl_video.texture)
    {
        SDL_DestroyTexture(is->sdl_video.texture);
    }

    av_free(is);
    return 0; 
}

/* pause or resume the video */
static void stream_toggle_pause(player_stat_t *is)
{
    if (is->paused)
    {
        // Here indicates the current state is paused, switch to playing state.
        // Before resuming, add the elapsed time during pause to frame_timer.
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->video_clk.last_updated;
        set_clock(&is->video_clk, get_clock(&is->video_clk), is->video_clk.serial);
    }
    is->paused = is->audio_clk.paused = is->video_clk.paused = !is->paused;
}

static void toggle_pause(player_stat_t *is)
{
    stream_toggle_pause(is);
    is->step = 0;
}

/* seek in the stream */
static void stream_seek(player_stat_t *is, int64_t pos, int64_t rel)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_req = 1;
        SDL_CondSignal(is->continue_read_thread);
    }
}

int time_str(double time, char *buf, int len) {
    if (len <= strlen("hh:mm:ss.ff")) {
        *buf = 0;
        return -1;
    }

    double integer = floor(time);
    double fractional = time - integer;
    
    int i = (int)integer;
    int f = (int)(100 * fractional);
    int hh = i / 3600;
    i %= 3600;
    int mm = i / 60;
    i %= 60;
    int ss = i;

    return snprintf(buf, len, "%02d:%02d:%02d.%02d", hh, mm, ss, f);
}

int progress_bar(double time, double total, char *buf, int len) {
    if (len <= 60) {
        *buf = 0;
        return -1;
    }
    int n = (int)ceil((time * 60) / total);
    int idx = 0;
    while (idx < n - 1) {
        buf[idx++] = '=';
    }
    buf[idx++] = '>';
    while (idx < 60) {
        buf[idx++] = '.';
    }
    buf[idx++] = 0;
    return 60;
}

int player_running(const char *p_input_file)
{
    player_stat_t *is = NULL;
    int ret;

    // Initialize queues, SDL system, and allocate player_stat_t structure.
    is = player_init(p_input_file);
    if (is == NULL)
    {
        do_exit(is);
    }

    // Demux the file.
    ret = open_demux(is);
    if (ret < 0) {
        do_exit(is);
    }

    // Open and decode the video stream for playback.
    ret = open_video(is);
    if (ret < 0) {
        do_exit(is);
    }

    // Open and decode the audio stream for playback.
    ret = open_audio(is);
    if (ret < 0) {
        do_exit(is);
    }

    SDL_Event event;
    double incr, pos;

    double duration, now;
    duration = (double)is->p_fmt_ctx->duration / AV_TIME_BASE;

    char totaltime[20], playtime[20], bar[80];
    time_str(duration, totaltime, 20);
    av_log(NULL, AV_LOG_INFO, "Control: \n\tquit:               <ESC>\n\tpause/unpause:      <SPACE>\n\tforward/backward:   <R/L/U/D>\n\n");
    while (1)
    {
        SDL_PumpEvents();
        // If the SDL event queue is empty, play video frames within this while loop.
        // If there's an event in the queue, exit this function and let the upper-level function handle the event.
        while (!SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT))
        {
            now = is->audio_clk.pts;
            if (isnan(now)) {
                now = 0.0;
            }

            time_str(now, playtime, 20);
            progress_bar(now, duration, bar, 80);

            av_log(NULL, AV_LOG_INFO, "[%s/%s] (%s) \r", playtime, totaltime, bar);

            av_usleep(100000);
            SDL_PumpEvents();
        }

        switch (event.type) {
        case SDL_KEYDOWN:
            if (event.key.keysym.sym == SDLK_ESCAPE) // Exit on ESC key.
            {
                do_exit(is);
                break;
            }

            switch (event.key.keysym.sym) {
            case SDLK_SPACE:        // Pause or resume playback on SPACE key.
                toggle_pause(is);
                break;
            case SDLK_LEFT:         // Seek backward on LEFT arrow key.
                incr = -10.0;
                goto do_seek;
            case SDLK_RIGHT:        // Seek forward on RIGHT arrow key.
                incr = 10.0;
                goto do_seek;
            case SDLK_UP:           // Fast forward on UP arrow key.
                incr = 60.0;
                goto do_seek;
            case SDLK_DOWN:         // Fast backward on DOWN arrow key.
                incr = -60.0;
            do_seek:
                    pos = is->audio_clk.pts;
                    pos += incr;
                    if (is->start_time != AV_NOPTS_VALUE && pos < is->start_time / (double)AV_TIME_BASE)
                        pos = is->start_time / (double)AV_TIME_BASE;
                    stream_seek(is, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE));
                break;
            default:
                break;
            }
            break;
        case SDL_WINDOWEVENT:
            // Handle window resizing to adjust video display.
            switch (event.window.event) {
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    is->sdl_video.window_width = event.window.data1;
                    is->sdl_video.window_height = event.window.data2;
                    if (is->sdl_video.window_width * is->sdl_video.height_width_ratio < (double)is->sdl_video.window_height) {
                        is->sdl_video.width = is->sdl_video.window_width;
                        is->sdl_video.height = (int)(is->sdl_video.window_width * is->sdl_video.height_width_ratio);
                    } else {
                        is->sdl_video.height = is->sdl_video.window_height;
                        is->sdl_video.width = (int)(is->sdl_video.window_height / is->sdl_video.height_width_ratio);
                    }
            }
            break;
        case SDL_QUIT:
        case FF_QUIT_EVENT:
            do_exit(is);
            break;
        default:
            break;
        }
    }

    return 0;
}
