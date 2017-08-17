//
//  03SDLPlayer.c
//  player2
//
//  Created by zhangyun on 11/08/2017.
//  Copyright © 2017 zhangyun. All rights reserved.
//

#include "03SDLPlayer.h"

#ifndef ffmpeg_h
#define ffmpeg_h
#include "avcodec.h"
#include "avformat.h"
#include "avfilter.h"
#include "swscale.h"
#endif


#ifndef sdl_h
#define sdl_h
#include "SDL.h"
#include "SDL_thread.h"
#endif


#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

typedef struct PacketQueue{

    AVPacketList *first_pkt,*last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
}PacketQueue;

PacketQueue audioq;

int quit = 0;

void packet_queue_init(PacketQueue *q){
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

// 添加 packet
int packet_queue_put(PacketQueue *q,AVPacket *pkt){

//    typedef struct AVPacketList {
//        AVPacket pkt;
//        struct AVPacketList *next;
//    } AVPacketList;
    AVPacketList *pkt1 = NULL;
    
    if(av_dup_packet(pkt) < 0) {
        return -1;
    }
    
//    if (av_packet_ref(pkt, pkt) < 0) {
//        return -1;
//    }
    pkt1 = av_malloc(sizeof(AVPacketList));
    
    if (!pkt1) {
        return -1;
    }
    
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    
    //Lock the mutex.
    SDL_LockMutex(q->mutex);
    
    
    if (!q->last_pkt) { // 说明是空链表
        q->first_pkt = pkt1;
    }else{
        q->last_pkt->next = pkt1;
    }
    
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    //Restart one of the threads that are waiting on the condition variable.
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}

// 取packet
static int packet_queue_get(PacketQueue *q,AVPacket *pkt,
                            int block){
    AVPacketList *pkt1;
    int ret;
    SDL_LockMutex(q->mutex);
    
    for (;;) { // 死循环
        if (quit) {
            ret = -1;
            break;
        }
        
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt) { // 只有一个node
                q->last_pkt = NULL;
            }
            
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        }else if(!block){ // 是否阻塞等待 q 中来了数据
            ret = 0;
            break;
        }else{
            
            
            /**
             *  Wait on the condition variable, 
             unlocking the provided mutex.
             The mutex must be locked before
             entering this function!
             *  The mutex is re-locked once the condition 
             variable is signaled.
             *  0 when it is signaled,
             or -1 on error.
             */
            SDL_CondWait(q->cond, q->mutex);
        }
    } // for
    
    SDL_UnlockMutex(q->mutex);
    return ret;
}


// 解码音频packet
static int audio_decode_frame(AVCodecContext *aCodecCtx,
                       uint8_t *audio_buf,int buf_size){

    
    static AVPacket pkt;
    static uint8_t *audio_pkt_data = NULL;
    static int audio_pkt_size = 0;
    static AVFrame frame;
    
    int len1,data_size = 0;
    
    for (;;) {
        while (audio_pkt_size > 0) {
            int got_frame = 0;
            len1 = avcodec_decode_audio4(aCodecCtx,&frame, &got_frame,&pkt);
            if (len1 < 0) {
                audio_pkt_size = 0;
                break;
            }
            audio_pkt_data += len1;
            audio_pkt_size -= len1;
            if (!got_frame) {
                data_size = av_samples_get_buffer_size(NULL,
                                                       aCodecCtx->channels, frame.nb_samples, aCodecCtx->sample_fmt, 1);
                
                memcpy(audio_buf, frame.data[0], data_size);
            }
            
            if (data_size <= 0) {
                continue;
            }
            
            return data_size;
        } // while
        
        if (pkt.data) {
            av_packet_unref(&pkt);
        }
        
        if (quit) {
            return -1;
        }
        
        if (packet_queue_get(&audioq, &pkt, 1) < 0) {
            return -1;
        }
        
        
        audio_pkt_data = pkt.data;
        audio_pkt_size = pkt.size;
        
    } // for
}

void audio_callback(void *userdata,Uint8 *stream,int len){
    AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;
    int len1,audio_size;
    
    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE *3) / 2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;
    // 去取音频数据 如果数据够就填充返回，剩下的下次使用，所以使用static ，
    // 不够就循环的去解码 取数据
    while (len > 0) {
        if (audio_buf_index >= audio_buf_size) {
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, audio_buf_size);
            if (audio_size < 0) {
                audio_buf_size = 1024;
                memset(audio_buf, 0, audio_buf_size);
            }else{
                audio_buf_size = audio_size;
            }
            audio_buf_index = 0;
        } // i
        
        len1 = audio_buf_size - audio_buf_index;
        if (len1 > len) {
            len1 = len;
        }
        
        memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
        len -= len1;
        stream += len1;
        audio_buf_index += len1;
    } // while
    
}

