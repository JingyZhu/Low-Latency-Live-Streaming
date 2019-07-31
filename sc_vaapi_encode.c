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
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavdevice/avdevice.h>
#include <libavutil/imgutils.h>

static int width, height, fps;
static AVBufferRef *hw_device_ctx = NULL;
static int metadata_sent = 0;
static unsigned char *metadata = NULL;
static int data_length = -1;

static int init_x11grab(AVFormatContext *pFormatCtx, AVCodecContext **pCodecCtx, AVCodec **pCodec){
	int i, videoindex = -1;
    if(avformat_find_stream_info(pFormatCtx, NULL)<0){
		printf("Couldn't find stream information.\n");
		return -1;
	}

	for(i = 0; i < pFormatCtx->nb_streams; i++) 
		if(pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO){
			videoindex = i;
			break;
		}

	if(videoindex == -1){
		fprintf(stderr, "Didn't find a video stream.\n");
		return -1;
	}
	
    *pCodec = avcodec_find_decoder(pFormatCtx->streams[videoindex]->codecpar->codec_id);
    *pCodecCtx = avcodec_alloc_context3(*pCodec);
    avcodec_parameters_to_context(*pCodecCtx, pFormatCtx->streams[videoindex]->codecpar);
	
    if(*pCodec==NULL){
		fprintf(stderr, "Codec not found.\n");
		return -1;
	}

	if(avcodec_open2(*pCodecCtx, *pCodec, NULL)<0){
		fprintf(stderr, "Could not open codec.\n");
		return -1;
	}

    return 0;
}

static int get_sps_pps(void* packet_data, unsigned char** metadata){
    // This functon assumes that sps apperars before pps header
    char* buffer = (char*) packet_data;
    int data_length= -1;
    int idx = 0;
    int sps_begin = -1, sps_end = -1;
    int pps_begin = -1, pps_end = -1;
    char NALU_header[4] = {0x00, 0x00, 0x00, 0x01};
    while(1){
        if (!memcmp(buffer + idx, NALU_header, 4)){ // Find NALU Header
            idx += 4;
            if (*(buffer + idx) == 0x67){ // sps
                sps_begin = idx - 4;
                while (memcmp(buffer + idx, NALU_header, 4)) idx++; // Iterate until next NALU Header
                sps_end = idx;
            } else if (*(buffer + idx) == 0x68){// pps
                pps_begin = idx - 4;
                while (memcmp(buffer + idx, NALU_header, 4)) idx++; // Iterate until next NALU Header
                pps_end = idx;
                break;
            }
        }
        else idx++;
    }
    data_length = sps_end - sps_begin + pps_end - pps_begin;
    *metadata = malloc(data_length);
    memcpy(*metadata, buffer + sps_begin, sps_end - sps_begin);
    memcpy(*metadata + sps_end - sps_begin, buffer + pps_begin, pps_end - pps_begin);
    return data_length;
}

static int set_hwframe_ctx(AVCodecContext *ctx, AVBufferRef *hw_device_ctx)
{
    AVBufferRef *hw_frames_ref;
    AVHWFramesContext *frames_ctx = NULL;
    int err = 0;

    if (!(hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx))) {
        fprintf(stderr, "Failed to create VAAPI frame context.\n");
        return -1;
    }
    frames_ctx = (AVHWFramesContext *)(hw_frames_ref->data);
    frames_ctx->format    = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width     = width;
    frames_ctx->height    = height;
    frames_ctx->initial_pool_size = 20;
    if ((err = av_hwframe_ctx_init(hw_frames_ref)) < 0) {
        fprintf(stderr, "Failed to initialize VAAPI frame context."
                "Error code: %s\n",av_err2str(err));
        av_buffer_unref(&hw_frames_ref);
        return err;
    }
    ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!ctx->hw_frames_ctx)
        err = AVERROR(ENOMEM);

    av_buffer_unref(&hw_frames_ref);
    return err;
}

