/*
 * SDL_Lesson.c
 *
 *  Created on: Aug 12, 2014
 *      Author: clarck
 */
#include <jni.h>
#include <android/native_window_jni.h>
#include "SDL.h"
#include "SDL_thread.h"
#include "SDL_events.h"
#include "../include/logger.h"
#include "../ffmpeg/include/libavcodec/avcodec.h"
#include "../ffmpeg/include/libavformat/avformat.h"
#include "../ffmpeg/include/libavutil/pixfmt.h"
#include "../ffmpeg/include/libswscale/swscale.h"
#include "../ffmpeg/include/libswresample/swresample.h"

#define SDL_AUDIO_BUFFER_SIZE 1024

#define MAX_AUDIO_SIZE (5 * 16 * 1024)
#define MAX_VIDEO_SIZE (5 * 256 * 1024)

#define FF_ALLOC_EVENT (SDL_USEREVENT)
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)
#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

#define VIDEO_PICTURE_QUEUE_SIZE 1
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio

typedef struct PacketQueue {
	AVPacketList *first_pkt, *last_pkt;
	int nb_packets;
	int size;
	SDL_mutex *mutex;
	SDL_cond *cond;
} PacketQueue;

typedef struct VideoPicture {
	SDL_Renderer *renderer;
	SDL_Texture *bmp;

	AVFrame* rawdata;
    int width, height; /*source height & width*/
    int allocated;
} VideoPicture;

typedef struct VideoState {
    AVFormatContext *pFormatCtx;
    int videoStream, audioStream;
    AVStream *audio_st;
    PacketQueue audioq;

    uint8_t         *audio_buf;
    DECLARE_ALIGNED(16,uint8_t,audio_buf2)[AVCODEC_MAX_AUDIO_FRAME_SIZE * 4];

    unsigned int audio_buf_size;
    unsigned int audio_buf_index;
    AVPacket audio_pkt;
    uint8_t *audio_pkt_data;
    int audio_pkt_size;
    AVFrame *audio_frame;
    AVStream *video_st;
    PacketQueue videoq;

    VideoPicture pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int pictq_size, pictq_rindex, pictq_windex;
    SDL_mutex *pictq_mutex;
    SDL_cond *pictq_cond;

    SDL_Thread *parse_tid;
    SDL_Thread *video_tid;

    AVIOContext *io_ctx;
    struct SwsContext *sws_ctx;

	enum AVSampleFormat audio_src_fmt;
	enum AVSampleFormat audio_tgt_fmt;
	int audio_src_channels;
	int audio_tgt_channels;
	int64_t audio_src_channel_layout;
	int64_t audio_tgt_channel_layout;
	int audio_src_freq;
	int audio_tgt_freq;
	struct SwrContext *swr_ctx;

    char filename[1024];
    int quit;
} VideoState;

SDL_Window *screen;

/*Since we only hava one decoding thread, the Big Struct can be
 * global in case we need it. */
VideoState *global_video_state;

void packet_queue_init(PacketQueue *q) {
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
	AVPacketList *pkt1;
	if (av_dup_packet(pkt) < 0) {
		return -1;
	}

	pkt1 = av_malloc(sizeof(AVPacketList));
	if (!pkt1)
		return -1;

	pkt1->pkt = *pkt;
	pkt1->next = NULL;

	SDL_LockMutex(q->mutex);

	if (!q->last_pkt)
		q->first_pkt = pkt1;
	else
		q->last_pkt->next = pkt1;

	q->last_pkt = pkt1;
	q->nb_packets++;
	q->size += pkt1->pkt.size;
	SDL_CondSignal(q->cond);

	SDL_UnlockMutex(q->mutex);
	return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
	AVPacketList *pkt1;
	int ret;

	SDL_LockMutex(q->mutex);

	for (;;) {
		if (global_video_state->quit) {
			ret = -1;
			break;
		}

		pkt1 = q->first_pkt;
		if (pkt1) {
			q->first_pkt = pkt1->next;
			if (!q->first_pkt)
				q->last_pkt = NULL;
			q->nb_packets--;
			q->size -= pkt1->pkt.size;
			*pkt = pkt1->pkt;
			av_free(pkt1);
			ret = 1;
			break;
		} else if (!block) {
			ret = 0;
		} else {
			SDL_CondWait(q->cond, q->mutex);
		}
	}

	SDL_UnlockMutex(q->mutex);
	return ret;
}

