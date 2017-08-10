//
//  02SDLPlayerTest.c
//  player2
//
//  Created by zhangyun on 10/08/2017.
//  Copyright © 2017 zhangyun. All rights reserved.
//

#include "02SDLPlayerTest.h"


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


int play(){
    
    AVFormatContext *fctx;
    int ret = -1;
    
    char *file_path = "/Users/zhangyun/Downloads/BBC.Wild.China.6.Tides.of.Change.美丽中国之六潮汐更迭.双语字幕.HR-HDTV.AC3.960×528.X264-人人影视制作.avi";
    
    av_register_all();
    
   ret = avformat_open_input(&fctx, file_path, NULL, NULL);
    
    if (ret != 0) {
        fprintf(stderr, "open input fail");
    }
    
    avformat_find_stream_info(fctx, NULL);
    
    
    
    
    
    
    
    return 0;
}