static int encode_write(AVCodecContext *avctx, AVFrame *frame, FILE *fout)
{
    int ret = 0;
    AVPacket enc_pkt;

    av_init_packet(&enc_pkt);
    enc_pkt.data = NULL;
    enc_pkt.size = 0;

    if ((ret = avcodec_send_frame(avctx, frame)) < 0) {
        fprintf(stderr, "Error code: %s\n", av_err2str(ret));
        goto end;
    }
    while (1) {
        ret = avcodec_receive_packet(avctx, &enc_pkt);
        if (ret)
            break;

        enc_pkt.stream_index = 0;
        if (!metadata_sent){
            data_length = get_sps_pps(enc_pkt.data, &metadata);
            ret = fwrite(&data_length, sizeof(data_length), 1, fout);
            ret = fwrite(metadata, sizeof(char), data_length, fout);
            fflush(fout);
            metadata_sent = 1;
            ret = fwrite(enc_pkt.data, sizeof(char), data_length, fout);
        }
        usleep(1e2);
        ret = fwrite(enc_pkt.data + data_length, 1, enc_pkt.size - data_length, fout);
        ret = fwrite(metadata, sizeof(char), data_length, fout);
        av_packet_unref(&enc_pkt);
        fflush(fout);
    }

end:
    ret = ((ret == AVERROR(EAGAIN)) ? 0 : -1);
    return ret;
}

