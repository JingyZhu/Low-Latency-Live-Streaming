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
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>

static const int num_ts = 1000;
static int width, height, fps;
static AVBufferRef *hw_device_ctx = NULL;
static int metadata_sent = 0;
static unsigned char *metadata = NULL;
static int data_length = -1;

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
            // fprintf(stderr, "metadata: %d\n", data_length);
            ret = fwrite(metadata, sizeof(char), data_length, fout);
            fflush(fout);
            metadata_sent = 1;
            ret = fwrite(enc_pkt.data, sizeof(char), data_length, fout);
        }
        usleep(1e2);
        ret = fwrite(enc_pkt.data + data_length, 1, enc_pkt.size - data_length, fout);
        ret = fwrite(metadata, sizeof(char), data_length, fout);
        fprintf(stderr, "Write Packet: %d\n", enc_pkt.size);
        // fprintf(stderr, "\nFirst 39: ");
        // for (int w=0; w < 39; w++) fprintf(stderr, "%#0x ", *(enc_pkt.data + w));
        av_packet_unref(&enc_pkt);
        fflush(fout);
    }

end:
    ret = ((ret == AVERROR(EAGAIN)) ? 0 : -1);
    return ret;
}

int main(int argc, char *argv[])
{
    int size, err;
    FILE *fin = NULL, *fout = NULL;
    AVFrame *sw_frame = NULL, *hw_frame = NULL;
    AVCodecContext *avctx = NULL;
    AVCodec *codec = NULL;
    const char *enc_name = "h264_vaapi";
    struct timespec ts[num_ts];

    if (argc < 6) {
        fprintf(stderr, "Usage: %s <width> <height> <fps> <input file> <output file>\n", argv[0]);
        return -1;
    }

    width  = atoi(argv[1]);
    height = atoi(argv[2]);
    fps = atoi(argv[3]);
    size   = width * height;

    char *infilename = malloc(strlen(argv[3]) + 15), *outfilename = malloc(strlen(argv[4]) + 15);
    if (!strcmp(argv[4], "-")) strcpy(infilename, "/dev/stdin");
    else strcpy(infilename, argv[4]);
    if (!strcmp(argv[5], "-")) strcpy(outfilename, "/dev/stdout");
    else strcpy(outfilename, argv[5]);

    if (!(fin = fopen(infilename, "r"))) {
        fprintf(stderr, "Fail to open input file : %s\n", strerror(errno));
        return -1;
    }
    if (!(fout = fopen(outfilename, "w+b"))) {
        fprintf(stderr, "Fail to open output file : %s\n", strerror(errno));
        err = -1;
        goto close;
    }

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


    /* set hw_frames_ctx for encoder's AVCodecContext */
    if ((err = set_hwframe_ctx(avctx, hw_device_ctx)) < 0) {
        fprintf(stderr, "Failed to set hwframe context.\n");
        goto close;
    }

    if ((err = avcodec_open2(avctx, codec, NULL)) < 0) {
        fprintf(stderr, "Cannot open video encoder codec. Error code: %s\n", av_err2str(err));
        goto close;
    }

    int n_frame = 0;

    while (1) {
        if (!(sw_frame = av_frame_alloc())) {
            err = AVERROR(ENOMEM);
            goto close;
        }
        /* read data into software frame, and transfer them into hw frame */
        sw_frame->width  = width;
        sw_frame->height = height;
        sw_frame->format = AV_PIX_FMT_NV12;
        if ((err = av_frame_get_buffer(sw_frame, 32)) < 0)
            goto close;
        if ((err = fread((uint8_t*)(sw_frame->data[0]), size, 1, fin)) <= 0)
            break;
        if ((err = fread((uint8_t*)(sw_frame->data[1]), size/2, 1, fin)) <= 0)
            break;

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
        if ((err = av_hwframe_transfer_data(hw_frame, sw_frame, 0)) < 0) {
            fprintf(stderr, "Error while transferring frame data to surface."
                    "Error code: %s.\n", av_err2str(err));
            goto close;
        }

        if ((err = (encode_write(avctx, hw_frame, fout))) < 0) {
            fprintf(stderr, "Failed to encode.\n");
            goto close;
        }
        clock_gettime(CLOCK_MONOTONIC, ts + n_frame);
        av_frame_free(&hw_frame);
        av_frame_free(&sw_frame);
        n_frame++;
        usleep(1e4);
    }

    for (int i = 0; i < n_frame; i++)
        fprintf(stderr, "#Frame: %d, timespec %ld.%ld\n", i, ts[i].tv_sec, ts[i].tv_nsec);
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
    av_frame_free(&sw_frame);
    av_frame_free(&hw_frame);
    avcodec_free_context(&avctx);
    av_buffer_unref(&hw_device_ctx);
    free(infilename);
    free(outfilename);
    free(metadata);

    return err;
}