int play03(){
    AVFormatContext *pFormatCtx = NULL;
    int videoStream,audioStream;
    AVCodecContext *pCodecCtx = NULL;
    AVCodec *pCodec = NULL;
    AVFrame *pFrame = NULL;
    AVPacket packet;
//    int frameFinished;
    
    AVCodecContext *aCodecCtx = NULL;
    AVCodec *aCodec = NULL;
    
    
    SDL_Window *window;
    SDL_Renderer *render;
    SDL_Texture *texture;
//    SDL_Rect rect;
    SDL_Event event;
    SDL_AudioSpec wanted_spec,spec;

    Uint8 *yPlane,*uPlane,*vPlane;
    size_t yPlaneSz,uvPlaneSz;
    int uvPitch;
    struct SwsContext *sws_ctx = NULL;
    
    AVDictionary *videoOptionDict = NULL;
    AVDictionary *audioOPtionsDict = NULL;
    int ret = -1;
    
    
    char *file_path = "/Users/zhangyun/Downloads/BBC.Wild.China.6.Tides.of.Change.美丽中国之六潮汐更迭.双语字幕.HR-HDTV.AC3.960×528.X264-人人影视制作.avi";
    
    av_register_all();
    
    
    // SDL
    if(SDL_Init(SDL_INIT_EVERYTHING)){
        fprintf(stderr, "sdl_init failed");
        exit(1);
    }
    
    if(avformat_open_input(&pFormatCtx, file_path, NULL, NULL) != 0){
        fprintf(stderr, "open file failed");
        return -1;
    }
    
    if(avformat_find_stream_info(pFormatCtx, NULL) < 0 ){
        fprintf(stderr, "find stream info failed");
        return -1;
    }
    
    av_dump_format(pFormatCtx, 0, file_path, 0);
    
    videoStream = -1;
    audioStream = -1;
    
    for (int i = 0; i < pFormatCtx->nb_streams; i++) {
        if (videoStream < 0 &&  pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = i;
        }
        
        if (audioStream < 0 && pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStream = i;
        }
    } // for
    
    if (videoStream == -1) {
        return -1;
    }
    
    if (audioStream == -1) {
        return -1;
    }
    
    
    // acodec
    aCodec = avcodec_find_decoder(pFormatCtx->streams[audioStream]->codecpar->codec_id);
    if (!aCodec) {
        fprintf(stderr, "find audio codec failed");
        return -1;
    }

    // aCodecCtx
    aCodecCtx = avcodec_alloc_context3(aCodec);
    if(avcodec_parameters_to_context(aCodecCtx, pFormatCtx->streams[audioStream]->codecpar)< 0){
        fprintf(stderr, "audio param to context failed");
        return -1;
    }
    if(avcodec_open2(aCodecCtx, aCodec, &audioOPtionsDict) < 0){
        fprintf(stderr, "open2 audioo codecctx failed");
        return -1;
    }
    
    wanted_spec.freq = aCodecCtx->sample_rate;
    //Signed 16-bit samples
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = aCodecCtx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = aCodecCtx;
    
    if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        fprintf(stderr, "open audio failed");
        return -1;
    }

    packet_queue_init(&audioq);
    SDL_PauseAudio(0);
    
    //pcodec
    pCodec = avcodec_find_decoder(pFormatCtx->streams[videoStream]->codecpar->codec_id);
    if (!pCodec) {
        fprintf(stderr, "find video codec failed");
        return -1;
    }
    // pcodecCtx
    pCodecCtx = avcodec_alloc_context3(pCodec);
    
    if(avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[videoStream]->codecpar) < 0){
        fprintf(stderr, "video param to context failed");
        return -1;
    }
    
    if(avcodec_open2(pCodecCtx, pCodec, &videoOptionDict) < 0){
        fprintf(stderr, "open2 video codec ctx failed");
        return -1;
    }
    
    // play
    pFrame = av_frame_alloc();
    
    window = SDL_CreateWindow("player2", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, pCodecCtx->width, pCodecCtx->height,0);
    
    if (window == NULL) {
        fprintf(stderr, "window failed");
        exit(1);
    }
    render = SDL_CreateRenderer(window, -1, 0);
    if (render == NULL) {
        fprintf(stderr, "render failed");
        exit(1);
    }
    
    texture = SDL_CreateTexture(render, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, pCodecCtx->width, pCodecCtx->height);
    if (texture == NULL) {
        fprintf(stderr, "texture failed");
        exit(1);
    }
    
        // initialize SWS context for software scaling
        sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
                                 pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
                                 AV_PIX_FMT_YUV420P,
                                 SWS_BILINEAR,
                                 NULL,
                                 NULL,
                                 NULL);
    
        // set up YV12 pixel array (12 bits per pixel)
        yPlaneSz = pCodecCtx->width * pCodecCtx->height;
        uvPlaneSz = pCodecCtx->width * pCodecCtx->height / 4;
        yPlane = (Uint8*)malloc(yPlaneSz);
        uPlane = (Uint8*)malloc(uvPlaneSz);
        vPlane = (Uint8*)malloc(uvPlaneSz);
        if (!yPlane || !uPlane || !vPlane) {
            fprintf(stderr, "Could not allocate pixel buffers - exiting\n");
            exit(1);
        }
    
        uvPitch = pCodecCtx->width / 2;
    
        /*
         */
        while (av_read_frame(pFormatCtx, &packet) >= 0) {
            // Is this a packet from the video stream?
            if (packet.stream_index == videoStream) {
                // Decode video frame
                ret = avcodec_send_packet(pCodecCtx,&packet);
                if (ret < 0) {
                    fprintf(stderr, "send packet error");
                }
                while (ret >= 0) {
                    ret = avcodec_receive_frame(pCodecCtx, pFrame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    }else if (ret < 0){
                        fprintf(stderr, "receive frame error");
                    }
    
                    if (ret >= 0) {
                        uint8_t *dst[3];
                        dst[0] = yPlane;
                        dst[1] = uPlane;
                        dst[2] = vPlane;
    
                        int size[3];
                        size[0] = pCodecCtx->width;
                        size[1] = uvPitch;
                        size[2] = uvPitch;
    
                        // Convert the image into YUV format that SDL uses
                        int oh = sws_scale(sws_ctx, (uint8_t const * const *) pFrame->data,
                                           pFrame->linesize, 0, pCodecCtx->height, dst,
                                           size);
    
                        if (oh < 0) {
                            fprintf(stderr, "sws scale errro");
                        }
    
                        //                    SDL_Rect *rect = malloc(sizeof(SDL_Rect));
                        //                    int r = random() % 2;
                        //                    if (r == 0) {
                        //                        rect->x = 0;
                        //                        rect->y = 0;
                        //                        rect->w = 100;
                        //                        rect->h = 100;
                        //                    }else if(r == 1){
                        //                        rect->x = 100;
                        //                        rect->y = 100;
                        //                        rect->w = 100;
                        //                        rect->h = 100;
                        //                    }else{
                        //                        rect->x = 200;
                        //                        rect->y = 200;
                        //                        rect->w = 100;
                        //                        rect->h = 100;
                        //                    }
    
                        int res = SDL_UpdateYUVTexture(
                                                       texture,
                                                       NULL,
                                                       yPlane,
                                                       pCodecCtx->width,
                                                       uPlane,
                                                       uvPitch,
                                                       vPlane,
                                                       uvPitch
                                                       );
    
                        if (res != 0) {
                            fprintf(stderr, "sdl update texture error");
                        }
                        SDL_RenderClear(render);
                        SDL_RenderCopy(render, texture, NULL, NULL);
                        SDL_RenderPresent(render);
//                        av_packet_unref(&packet);
                    } // if
                } // while
            }else if (packet.stream_index == audioStream){
                packet_queue_put(&audioq, &packet);
            }else{
                av_packet_unref(&packet);
            }
            
            // Free the packet that was allocated by av_read_frame
            av_packet_unref(&packet);
            SDL_PollEvent(&event);
            switch (event.type) {
                case SDL_QUIT:
                    SDL_DestroyTexture(texture);
                    SDL_DestroyRenderer(render);
                    SDL_DestroyWindow(window);
                    SDL_Quit();
                    exit(0);
                    break;
                default:
                    break;
            }
            
        }// while
        
        // Free the YUV frame
        av_frame_free(&pFrame);
        free(yPlane);
        free(uPlane);
        free(vPlane);
        
        // Close the codec
        avcodec_close(pCodecCtx);
        
        // Close the video file
        avformat_close_input(&pFormatCtx);
        return 0;
}

