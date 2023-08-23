#include "frame.h"
#include "player.h"

// Unreference the frame item by releasing its associated resources.
void frame_queue_unref_item(frame_t *vp)
{
    av_frame_unref(vp->frame);
}

// Initialize a frame queue.
int frame_queue_init(frame_queue_t *f, packet_queue_t *pktq, int max_size, int keep_last)
{
    int i;
    memset(f, 0, sizeof(frame_queue_t));
    if (!(f->mutex = SDL_CreateMutex())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    if (!(f->cond = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

// Destroy a frame queue by freeing its resources.
void frame_queue_destory(frame_queue_t *f)
{
    int i;
    for (i = 0; i < f->max_size; i++) {
        frame_t *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
    }
    SDL_DestroyMutex(f->mutex);
    SDL_DestroyCond(f->cond);
}

// Signal that there's a change in the frame queue.
void frame_queue_signal(frame_queue_t *f)
{
    SDL_LockMutex(f->mutex);
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

// Peek at the next frame in the frame queue without removing it.
frame_t *frame_queue_peek(frame_queue_t *f)
{
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

// Peek at the frame following the next frame in the frame queue.
frame_t *frame_queue_peek_next(frame_queue_t *f)
{
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

// Peek at the last frame in the frame queue (used for playback).
frame_t *frame_queue_peek_last(frame_queue_t *f)
{
    return &f->queue[f->rindex];
}

// Peek at the next writable frame in the frame queue, waiting if necessary.
frame_t *frame_queue_peek_writable(frame_queue_t *f)
{
    /* wait until we have space to put a new frame */
    SDL_LockMutex(f->mutex);
    int signaled = 1;
    while (signaled && f->size >= f->max_size &&
           !f->pktq->abort_request) {
        signaled = SDL_CondWaitTimeout(f->cond, f->mutex, 20);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[f->windex];
}

// Peek at the next readable frame in the frame queue, waiting if necessary.
frame_t *frame_queue_peek_readable(frame_queue_t *f)
{
    int signaled = 1;
    /* wait until we have a readable new frame */
    SDL_LockMutex(f->mutex);
    while (signaled && f->size - f->rindex_shown <= 0 &&
           !f->pktq->abort_request) {
        signaled = SDL_CondWaitTimeout(f->cond, f->mutex, 20);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

// Push a frame into the frame queue, updating counters and signals.
void frame_queue_push(frame_queue_t *f)
{
    if (++f->windex == f->max_size)
        f->windex = 0;
    SDL_LockMutex(f->mutex);
    f->size++;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

// Move to the next frame in the frame queue after it has been shown.
void frame_queue_next(frame_queue_t *f)
{
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    SDL_LockMutex(f->mutex);
    f->size--;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

// Return the number of undisplayed frames remaining in the queue.
int frame_queue_nb_remaining(frame_queue_t *f)
{
    return f->size - f->rindex_shown;
}

// Return the last shown position.
int64_t frame_queue_last_pos(frame_queue_t *f)
{
    frame_t *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial)
        return fp->pos;
    else
        return -1;
}