int audio_decode_frame(VideoState *is) {
	int len1, len2, decoded_data_size = 0;
	AVPacket *pkt = &is->audio_pkt;

	int got_frame = 0;
	int64_t dec_channel_layout;
	int wanted_nb_samples, resampled_data_size;

	for (;;) {
		while (is->audio_pkt_size > 0) {
			if (!is->audio_frame) {
				if (!(is->audio_frame = avcodec_alloc_frame())) {
					return AVERROR(ENOMEM);
				}
			} else
				avcodec_get_frame_defaults(is->audio_frame);

			len1 = avcodec_decode_audio4(is->audio_st->codec, is->audio_frame,
					&got_frame, pkt);

			if (len1 < 0) {
				/*if error, skip frame*/
				is->audio_pkt_size = 0;
				break;
			}

			if (!got_frame)
				continue;

			/* 计算解码出来的桢需要的缓冲大小 */
			decoded_data_size = av_samples_get_buffer_size(NULL,
					is->audio_frame->channels, is->audio_frame->nb_samples,
					is->audio_frame->format, 1);

			dec_channel_layout =
					(is->audio_frame->channel_layout
							&& is->audio_frame->channels
									== av_get_channel_layout_nb_channels(
											is->audio_frame->channel_layout)) ?
													is->audio_frame->channel_layout :
							av_get_default_channel_layout(
									is->audio_frame->channels);

			wanted_nb_samples = is->audio_frame->nb_samples;

			if (is->audio_frame->format != is->audio_src_fmt
					|| dec_channel_layout != is->audio_src_channel_layout
					|| is->audio_frame->sample_rate != is->audio_src_freq
					|| (wanted_nb_samples != is->audio_frame->nb_samples
							&& !is->swr_ctx)) {
				if (is->swr_ctx)
					swr_free(&is->swr_ctx);
				is->swr_ctx = swr_alloc_set_opts(NULL,
						is->audio_tgt_channel_layout, is->audio_tgt_fmt,
						is->audio_tgt_freq, dec_channel_layout,
						is->audio_frame->format, is->audio_frame->sample_rate,
						0, NULL);
				if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
					fprintf(stderr, "swr_init() failed\n");
					break;
				}
				is->audio_src_channel_layout = dec_channel_layout;
				is->audio_src_channels = is->audio_st->codec->channels;
				is->audio_src_freq = is->audio_st->codec->sample_rate;
				is->audio_src_fmt = is->audio_st->codec->sample_fmt;
			}

			/* 这里我们可以对采样数进行调整，增加或者减少，一般可以用来做声画同步 */
			if (is->swr_ctx) {
				// const uint8_t *in[] = { is->audio_frame->data[0] };
				const uint8_t **in =
						(const uint8_t **) is->audio_frame->extended_data;
				uint8_t *out[] = { is->audio_buf2 };
				if (wanted_nb_samples != is->audio_frame->nb_samples) {
					if (swr_set_compensation(is->swr_ctx,
							(wanted_nb_samples - is->audio_frame->nb_samples)
									* is->audio_tgt_freq
									/ is->audio_frame->sample_rate,
							wanted_nb_samples * is->audio_tgt_freq
									/ is->audio_frame->sample_rate) < 0) {
						fprintf(stderr, "swr_set_compensation() failed\n");
						break;
					}
				}

				len2 = swr_convert(is->swr_ctx, out,
						sizeof(is->audio_buf2) / is->audio_tgt_channels
								/ av_get_bytes_per_sample(is->audio_tgt_fmt),
						in, is->audio_frame->nb_samples);
				if (len2 < 0) {
					fprintf(stderr, "swr_convert() failed\n");
					break;
				}
				if (len2
						== sizeof(is->audio_buf2) / is->audio_tgt_channels
								/ av_get_bytes_per_sample(is->audio_tgt_fmt)) {
					fprintf(stderr,
							"warning: audio buffer is probably too small\n");
					swr_init(is->swr_ctx);
				}
				is->audio_buf = is->audio_buf2;
				resampled_data_size = len2 * is->audio_tgt_channels
						* av_get_bytes_per_sample(is->audio_tgt_fmt);
			} else {
				resampled_data_size = decoded_data_size;
				is->audio_buf = is->audio_frame->data[0];
			}

			is->audio_pkt_data += len1;
			is->audio_pkt_size -= len1;

			if (decoded_data_size <= 0) {
				/*No data yet, get more frames*/
				continue;
			}
			/* We have data, return it and come back for more later */
			return decoded_data_size;
		}
		if (pkt->data)
			av_free_packet(pkt);

		memset(pkt, 0, sizeof(*pkt));

		if (is->quit)
			return -1;

		if (packet_queue_get(&is->audioq, pkt, 1) < 0)
			return -1;

		is->audio_pkt_data = pkt->data;
		is->audio_pkt_size = pkt->size;

	}
	return 0;
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
	VideoState *is = (VideoState *) userdata;
	int len1, audio_size;

	while (len > 0) {
		if (is->audio_buf_index >= is->audio_buf_size) {
			/* We have already sent all out data; get more*/
			audio_size = audio_decode_frame(is);
			if (audio_size < 0) {
				/* If error, output silence */
				is->audio_buf_size = 1024;
				memset(is->audio_buf, 0, is->audio_buf_size);
			} else {
				is->audio_buf_size = audio_size;
			}
			is->audio_buf_index = 0;
		}
		len1 = is->audio_buf_size - is->audio_buf_index;
		if (len1 > len)
			len1 = len;
		memcpy(stream, (uint8_t *) is->audio_buf + is->audio_buf_index, len1);
		len -= len1;
		stream += len1;
		is->audio_buf_index += len1;
	}
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0;/* 0 means stop timer */
}

