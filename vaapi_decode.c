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
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libavutil/imgutils.h>


static AVBufferRef *hw_device_ctx = NULL;
static FILE *output_file = NULL;
static unsigned int data_size = -1;
static unsigned char* sps_pps = NULL; // = {0, 0, 0, 0x1, 0x67, 0x64, 0x1c, 0x14, 0xac, 0x2c, 0xb0, 0x14, 0x1, 0x6e, 0xc0, 0x44, 0, 0, 0x3, 0, 0x4, 0, 0, 0x3, 0, 0xca, 0x3c, 0x20, 0x10, 0xa8, 0, 0, 0, 0x1, 0x68, 0xee, 0x6, 0xe2, 0xc0};

static int get_video_extradata(AVFormatContext *s, int video_index)
{ 
    AVCodecParameters *codecpar = s->streams[video_index]->codecpar;
    codecpar->extradata_size = data_size;
    codecpar->extradata = (uint8_t*)av_malloc(data_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (codecpar->extradata == NULL) {
        printf("could not av_malloc the video params extradata!\n");
        return -1;
    }
    memcpy(codecpar->extradata, sps_pps, data_size);
   return 0;
}

int init_decode(AVFormatContext *s)
{
     s->iformat = av_find_input_format("h264");
     int video_index = -1;
     int ret = -1;

     if (!s)
          return ret;
    
    video_index = 0;

    //Init the video codec(H264).
     s->streams[video_index]->codecpar->codec_id = AV_CODEC_ID_H264;
     ret = get_video_extradata(s, video_index);

     /*Update the AVFormatContext Info*/
     s->nb_streams = 1;
    
     return 0;
}

static int get_sps_pps(void* packet_data, unsigned char** metadata){
    // This functon assumes that sps apperars before pps header
    // Assume we can find sps pps 
    char* buffer = (char*) packet_data;
    int idx = 0;
    int sps_begin = -1, sps_end = -1;
    int pps_begin = -1, pps_end = -1;
    char NALU_header[4] = {0x00, 0x00, 0x00, 0x01};
    while(1){
        if (!memcmp(buffer + idx, NALU_header, 4)){ // Find NALU Header
            idx += 4;
            if (*buffer == 0x67){ // sps
                sps_begin = idx - 4;
                while (memcmp(buffer + idx, NALU_header, 4)) idx++; // Iterate until next NALU Header
                sps_end = idx;
            } else if (*buffer == 0x68){// pps
                pps_begin = idx - 4;
                while (memcmp(buffer + idx, NALU_header, 4)) idx++; // Iterate until next NALU Header
                pps_end = idx;
                break;
            }
        } else idx++;
    }
    int data_size = sps_end - sps_begin + pps_end - pps_begin;
    *metadata = malloc(data_size);
    memcpy(*metadata, buffer + sps_begin, sps_end - sps_begin);
    memcpy(*metadata + sps_end - sps_begin, buffer + pps_begin, pps_end - pps_begin);
    return data_size;
}

static int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type)
{
    int err = 0;

    if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type,
                                      NULL, NULL, 0)) < 0) {
        fprintf(stderr, "Failed to create specified HW device.\n");
        return err;
    }
    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

    return err;
}