int main(int argc, char *argv[])
{
    int             err;
    FILE            *fin = NULL, *fout = NULL;
    AVFrame         *hw_frame = NULL;
    AVCodecContext  *avctx = NULL;
    AVCodec         *codec  = NULL;
    const char      *enc_name = "h264_vaapi";

    AVFormatContext	*pFormatCtx;
	AVCodecContext	*pCodecCtx;
	AVCodec			*pCodec;

    if (argc < 4) {
        fprintf(stderr, "Usage: %s <width> <height> <fps>\n", argv[0]);
        return -1;
    }

    char *infilename = "/dev/stdin", *outfilename = "/dev/stdout";
    width  = atoi(argv[1]);
    height = atoi(argv[2]);
    fps = atoi(argv[3]);

    if (!(fin = fopen(infilename, "r"))) {
        fprintf(stderr, "Fail to open input file : %s\n", strerror(errno));
        return -1;
    }
    if (!(fout = fopen(outfilename, "w+b"))) {
        fprintf(stderr, "Fail to open output file : %s\n", strerror(errno));
        err = -1;
        goto close;
    }
    
    // Deprecated
    // av_register_all();
	pFormatCtx = avformat_alloc_context();
    avdevice_register_all();

	AVDictionary* options = NULL;
	//Set some options
	//grabbing frame rate
	av_dict_set(&options, "framerate", argv[3], 0);
	//av_dict_set(&options,"follow_mouse","centered",0);
	//Video frame size. The default is to capture the full screen
    char resolution[10] = {0};
    sprintf(resolution, "%d*%d", width, height);
	av_dict_set(&options, "video_size", resolution, 0);
	AVInputFormat *ifmt = av_find_input_format("x11grab");

    // Open x11
    //Grab at position 10,20
	if(avformat_open_input(&pFormatCtx,":0.0", ifmt, &options) !=0){
		printf("Couldn't open input stream.\n");
		return -1;
	}

    if (init_x11grab(pFormatCtx, &pCodecCtx, &pCodec) < 0) {
        fprintf(stderr, "Error initialize X11\n");
        return -1;
    }

    // Create HW encoder ctx
    err = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                 NULL, NULL, 0);
    if (err < 0) {
        fprintf(stderr, "Failed to create a VAAPI device. Error code: %s\n", av_err2str(err));
        goto close;
    }

    if (!(codec = avcodec_find_encoder_by_name(enc_name))) {
        fprintf(stderr, "Could not find encoder.\n");
        err = -1;
        goto close;
    }

    if (!(avctx = avcodec_alloc_context3(codec))) {
        err = AVERROR(ENOMEM);
        goto close;
    }

    avctx->width     = width;
    avctx->height    = height;
    avctx->time_base = (AVRational){1, fps};
    avctx->framerate = (AVRational){fps, 1};
    avctx->sample_aspect_ratio = (AVRational){1, 1};
    avctx->pix_fmt   = AV_PIX_FMT_VAAPI;
    avctx->max_b_frames = 0;
    avctx->gop_size = 1;
    avctx->level = 20;
    avctx->qmin = 10;
    avctx->qmax = 30;
    avctx->global_quality = 35;

    /* set hw_frames_ctx for encoder's AVCodecContext */
    if ((err = set_hwframe_ctx(avctx, hw_device_ctx)) < 0) {
        fprintf(stderr, "Failed to set hwframe context.\n");
        goto close;
    }

    if ((err = avcodec_open2(avctx, codec, NULL)) < 0) {
        fprintf(stderr, "Cannot open video encoder codec. Error code: %s\n", av_err2str(err));
        goto close;
    }
    // End of hw encoder init

    AVFrame	*pFrame,*pFrameNV12;
	pFrame = av_frame_alloc();
	pFrameNV12 = av_frame_alloc();
	unsigned char *out_buffer = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_NV12, pCodecCtx->width, pCodecCtx->height, 1));

	av_image_fill_arrays(pFrameNV12->data, pFrameNV12->linesize, out_buffer, AV_PIX_FMT_NV12, pCodecCtx->width, pCodecCtx->height, 1);

    struct SwsContext *img_convert_ctx;
	img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_NV12, 0, NULL, NULL, NULL); 

    AVPacket *packet = (AVPacket *)av_malloc(sizeof(AVPacket));
    int ret, got_picture;

    while (1) {
        if(av_read_frame(pFormatCtx, packet) < 0)
            break;
        ret = avcodec_send_packet(pCodecCtx, packet);
        if(ret < 0){
            printf("Decode Error.\n");
            return -1;
        }
        got_picture = avcodec_receive_frame(pCodecCtx, pFrame);
        if(got_picture) continue;

        sws_scale(img_convert_ctx, (const unsigned char* const*)pFrame->data, pFrame->linesize, 0, pCodecCtx->height, pFrameNV12->data, pFrameNV12->linesize);
        pFrameNV12->width = width;
        pFrameNV12->height = height;
        pFrameNV12->format = AV_PIX_FMT_NV12;

        if (!(hw_frame = av_frame_alloc())) {
            err = AVERROR(ENOMEM);
            goto close;
        }
        if ((err = av_hwframe_get_buffer(avctx->hw_frames_ctx, hw_frame, 0)) < 0) {
            fprintf(stderr, "Error code: %s.\n", av_err2str(err));
            goto close;
        }
        if (!hw_frame->hw_frames_ctx) {
            err = AVERROR(ENOMEM);
            goto close;
        }
        if ((err = av_hwframe_transfer_data(hw_frame, pFrameNV12, 0)) < 0) {
            fprintf(stderr, "Error while transferring frame data to surface."
                    "Error code: %s.\n", av_err2str(err));
            goto close;
        }
        if ((err = (encode_write(avctx, hw_frame, fout))) < 0) {
            fprintf(stderr, "Failed to encode.\n");
            goto close;
        }

        av_frame_free(&hw_frame);
        av_packet_unref(packet);

    }
    /* flush encoder */
    err = encode_write(avctx, NULL, fout);
    if (err == AVERROR_EOF)
        err = 0;

close:
    if (fin)
        fclose(fin);
    if (fout){
        fclose(fout);
    }
    av_frame_free(&pFrame);
    av_frame_free(&pFrameNV12);
    av_frame_free(&hw_frame);
    avcodec_free_context(&avctx);
    av_buffer_unref(&hw_device_ctx);
    free(metadata);

    return err;
}