/* schedule a video refresh in 'delay' ms*/
static void schedule_refresh(VideoState *is, int delay) {
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_display(VideoState *is) {
    SDL_Rect rect;
    VideoPicture *vp;
    float aspect_ratio;

    vp = &is->pictq[is->pictq_rindex];
    if (vp->bmp) {
        if (is->video_st->codec->sample_aspect_ratio.num == 0) {
            aspect_ratio = 0;
        } else {
            aspect_ratio = av_q2d(is->video_st->codec->sample_aspect_ratio)
                    * is->video_st->codec->width / is->video_st->codec->height;
        }

        if (aspect_ratio <= 0.0) {
            aspect_ratio = (float) is->video_st->codec->width
                    / (float) is->video_st->codec->height;
        }

		rect.x = 0;
		rect.y = 0;
		rect.w = vp->width;
		rect.h = vp->height;

        SDL_UpdateYUVTexture(vp->bmp, &rect, vp->rawdata->data[0],
        		vp->rawdata->linesize[0], vp->rawdata->data[1],
        		vp->rawdata->linesize[1], vp->rawdata->data[2],
        		vp->rawdata->linesize[2]);

        SDL_RenderClear(vp->renderer);
		SDL_RenderCopy(vp->renderer, vp->bmp, &rect, &rect);
		SDL_RenderPresent(vp->renderer);
    }
}

void video_refresh_timer(void *userdata) {
    VideoState *is = (VideoState *) userdata;
    //VideoPicture *vp;
    if (is->video_st) {
        if (is->pictq_size == 0) {
            schedule_refresh(is, 1);
        } else {
            //vp = &is->pictq[is->pictq_rindex];
            /* Now, normally here goes a ton of code
             about timing, etc. we're just going to
             guess at a delay for now. You can
             increase and decrease this value and hard code
             the timing - but I don't suggest that ;)
             We'll learn how to do it for real later.
             */
            schedule_refresh(is, 80);

            /* show the picture! */
            video_display(is);

            /* update queue for next picture! */
            if (++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
                is->pictq_rindex = 0;
            }
            SDL_LockMutex(is->pictq_mutex);
            is->pictq_size--;
            SDL_CondSignal(is->pictq_cond);
            SDL_UnlockMutex(is->pictq_mutex);
        }
    } else {
        schedule_refresh(is, 100);
    }
}

void alloc_picture(void *userdata) {
    VideoState *is = (VideoState *) userdata;
    VideoPicture *vp;

    vp = &is->pictq[is->pictq_windex];
    if (vp->bmp) {
        // we already have one make another, bigger/smaller
    	SDL_DestroyTexture(vp->bmp);
    }

    if(vp->rawdata) {
    	av_free(vp->rawdata);
    }

    // Allocate a place to put our YUV image on that screen
	screen = SDL_CreateWindow("My Player Window", SDL_WINDOWPOS_UNDEFINED,
			SDL_WINDOWPOS_UNDEFINED, is->video_st->codec->width,
			is->video_st->codec->height,
			SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL);

    vp->renderer = SDL_CreateRenderer(screen, -1, 0);
    vp->bmp = SDL_CreateTexture(vp->renderer, SDL_PIXELFORMAT_YV12,
    			SDL_TEXTUREACCESS_STREAMING, is->video_st->codec->width, is->video_st->codec->height);

    vp->width = is->video_st->codec->width;
    vp->height = is->video_st->codec->height;


    AVFrame* pFrameYUV = avcodec_alloc_frame();
	if (pFrameYUV == NULL)
		return;

	int numBytes = avpicture_get_size(PIX_FMT_YUV420P, vp->width,
			vp->height);

	uint8_t* buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));

	avpicture_fill((AVPicture *) pFrameYUV, buffer, PIX_FMT_YUV420P,
			vp->width, vp->height);

	vp->rawdata = pFrameYUV;

    SDL_LockMutex(is->pictq_mutex);
    vp->allocated = 1;
    SDL_CondSignal(is->pictq_cond);
    SDL_UnlockMutex(is->pictq_mutex);
}

