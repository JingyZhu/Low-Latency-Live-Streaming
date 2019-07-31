/*  
 * MIT License
 *
 * Copyright (c) 2019 Jingyuan Zhu
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavdevice/avdevice.h>
#include <libavutil/imgutils.h>
 
int main(int argc, char* argv[])
{
 
	AVFormatContext	*pFormatCtx;
	int				i, videoindex;
	AVCodecContext	*pCodecCtx;
	AVCodec			*pCodec;

	char* video_size = "1280*720";
	char* framerate = "30";
	
	// Deprecated
	// av_register_all();
	avformat_network_init();
	pFormatCtx = avformat_alloc_context();
 
	//Register Device Deprecated
	avdevice_register_all();

	AVDictionary* options = NULL;
	//Set some options
	//grabbing frame rate
	av_dict_set(&options,"framerate", framerate, 0);
	//Make the grabbed area follow the mouse
	//av_dict_set(&options,"follow_mouse","centered",0);
	//Video frame size. The default is to capture the full screen
	av_dict_set(&options, "video_size", video_size, 0);
	AVInputFormat *ifmt=av_find_input_format("x11grab");
	//Grab at position 10,20
	if(avformat_open_input(&pFormatCtx,":0.0",ifmt,&options)!=0){
		fprintf(stderr, "Couldn't open input stream.\n");
		return -1;
	}
 
	if(avformat_find_stream_info(pFormatCtx,NULL)<0){
		fprintf(stderr, "Couldn't find stream information.\n");
		return -1;
	}

	videoindex = -1;
	for(i = 0; i < pFormatCtx->nb_streams; i++) 
		if(pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO){
			videoindex = i;
			break;
		}

	if(videoindex == -1){
		fprintf(stderr, "Didn't find a video stream.\n");
		return -1;
	}
	
	pCodec = avcodec_find_decoder(pFormatCtx->streams[videoindex]->codecpar->codec_id);
	pCodecCtx = avcodec_alloc_context3(pCodec);
	avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[videoindex]->codecpar);

    if(pCodec==NULL){
		fprintf(stderr, "Codec not found.\n");
		return -1;
	}

	if(avcodec_open2(pCodecCtx, pCodec,NULL)<0){
		fprintf(stderr, "Could not open codec.\n");
		return -1;
	}

	AVFrame	*pFrame,*pFrameYUV;
	pFrame = av_frame_alloc();
	pFrameYUV = av_frame_alloc();
	unsigned char *out_buffer=(unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_NV12, pCodecCtx->width, pCodecCtx->height, 1));
	av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, out_buffer, AV_PIX_FMT_NV12, pCodecCtx->width, pCodecCtx->height, 1);
	int ret;
 
	AVPacket *packet = (AVPacket *)av_malloc(sizeof(AVPacket));
 
    FILE *fp_yuv = fopen("output.yuv","wb+");  
 
	struct SwsContext *img_convert_ctx;
	img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_NV12, 0, NULL, NULL, NULL); 
 
	while(1) {
        if(av_read_frame(pFormatCtx, packet) < 0)
            break;
        if(packet->stream_index==videoindex){
			ret = avcodec_send_packet(pCodecCtx, packet);
            if(ret < 0){
                fprintf(stderr, "Decode Error.\n");
                return -1;
            }
			while( avcodec_receive_frame(pCodecCtx, pFrame) == 0){
                sws_scale(img_convert_ctx, (const unsigned char* const*)pFrame->data, pFrame->linesize, 0, pCodecCtx->height, pFrameYUV->data, pFrameYUV->linesize);
                int y_size = pCodecCtx->width*pCodecCtx->height;
                fwrite(pFrameYUV->data[0], 1, y_size, fp_yuv);    //Y   
                fwrite(pFrameYUV->data[1], 1, y_size/2, fp_yuv);  //U  
            }
        }
        av_packet_unref(packet);
    }
 
 
	sws_freeContext(img_convert_ctx);
 
    fclose(fp_yuv);
	//av_free(out_buffer);
	av_free(pFrameYUV);
	avcodec_close(pCodecCtx);
	avformat_close_input(&pFormatCtx);
 
	return 0;
}