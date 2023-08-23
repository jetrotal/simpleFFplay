#include "demux.h"
#include "packet.h"

static int decode_interrupt_cb(void *ctx)
{
    player_stat_t *is = ctx;
    return is->abort_request;
}

static int demux_init(player_stat_t *is)
{
    AVFormatContext *p_fmt_ctx = NULL;
    int err, i, ret;
    int a_idx;
    int v_idx;

    p_fmt_ctx = avformat_alloc_context();
    if (!p_fmt_ctx)
    {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    // Interrupt callback mechanism. Provides a processing interface for the underlying I/O layer, e.g., to abort I/O operations.
    p_fmt_ctx->interrupt_callback.callback = decode_interrupt_cb;
    p_fmt_ctx->interrupt_callback.opaque = is;

    // 1. Build AVFormatContext
    // 1.1 Open the video file: Read the file header and store the file format information in the "fmt context."
    err = avformat_open_input(&p_fmt_ctx, is->filename, NULL, NULL);
    if (err < 0)
    {
        if (err == -2) {
            av_log(NULL, AV_LOG_FATAL, "URL %s not found\n", is->filename);
            exit(0);
        }

        av_log(NULL, AV_LOG_FATAL, "avformat_open_input() failed %d\n", err);
        ret = -1;
        goto fail;
    }
    is->p_fmt_ctx = p_fmt_ctx;

    // 1.2 Search for stream information: Read a portion of the video file data, attempt decoding, and fill in p_fmt_ctx->streams with the obtained stream information.
    err = avformat_find_stream_info(p_fmt_ctx, NULL);
    if (err < 0)
    {
        av_log(NULL, AV_LOG_FATAL, "avformat_find_stream_info() failed %d\n", err);
        ret = -1;
        goto fail;
    }

    // 2. Find the first audio stream/video stream
    a_idx = -1;
    v_idx = -1;
    for (i=0; i<(int)p_fmt_ctx->nb_streams; i++)
    {
        if ((p_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) &&
            (a_idx == -1))
        {
            a_idx = i;
            av_log(NULL, AV_LOG_DEBUG, "Find an audio stream, index %d\n", a_idx);
        }
        if ((p_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) &&
            (v_idx == -1))
        {
            v_idx = i;
            av_log(NULL, AV_LOG_DEBUG, "Find a video stream, index %d\n", v_idx);
        }
        if (a_idx != -1 && v_idx != -1)
        {
            break;
        }
    }
    if (a_idx == -1 && v_idx == -1)
    {
        av_log(NULL, AV_LOG_INFO, "Couldn't find any audio/video streams\n");
        ret = -1;
 fail:
        if (p_fmt_ctx != NULL)
        {
            avformat_close_input(&p_fmt_ctx);
        }
        return ret;
    }

    is->audio_idx = a_idx;
    is->video_idx = v_idx;
    is->p_audio_stream = p_fmt_ctx->streams[a_idx];
    is->p_video_stream = p_fmt_ctx->streams[v_idx];

    return 0;
}

int demux_deinit()
{
    return 0;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, packet_queue_t *queue)
{
    return stream_id < 0 ||
           queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

/* this thread gets the stream from the disk or the network */
static int demux_thread(void *arg)
{
    player_stat_t *is = (player_stat_t *)arg;
    AVFormatContext *p_fmt_ctx = is->p_fmt_ctx;
    int ret;
    AVPacket pkt1, *pkt = &pkt1;

    SDL_mutex *wait_mutex = SDL_CreateMutex();
    bool first_pack = true;

    av_log(NULL, AV_LOG_DEBUG, "demux_thread running...\n");

    // 4. Demultiplexing process
    while (1)
    {
        if (is->abort_request)
        {
            av_log(NULL, AV_LOG_DEBUG, "demux_thread thread received quit\n");
            break;
        }

        // Handle seeking request
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;

            // FIXME the +-2 is due to rounding being not done in the correct direction in generation
            // of the seek_pos/seek_rel variables

            ret = avformat_seek_file(p_fmt_ctx, -1, seek_min, seek_target, seek_max, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                        "%s: error while seeking\n", is->filename);
            } else {
                if (is->audio_idx >= 0) {
                    packet_queue_flush(&is->audio_pkt_queue);
                    packet_queue_put_nullpacket(&is->audio_pkt_queue, is->audio_idx);
                }
                if (is->video_idx >= 0) {
                    packet_queue_flush(&is->video_pkt_queue);
                    packet_queue_put_nullpacket(&is->video_pkt_queue, is->video_idx);
                }

                // set_clock(&is->audio_clk, seek_target / (double)AV_TIME_BASE, 0);
                // set_clock(&is->video_clk, seek_target / (double)AV_TIME_BASE, 0);
            }

            is->seek_req = 0;
            // if (is->paused)
            //     step_to_next_frame(is);
        }

        /* if the queues are full, no need to read more */
        if (is->audio_pkt_queue.size + is->video_pkt_queue.size > MAX_QUEUE_SIZE ||
            (stream_has_enough_packets(is->p_audio_stream, is->audio_idx, &is->audio_pkt_queue) &&
             stream_has_enough_packets(is->p_video_stream, is->video_idx, &is->video_pkt_queue)))
        {
            /* wait for 10 ms */
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }

        // 4.1 Read a packet from the input file
        ret = av_read_frame(is->p_fmt_ctx, pkt);
        if (ret < 0)
        {
            if ((ret == AVERROR_EOF))// || avio_feof(ic->pb)) && !is->eof)
            {
                // If the input file is finished, send a NULL packet to the packet queue to flush the decoder; otherwise, cached frames in the decoder can't be retrieved.
                if (is->video_idx >= 0)
                {
                    packet_queue_put_nullpacket(&is->video_pkt_queue, is->video_idx);
                }
                if (is->audio_idx >= 0)
                {
                    packet_queue_put_nullpacket(&is->audio_pkt_queue, is->audio_idx);
                }
            }

            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }

        if (first_pack) {
            is->start_time = is->p_fmt_ctx->streams[pkt->stream_index]->start_time;
            first_pack = false;
        }

        // 4.3 Put the current packet into the corresponding packet queue based on its type (audio, video, subtitle)
        if (pkt->stream_index == is->audio_idx)
        {
            packet_queue_put(&is->audio_pkt_queue, pkt);
        }
        else if (pkt->stream_index == is->video_idx)
        {
            packet_queue_put(&is->video_pkt_queue, pkt);
        }
        else
        {
            av_packet_unref(pkt);
        }
    }

    ret = 0;

    if (ret != 0)
    {
        SDL_Event event;

        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }

    SDL_DestroyMutex(wait_mutex);
    return 0;
}

int open_demux(player_stat_t *is)
{
    // Open, read the file, and obtain audio and video stream information
    if (demux_init(is) != 0)
    {
        av_log(NULL, AV_LOG_FATAL, "demux_init() failed\n");
        return -1;
    }

    // Start the demuxing thread
    is->read_tid = SDL_CreateThread(demux_thread, "demux_thread", is);
    if (is->read_tid == NULL)
    {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread() failed: %s\n", SDL_GetError());
        return -1;
    }

    return 0;
}