int queue_picture(VideoState *is, AVFrame *pFrame) {
    VideoPicture *vp;
    //int dst_pic_fmt
    AVPicture pict;

    /* wait unitl we have space for a new pic */
    SDL_LockMutex(is->pictq_mutex);
    while (is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit) {
        SDL_CondWait(is->pictq_cond, is->pictq_mutex);
    }
    SDL_UnlockMutex(is->pictq_mutex);

    if (is->quit)
        return -1;

    // windex is set to 0 initially
    vp = &is->pictq[is->pictq_windex];

    /* allocate or resize the buffer ! */
    if (!vp->bmp || vp->width != is->video_st->codec->width
            || vp->height != is->video_st->codec->height) {
        SDL_Event event;

        vp->allocated = 0;
        /* we have to do it in the main thread */
        event.type = FF_ALLOC_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);

        /* wait until we have a picture allocated */
        SDL_LockMutex(is->pictq_mutex);
        while (!vp->allocated && !is->quit) {
            SDL_CondWait(is->pictq_cond, is->pictq_mutex);
        }
    }
    SDL_UnlockMutex(is->pictq_mutex);
    if (is->quit) {
        return -1;
    }

    /* We have a place to put our picture on the queue */
    if (vp->rawdata) {
        // Convert the image into YUV format that SDL uses
        sws_scale(is->sws_ctx, (uint8_t const * const *)pFrame->data,
                pFrame->linesize, 0, is->video_st->codec->height,
                vp->rawdata->data, vp->rawdata->linesize);

        /* now we inform our display thread that we have a pic ready */
        if (++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
            is->pictq_windex = 0;
        }
        SDL_LockMutex(is->pictq_mutex);
        is->pictq_size++;
        SDL_UnlockMutex(is->pictq_mutex);
    }
    return 0;
}