//typedef struct PacketQueue{
//    AVPacketList *first_pkt,*last_pkt;
//    int
//}PacketQueue;
//
//int playSDL2(){
//    AVFormatContext *pFormatCtx = NULL;
//    int videoStream;
//    unsigned i;
//    AVCodecContext *pCodecCtxOrig = NULL;
//    AVCodecContext *pCodecCtx = NULL;
//    AVCodec *pCodec = NULL;
//    AVFrame *pFrame = NULL;
//    AVPacket packet;
//    struct SwsContext *sws_ctx = NULL;
//    SDL_Event event;
//    SDL_Window *screen;
//    SDL_Renderer *renderer;
//    SDL_Texture *texture;
//    Uint8 *yPlane, *uPlane, *vPlane;
//    size_t yPlaneSz, uvPlaneSz;
//    int uvPitch;
//    
//    // 1. 设置文件路径
//    char *filepath = "/Users/zhangyun/Documents/ffmpegtest.mp4";
//    //    char *filepath = "/Users/zhangyun/Downloads/BBC.Wild.China.6.Tides.of.Change.美丽中国之六潮汐更迭.双语字幕.HR-HDTV.AC3.960×528.X264-人人影视制作.avi";
//    // 2. 注册全部的mutex demutex
//    // Register all formats and codecs
//    av_register_all();
//    
//    // 3. 初始化SDL
//    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
//        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
//        exit(1);
//    }
//    
//    // 4. 根据路径 打开文件流 并获取头部信息
//    // Open video file
//    if (avformat_open_input(&pFormatCtx, filepath, NULL, NULL) != 0)
//        return -1; // Couldn't open file
//    
//    /*
//     Read packets of a media file to get stream information. This is useful for file formats with no headers such as MPEG. This function also computes the real framerate in case of MPEG-2 repeat frame mode. The logical file position is not changed by this function; examined packets may be buffered for later processing.
//     Note
//     this function isn't guaranteed to open all the codecs, so options being non-empty at return is a perfectly normal behavior.
//     To Do
//     Let the user decide somehow what information is needed so that we do not waste time getting stuff the user does not need.
//     
//     */
//    // Retrieve stream information
//    if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
//        return -1; // Couldn't find stream information
//    
//    // Dump information about file onto standard error
//    av_dump_format(pFormatCtx, 0, filepath, 0);
//    
//    // 6.Find the first video stream
//    videoStream = -1;
//    for (i = 0; i < pFormatCtx->nb_streams; i++)
//        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
//            videoStream = i;
//            break;
//        }
//    if (videoStream == -1)
//        return -1; // Didn't find a video stream
//    
//    // 7. 创建 code 上下文
//    pCodecCtxOrig = av_malloc(sizeof(AVCodecContext));
//    avcodec_parameters_to_context(pCodecCtxOrig,pFormatCtx->streams[videoStream]->codecpar);
//    
//    // 8. 查找解码器
//    pCodec = avcodec_find_decoder(pCodecCtxOrig->codec_id);
//    if (pCodec == NULL) {
//        fprintf(stderr, "Unsupported codec!\n");
//        return -1; // Codec not found
//    }
//    
//    // Copy context
//    pCodecCtx = avcodec_alloc_context3(pCodec);
//    if (avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[videoStream]->codecpar)!= 0) {
//        fprintf(stderr, "Couldn't copy codec context");
//        return -1; // Error copying codec context
//    }
//    
//    /*
//     Initialize the AVCodecContext to use the given AVCodec. Prior to using this function the context has to be allocated with avcodec_alloc_context3().
//     The functions avcodec_find_decoder_by_name(), avcodec_find_encoder_by_name(), avcodec_find_decoder() and avcodec_find_encoder() provide an easy way for retrieving a codec.
//     */
//    // 9. 打开解码器
//    // Open codec
//    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0)
//        return -1; // Could not open codec
//    
//    // Allocate video frame
//    pFrame = av_frame_alloc();
//    
//    // Make a screen to put our video
//    screen = SDL_CreateWindow(
//                              "FFmpeg Tutorial",
//                              SDL_WINDOWPOS_UNDEFINED,
//                              SDL_WINDOWPOS_UNDEFINED,
//                              pCodecCtx->width,
//                              pCodecCtx->height,
//                              0
//                              );
//    
//    if (!screen) {
//        fprintf(stderr, "SDL: could not create window - exiting\n");
//        exit(1);
//    }
//    
//    renderer = SDL_CreateRenderer(screen, -1, 0);
//    if (!renderer) {
//        fprintf(stderr, "SDL: could not create renderer - exiting\n");
//        exit(1);
//    }
//    
//    // Allocate a place to put our YUV image on that screen
//    texture = SDL_CreateTexture(
//                                renderer,
//                                SDL_PIXELFORMAT_YV12,
//                                SDL_TEXTUREACCESS_STREAMING,
//                                pCodecCtx->width,
//                                pCodecCtx->height
//                                );
//    if (!texture) {
//        fprintf(stderr, "SDL: could not create texture - exiting\n");
//        exit(1);
//    }
//    
//    // initialize SWS context for software scaling
//    sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
//                             pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
//                             AV_PIX_FMT_YUV420P,
//                             SWS_BILINEAR,
//                             NULL,
//                             NULL,
//                             NULL);
//    
//    // set up YV12 pixel array (12 bits per pixel)
//    yPlaneSz = pCodecCtx->width * pCodecCtx->height;
//    uvPlaneSz = pCodecCtx->width * pCodecCtx->height / 4;
//    yPlane = (Uint8*)malloc(yPlaneSz);
//    uPlane = (Uint8*)malloc(uvPlaneSz);
//    vPlane = (Uint8*)malloc(uvPlaneSz);
//    if (!yPlane || !uPlane || !vPlane) {
//        fprintf(stderr, "Could not allocate pixel buffers - exiting\n");
//        exit(1);
//    }
//    
//    int ret = 0;
//    uvPitch = pCodecCtx->width / 2;
//    
//    /*
//     */
//    while (av_read_frame(pFormatCtx, &packet) >= 0) {
//        // Is this a packet from the video stream?
//        if (packet.stream_index == videoStream) {
//            // Decode video frame
//            ret = avcodec_send_packet(pCodecCtx,&packet);
//            if (ret < 0) {
//                fprintf(stderr, "send packet error");
//            }
//            while (ret >= 0) {
//                ret = avcodec_receive_frame(pCodecCtx, pFrame);
//                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
//                    break;
//                }else if (ret < 0){
//                    fprintf(stderr, "receive frame error");
//                }
//                
//                if (ret >= 0) {
//                    uint8_t *dst[3];
//                    dst[0] = yPlane;
//                    dst[1] = uPlane;
//                    dst[2] = vPlane;
//                    
//                    int size[3];
//                    size[0] = pCodecCtx->width;
//                    size[1] = uvPitch;
//                    size[2] = uvPitch;
//                    
//                    // Convert the image into YUV format that SDL uses
//                    int oh = sws_scale(sws_ctx, (uint8_t const * const *) pFrame->data,
//                                       pFrame->linesize, 0, pCodecCtx->height, dst,
//                                       size);
//                    
//                    if (oh < 0) {
//                        fprintf(stderr, "sws scale errro");
//                    }
//                    
//                    //                    SDL_Rect *rect = malloc(sizeof(SDL_Rect));
//                    //                    int r = random() % 2;
//                    //                    if (r == 0) {
//                    //                        rect->x = 0;
//                    //                        rect->y = 0;
//                    //                        rect->w = 100;
//                    //                        rect->h = 100;
//                    //                    }else if(r == 1){
//                    //                        rect->x = 100;
//                    //                        rect->y = 100;
//                    //                        rect->w = 100;
//                    //                        rect->h = 100;
//                    //                    }else{
//                    //                        rect->x = 200;
//                    //                        rect->y = 200;
//                    //                        rect->w = 100;
//                    //                        rect->h = 100;
//                    //                    }
//                    
//                    int res = SDL_UpdateYUVTexture(
//                                                   texture,
//                                                   NULL,
//                                                   yPlane,
//                                                   pCodecCtx->width,
//                                                   uPlane,
//                                                   uvPitch,
//                                                   vPlane,
//                                                   uvPitch
//                                                   );
//                    
//                    if (res != 0) {
//                        fprintf(stderr, "sdl update texture error");
//                    }
//                    SDL_RenderClear(renderer);
//                    SDL_RenderCopy(renderer, texture, NULL, NULL);
//                    SDL_RenderPresent(renderer);
//                }
//            }
//        }
//        // Free the packet that was allocated by av_read_frame
//        av_packet_unref(&packet);
//        SDL_PollEvent(&event);
//        switch (event.type) {
//            case SDL_QUIT:
//                SDL_DestroyTexture(texture);
//                SDL_DestroyRenderer(renderer);
//                SDL_DestroyWindow(screen);
//                SDL_Quit();
//                exit(0);
//                break;
//            default:
//                break;
//        }
//        
//    }// while
//    
//    // Free the YUV frame
//    av_frame_free(&pFrame);
//    free(yPlane);
//    free(uPlane);
//    free(vPlane);
//    
//    // Close the codec
//    avcodec_close(pCodecCtx);
//    avcodec_close(pCodecCtxOrig);
//    
//    // Close the video file
//    avformat_close_input(&pFormatCtx);
//    return 0;
//}