static int decode_write(AVCodecContext *avctx, AVPacket *packet)
{
    AVFrame *frame = NULL, *sw_frame = NULL;
    AVFrame *tmp_frame = NULL;
    uint8_t *buffer = NULL;
    int size;
    int ret = 0;

    ret = avcodec_send_packet(avctx, packet);
    if (ret < 0) {
        fprintf(stderr, "Error during decoding\n");
        return ret;
    }

    while (1) {
        if (!(frame = av_frame_alloc()) || !(sw_frame = av_frame_alloc())) {
            fprintf(stderr, "Can not alloc frame\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        ret = avcodec_receive_frame(avctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_frame_free(&frame);
            av_frame_free(&sw_frame);
            return 0;
        } else if (ret < 0) {
            fprintf(stderr, "Error while decoding\n");
            goto fail;
        }

        if (frame->format == AV_PIX_FMT_VAAPI) {
            /* retrieve data from GPU to CPU */
            if ((ret = av_hwframe_transfer_data(sw_frame, frame, 0)) < 0) {
                fprintf(stderr, "Error transferring the data to system memory\n");
                goto fail;
            }
            tmp_frame = sw_frame;
        } else
            tmp_frame = frame;

        size = av_image_get_buffer_size(tmp_frame->format, tmp_frame->width,
                                        tmp_frame->height, 1);
        buffer = av_malloc(size);
        if (!buffer) {
            fprintf(stderr, "Can not alloc buffer\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ret = av_image_copy_to_buffer(buffer, size,
                                      (const uint8_t * const *)tmp_frame->data,
                                      (const int *)tmp_frame->linesize, tmp_frame->format,
                                      tmp_frame->width, tmp_frame->height, 1);
        if (ret < 0) {
            fprintf(stderr, "Can not copy image to buffer\n");
            goto fail;
        }

        if ((ret = fwrite(buffer, 1, size, output_file)) < 0) {
            fprintf(stderr, "Failed to dump raw data.\n");
            goto fail;
        }
        fflush(output_file);
    fail:
        av_frame_free(&frame);
        av_frame_free(&sw_frame);
        av_freep(&buffer);
        if (ret < 0)
            return ret;
    }
}

int main(int argc, char *argv[])
{
    AVCodec *decoder = NULL;
    AVCodecContext *decoder_ctx = NULL;
    AVFormatContext *input_ctx = NULL;
    int video_stream, ret;
    AVStream *video = NULL;
    AVPacket packet;

    AVDictionary *AV_Dict = NULL;
    AVInputFormat *AV_in = NULL;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input file> <output file>\n", argv[0]);
        return -1;
    }

    char *infilename = malloc(strlen(argv[1]) + 15), *outfilename = malloc(strlen(argv[2]) + 15);

    ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Failed to create a VAAPI device. Error code: %s\n", av_err2str(ret));
        return -1;
    }

    if (!strcmp(argv[1], "-")){
        strcpy(infilename, "/dev/stdin");
        fread(&data_size, sizeof(data_size), 1, stdin);
        sps_pps =  malloc(data_size);
        fread(sps_pps, sizeof(char), data_size, stdin);
        AV_in = av_find_input_format("h264");
        // Buffer has the right size in stdin
    }
    else{
        strcpy(infilename, argv[1]);
        // TODO Find sps pps mannually
        FILE* fin;
        fin = fopen(infilename, "r");
        unsigned char buffer[300];
        fread(buffer, 300, 1, fin);
        data_size = get_sps_pps(buffer, &sps_pps);
        fclose(fin);
    }

    av_dict_set(&AV_Dict, "format_probesize", "0", 0);

    /* open the input file */
    if (avformat_open_input(&input_ctx, infilename, AV_in, &AV_Dict) != 0) {
        fprintf(stderr, "Cannot open input file '%s'\n", infilename);
        return -1;
    }
    // input_ctx->flags = AVFMT_FLAG_AUTO_BSF + AVFMT_FLAG_NONBLOCK;
    if (init_decode(input_ctx) < 0) {
        fprintf(stderr, "Cannot find input stream information.\n");
        return -1;
    }

    /* find the video stream information */
    ret = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (ret < 0) {
        fprintf(stderr, "Cannot find a video stream in the input file\n");
        return -1;
    }
    video_stream = 0;

    if (!(decoder_ctx = avcodec_alloc_context3(decoder)))
        return AVERROR(ENOMEM);

    video = input_ctx->streams[video_stream];
    if (avcodec_parameters_to_context(decoder_ctx, video->codecpar) < 0)
        return -1;


    if (hw_decoder_init(decoder_ctx, AV_HWDEVICE_TYPE_VAAPI) < 0)
        return -1;

    if ((ret = avcodec_open2(decoder_ctx, decoder, NULL)) < 0) {
        fprintf(stderr, "Failed to open codec for stream #%u\n", video_stream);
        return -1;
    }

    if (!strcmp(argv[2], "-")) strcpy(outfilename, "/dev/stdout");
    else strcpy(outfilename, argv[2]);
    /* open the file to dump raw data */
    output_file = fopen(outfilename, "w+");

    /* actual decoding and dump the raw data */
    while (ret >= 0) {
        if ((ret = av_read_frame(input_ctx, &packet)) < 0)
            break;
        if (video_stream == packet.stream_index)
            ret = decode_write(decoder_ctx, &packet);
        av_packet_unref(&packet);
    }

    /* flush the decoder */
    packet.data = NULL;
    packet.size = 0;
    ret = decode_write(decoder_ctx, &packet);
    av_packet_unref(&packet);

    if (output_file)
        fclose(output_file);
    avcodec_free_context(&decoder_ctx);
    avformat_close_input(&input_ctx);
    av_buffer_unref(&hw_device_ctx);
    free(infilename);
    free(outfilename);

    return 0;
}