int video_thread(void *arg) {
	VideoState *is = (VideoState *) arg;
	AVPacket pkt1, *packet = &pkt1;
	int frameFinished;
	AVFrame *pFrame;

	pFrame = av_frame_alloc();

	for (;;) {
		if (packet_queue_get(&is->videoq, packet, 1) < 0) {
			// means we quit getting packets
			break;
		}

		// Decode video frame
		avcodec_decode_video2(is->video_st->codec, pFrame, &frameFinished,
				packet);

		// Did we get a video frame?
		if (frameFinished) {
			if (queue_picture(is, pFrame) < 0) {
				break;
			}
		}
		av_free_packet(packet);
	}

	av_free(pFrame);
	return 0;
}

int stream_component_open(VideoState *is, int stream_index) {
    AVFormatContext *pFormatCtx = is->pFormatCtx;
    AVCodecContext *codecCtx;
    AVCodec *codec;
    SDL_AudioSpec wanted_spec, spec;

    int64_t wanted_channel_layout = 0;
	int wanted_nb_channels;
	const int next_nb_channels[] = { 0, 0, 1, 6, 2, 6, 4, 6 };

    if (stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
        return -1;
    }

    // Get a pointer to the codec context for the video stream
    codecCtx = pFormatCtx->streams[stream_index]->codec;

    if (codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
    	wanted_nb_channels = codecCtx->channels;
		if (!wanted_channel_layout
				|| wanted_nb_channels
						!= av_get_channel_layout_nb_channels(
								wanted_channel_layout)) {
			wanted_channel_layout = av_get_default_channel_layout(
					wanted_nb_channels);
			wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
		}

		wanted_spec.channels = av_get_channel_layout_nb_channels(
				wanted_channel_layout);
		wanted_spec.freq = codecCtx->sample_rate;
		if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
			fprintf(stderr, "Invalid sample rate or channel count!\n");
			return -1;
		}
		wanted_spec.format = AUDIO_S16SYS;
		wanted_spec.silence = 0;
		wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
		wanted_spec.callback = audio_callback;
		wanted_spec.userdata = is;

		while (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
			fprintf(stderr, "SDL_OpenAudio (%d channels): %s\n",
					wanted_spec.channels, SDL_GetError());
			wanted_spec.channels = next_nb_channels[FFMIN(7,
					wanted_spec.channels)];
			if (!wanted_spec.channels) {
				fprintf(stderr,
						"No more channel combinations to tyu, audio open failed\n");
				return -1;
			}
			wanted_channel_layout = av_get_default_channel_layout(
					wanted_spec.channels);
		}

		if (spec.format != AUDIO_S16SYS) {
			fprintf(stderr, "SDL advised audio format %d is not supported!\n",
					spec.format);
			return -1;
		}
		if (spec.channels != wanted_spec.channels) {
			wanted_channel_layout = av_get_default_channel_layout(
					spec.channels);
			if (!wanted_channel_layout) {
				fprintf(stderr,
						"SDL advised channel count %d is not supported!\n",
						spec.channels);
				return -1;
			}
		}

		is->audio_src_fmt = is->audio_tgt_fmt = AV_SAMPLE_FMT_S16;
		is->audio_src_freq = is->audio_tgt_freq = spec.freq;
		is->audio_src_channel_layout = is->audio_tgt_channel_layout =
				wanted_channel_layout;
		is->audio_src_channels = is->audio_tgt_channels = spec.channels;
    }

    codec = avcodec_find_decoder(codecCtx->codec_id);
    if (!codec || (avcodec_open2(codecCtx, codec, NULL) < 0)) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }

    pFormatCtx->streams[stream_index]->discard = AVDISCARD_DEFAULT;

    switch (codecCtx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audioStream = stream_index;
        is->audio_st = pFormatCtx->streams[stream_index];
        is->audio_buf_size = 0;
        is->audio_buf_index = 0;
        memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
        packet_queue_init(&is->audioq);
        SDL_PauseAudio(0);
        break;

    case AVMEDIA_TYPE_VIDEO:
        is->videoStream = stream_index;
        is->video_st = pFormatCtx->streams[stream_index];
        is->sws_ctx = sws_getContext(is->video_st->codec->width,
                is->video_st->codec->height, is->video_st->codec->pix_fmt,
                is->video_st->codec->width, is->video_st->codec->height,
                AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, NULL, NULL, NULL);
        packet_queue_init(&is->videoq);
        is->video_tid = SDL_CreateThread(video_thread, "video_decode_thread", is);
        break;
    default:
        break;
    }
    return 0;
}

int decode_interrupt_cb(void *opaque) {
    return (global_video_state && global_video_state->quit);
}

int decode_thread(void *arg) {
    VideoState *is = (VideoState *) arg;
    AVFormatContext *pFormatCtx = NULL;
    AVPacket pkt1, *packet = &pkt1;

    int video_index = -1;
    int audio_index = -1;
    int i;

    is->videoStream = -1;
    is->audioStream = -1;

    AVIOInterruptCB interupt_cb;

    global_video_state = is;

    // will interrup blocking functions if we quit!
    interupt_cb.callback = decode_interrupt_cb;
    interupt_cb.opaque = is;

    if (avio_open2(&is->io_ctx, is->filename, 0, &interupt_cb, NULL)) {
        fprintf(stderr, "Cannot open I/O for %s\n", is->filename);
        return -1;
    }

    //Open video file
    if (avformat_open_input(&pFormatCtx, is->filename, NULL, NULL) != 0) {
        return -1; //Couldn't open file
    }

    is->pFormatCtx = pFormatCtx;

    //Retrieve stream infomation
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
        return -1; // Couldn't find stream information
    }

    //Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, is->filename, 0);

    //Find the first video stream
    for (i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codec->coder_type == AVMEDIA_TYPE_VIDEO
                && video_index < 0) {
            video_index = i;
        }

        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO
                && audio_index < 0) {
            audio_index = i;
        }
    }

    if (audio_index >= 0) {
        stream_component_open(is, audio_index);
    }

    if (video_index >= 0) {
        stream_component_open(is, video_index);
    }

    if (is->videoStream < 0 || is->audioStream <= 0) {
        fprintf(stderr, "%s: could not open codec\n", is->filename);
        goto fail;
    }

    //main decode loop
    for (;;) {
        if (is->quit) {
            break;
        }

        //seek  stuff goes here
        if (is->audioq.size > MAX_AUDIO_SIZE || is->videoq.size > MAX_VIDEO_SIZE) {
            SDL_Delay(10);
            continue;
        }
        if (av_read_frame(is->pFormatCtx, packet) < 0) {
            if (is->pFormatCtx->pb->error == 0) {
                SDL_Delay(100); /* no error; wait for user input */
                continue;
            } else {
                break;
            }
        }
        // Is this a packet from the video stream?
        if (packet->stream_index == is->videoStream) {
            packet_queue_put(&is->videoq, packet);
        } else if (packet->stream_index == is->audioStream) {
            packet_queue_put(&is->audioq, packet);
        } else {
            av_free_packet(packet);
        }
    }

    /*all done - wait for it*/
    while (!is->quit) {
        SDL_Delay(100);
    }

    fail: if (1) {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    return 0;
}

int main(int argc, char *argv[]) {
	char *file_path = argv[1];
	LOGI("file_path:%s", file_path);

	SDL_Event event;

	VideoState *is;
	is = av_malloc(sizeof(VideoState));

	// Register all formats and codecs
	av_register_all();

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
		exit(1);
	}

	av_strlcpy(is->filename, file_path, sizeof(is->filename));

	is->pictq_mutex = SDL_CreateMutex();
	is->pictq_cond = SDL_CreateCond();

	schedule_refresh(is, 40);

	is->parse_tid = SDL_CreateThread(decode_thread, "parse_thread", is);
	if (!is->parse_tid) {
		av_free(is);
		return -1;
	}

	for (;;) {
		SDL_WaitEvent(&event);
		switch (event.type) {
		case FF_QUIT_EVENT:
		case SDL_QUIT:
			SDL_CondSignal(is->audioq.cond);
			SDL_CondSignal(is->videoq.cond);
			is->quit = 1;
			SDL_Quit();
			return 0;
			break;
		case FF_ALLOC_EVENT:
			alloc_picture(event.user.data1);
			break;

		case FF_REFRESH_EVENT:
			video_refresh_timer(event.user.data1);
			break;
		}
	}

	return 0;
}
