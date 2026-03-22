//
// Created by 田中伸明 on 2025/06/13.
//

#include <jni.h>
#include <string>
#include <android/log.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/timestamp.h>
#include <libavutil/opt.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libswscale/swscale.h>
}

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "FFmpegJNI", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "FFmpegJNI", __VA_ARGS__)

static inline void release_utf(JNIEnv* env, jstring jstr, const char* cstr) {
    if (cstr) env->ReleaseStringUTFChars(jstr, cstr);
}
static void init_ffmpeg_once() {
    static bool inited = false;
    if (!inited) {
        av_log_set_level(AV_LOG_INFO);
        inited = true;
    }
}
static void log_fferr(int err) {
    char buf[128];
    av_strerror(err, buf, sizeof(buf));
    LOGE("FFmpeg err %d: %s", err, buf);
}

// =============================================================================
// process_with_filter : 1 入力 1 出力のフィルタグラフ共通処理
//   fg_end : フィルタグラフの解放・後始末専用ラベル
//   end    : すべてのリソース解放＆return 共通ラベル
// =============================================================================
static int process_with_filter(const char* inPath, const char* outPath, const char* filter_descr) {
    int ret = 0;

    // ---------- 構造体の初期化 ----------
    AVFormatContext *ifmt = nullptr;
    AVFormatContext *ofmt = nullptr;
    AVPacket pkt; av_init_packet(&pkt); pkt.data = nullptr; pkt.size = 0;
    AVStream *in_v = nullptr, *out_v = nullptr, *out_a = nullptr;
    int vindex = -1, aindex = -1;
    AVFilterGraph *graph = nullptr;
    AVFilterContext *src_ctx = nullptr, *sink_ctx = nullptr;
    AVFilterInOut *outputs = nullptr, *inputs = nullptr;
    AVFrame *frame = nullptr;
    char args[256];
    const AVFilter *buffersrc;
    const AVFilter *buffersink;
    AVRational tb;
    AVCodecContext* dec_ctx = nullptr;
    AVCodecContext* enc_ctx = nullptr;
    const AVCodec* decoder = avcodec_find_decoder(in_v->codecpar->codec_id);
    // ---------- 入力ファイル ----------
    if ((ret = avformat_open_input(&ifmt, inPath, nullptr, nullptr)) < 0) return ret;
    if ((ret = avformat_find_stream_info(ifmt, nullptr)) < 0) goto cleanup;

    for (unsigned i = 0; i < ifmt->nb_streams; ++i) {
        if (ifmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vindex == -1) vindex = i;
        if (ifmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && aindex == -1) aindex = i;
    }
    if (vindex == -1) { ret = AVERROR_STREAM_NOT_FOUND; goto cleanup; }
    in_v = ifmt->streams[vindex];

    // ---------- 出力ファイル ----------
    if ((ret = avformat_alloc_output_context2(&ofmt, nullptr, nullptr, outPath)) < 0) goto cleanup;
    out_v = avformat_new_stream(ofmt, nullptr);
    avcodec_parameters_copy(out_v->codecpar, in_v->codecpar);
    out_v->codecpar->codec_tag = 0;

    if (aindex != -1) {
        AVStream* in_a = ifmt->streams[aindex];
        out_a = avformat_new_stream(ofmt, nullptr);
        avcodec_parameters_copy(out_a->codecpar, in_a->codecpar);
        out_a->codecpar->codec_tag = 0;
    }

    if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&ofmt->pb, outPath, AVIO_FLAG_WRITE)) < 0) goto cleanup;
    }
    if ((ret = avformat_write_header(ofmt, nullptr)) < 0) goto cleanup;

    // ---------- フィルタグラフ構築 ----------
    graph = avfilter_graph_alloc();
    if (!graph) { ret = AVERROR(ENOMEM); goto cleanup;
    }

    tb = in_v->time_base;
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             in_v->codecpar->width, in_v->codecpar->height, in_v->codecpar->format,
             tb.num, tb.den, in_v->sample_aspect_ratio.num, in_v->sample_aspect_ratio.den);

    buffersrc = avfilter_get_by_name("buffer");
    buffersink = avfilter_get_by_name("buffersink");
    avfilter_graph_create_filter(&src_ctx, buffersrc, "in", args, nullptr, graph);
    avfilter_graph_create_filter(&sink_ctx, buffersink, "out", nullptr, nullptr, graph);

    outputs = avfilter_inout_alloc();
    inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in"); outputs->filter_ctx = src_ctx; outputs->pad_idx = 0; outputs->next = nullptr;
    inputs->name = av_strdup("out"); inputs->filter_ctx = sink_ctx; inputs->pad_idx = 0; inputs->next = nullptr;

    if ((ret = avfilter_graph_parse_ptr(graph, filter_descr, &inputs, &outputs, nullptr)) < 0) goto cleanup;
    if ((ret = avfilter_graph_config(graph, nullptr)) < 0) goto cleanup;

    frame = av_frame_alloc();
    while (av_read_frame(ifmt, &pkt) >= 0) {
        if (pkt.stream_index == vindex) {
            if (!decoder) {
                ret = AVERROR_DECODER_NOT_FOUND;
                goto cleanup;
            }
            dec_ctx = avcodec_alloc_context3(decoder);
            if ((ret = avcodec_parameters_to_context(dec_ctx, in_v->codecpar)) < 0) goto cleanup;
            if ((ret = avcodec_open2(dec_ctx, decoder, nullptr)) < 0) goto cleanup;

            while (av_read_frame(ifmt, &pkt) >= 0) {
                if (pkt.stream_index == vindex) {
                    if ((ret = avcodec_send_packet(dec_ctx, &pkt)) < 0) { log_fferr(ret); av_packet_unref(&pkt); continue; }

                    while ((ret = avcodec_receive_frame(dec_ctx, frame)) >= 0) {
                        frame->pts = frame->best_effort_timestamp;

                        // フィルタに送る
                        if ((ret = av_buffersrc_add_frame_flags(src_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF)) < 0) {
                            log_fferr(ret);
                            break;
                        }

                        // フィルタ出力から取得してパケットにする
                        while ((ret = av_buffersink_get_frame(sink_ctx, frame)) >= 0) {
                            // フレームをそのまま mux に書き出すなら再エンコードが必要だが、今はデモ的にフレーム表示
                            // 再エンコードは未実装 (後述)

                            av_frame_unref(frame);
                        }
                    }
                    av_packet_unref(&pkt);
                } else {
                    // オーディオなどはそのまま passthrough
                    pkt.stream_index = (pkt.stream_index == aindex && out_a) ? out_a->index : out_v->index;
                    av_interleaved_write_frame(ofmt, &pkt);
                    av_packet_unref(&pkt);
                }
            }
            avcodec_free_context(&dec_ctx);



            // 1. パケットをデコード
            ret = avcodec_send_packet(dec_ctx, &pkt);
            if (ret < 0) goto cleanup;

            while (av_read_frame(ifmt, &pkt) >= 0) {
                if (pkt.stream_index == vindex) {
                    // デコード
                    ret = avcodec_send_packet(dec_ctx, &pkt);
                    if (ret < 0) { av_packet_unref(&pkt); continue; }

                    while ((ret = avcodec_receive_frame(dec_ctx, frame)) >= 0) {
                        frame->pts = frame->best_effort_timestamp;

                        // フィルタに送る
                        if ((ret = av_buffersrc_add_frame_flags(src_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF)) < 0) {
                            av_frame_unref(frame);
                            break;
                        }

                        // フィルタから出力を受け取る
                        while ((ret = av_buffersink_get_frame(sink_ctx, frame)) >= 0) {
                            // エンコード
                            ret = avcodec_send_frame(enc_ctx, frame);
                            av_frame_unref(frame);
                            if (ret < 0) break;

                            AVPacket enc_pkt;
                            av_init_packet(&enc_pkt);
                            enc_pkt.data = nullptr;
                            enc_pkt.size = 0;

                            while ((ret = avcodec_receive_packet(enc_ctx, &enc_pkt)) >= 0) {
                                enc_pkt.stream_index = out_v->index;
                                av_packet_rescale_ts(&enc_pkt, enc_ctx->time_base, out_v->time_base);
                                av_interleaved_write_frame(ofmt, &enc_pkt);
                                av_packet_unref(&enc_pkt);
                            }
                        }
                    }
                    av_packet_unref(&pkt);
                } else {
                    // 音声などはそのまま
                    pkt.stream_index = (pkt.stream_index == aindex && out_a) ? out_a->index : out_v->index;
                    av_interleaved_write_frame(ofmt, &pkt);
                    av_packet_unref(&pkt);
                }
            }


        }
        pkt.stream_index = (pkt.stream_index == aindex && out_a) ? out_a->index : out_v->index;
        av_interleaved_write_frame(ofmt, &pkt);
        av_packet_unref(&pkt);
    }
    cleanup:
    if (inputs) avfilter_inout_free(&inputs);
    if (outputs) avfilter_inout_free(&outputs);
    if (graph) avfilter_graph_free(&graph);
    if (frame) av_frame_free(&frame);
    if (ofmt) {
        if (!(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
        avformat_free_context(ofmt);
    }
    if (ifmt) avformat_close_input(&ifmt);
    if (enc_ctx) avcodec_free_context(&enc_ctx);
    return ret;
}
static int process_multi_input_filter(const char* in1Path, const char* in2Path,
                                      const char* outPath, const char* filter_descr) {
    int ret = 0;

    AVFormatContext *ifmt1 = nullptr, *ifmt2 = nullptr, *ofmt = nullptr;
    AVCodecContext *dec_ctx1 = nullptr, *dec_ctx2 = nullptr, *enc_ctx = nullptr;
    AVFilterGraph *graph = nullptr;
    AVFilterContext *src_ctx1 = nullptr, *src_ctx2 = nullptr, *sink_ctx = nullptr;
    AVFilterInOut *inputs = nullptr, *outputs = nullptr;
    AVFrame *frame = nullptr;
    AVPacket pkt;
    av_init_packet(&pkt); pkt.data = nullptr; pkt.size = 0;

    // ------------------ 入力ファイル読み込み ------------------
    if ((ret = avformat_open_input(&ifmt1, in1Path, nullptr, nullptr)) < 0) return ret;
    if ((ret = avformat_find_stream_info(ifmt1, nullptr)) < 0) goto cleanup;

    if ((ret = avformat_open_input(&ifmt2, in2Path, nullptr, nullptr)) < 0) goto cleanup;
    if ((ret = avformat_find_stream_info(ifmt2, nullptr)) < 0) goto cleanup;

    // TODO: dec_ctx1, dec_ctx2 を作成して、それぞれの video stream を decode 可能にする
    // TODO: ofmt, enc_ctx を作成して出力ストリーム準備

    // ------------------ フィルタグラフ作成 ------------------
    graph = avfilter_graph_alloc();
    if (!graph) { ret = AVERROR(ENOMEM); goto cleanup; }

    // 例:
    // filter_descr = "[0:v]scale=720:-1[a];[1:v]scale=720:-1[b];[a][b]vstack=inputs=2[outv]"

    // 入力0 = ifmt1 → src_ctx1
    // 入力1 = ifmt2 → src_ctx2
    // 出力 = sink_ctx

    // TODO:
    // - avfilter_graph_create_filter() で 2つの buffer を接続
    // - parse_ptr → config

    // ------------------ ループでデコード→フィルタ→エンコード ------------------
    // 省略（cropFace の中盤とほぼ同じ）

    cleanup:
    if (ifmt1) avformat_close_input(&ifmt1);
    if (ifmt2) avformat_close_input(&ifmt2);
    if (ofmt) {
        if (!(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
        avformat_free_context(ofmt);
    }
    if (graph) avfilter_graph_free(&graph);
    if (frame) av_frame_free(&frame);
    return ret;
}


extern "C"
JNIEXPORT jstring JNICALL
Java_com_example_try_1ffmpeg_MainActivity_stringFromJNI(JNIEnv* env, jobject /* thiz */) {
    const char* config = avcodec_configuration();   // ← FFmpeg 情報を取得
    return env->NewStringUTF(config);               // UI に返す
}
extern "C"
JNIEXPORT jint JNICALL
Java_com_example_try_1ffmpeg_FFmpegBridge_trimVideo(
        JNIEnv* env, jclass /*clazz*/,
        jstring inputPath_, jstring outputPath_,
        jdouble startTime, jdouble duration) {

    init_ffmpeg_once();
    const char* inputPath  = env->GetStringUTFChars(inputPath_,  nullptr);
    const char* outputPath = env->GetStringUTFChars(outputPath_, nullptr);

    int ret = 0;
    AVFormatContext* ifmt_ctx = nullptr;
    AVFormatContext* ofmt_ctx = nullptr;
    AVPacket pkt;  av_init_packet(&pkt); pkt.data = nullptr; pkt.size = 0;

    const int64_t start_pts_global = static_cast<int64_t>(startTime * AV_TIME_BASE);
    const int64_t end_pts_global   = static_cast<int64_t>((startTime + duration) * AV_TIME_BASE);

    // 1) open input ----------------------------------------------------------
    if ((ret = avformat_open_input(&ifmt_ctx, inputPath, nullptr, nullptr)) < 0) goto end;
    if ((ret = avformat_find_stream_info(ifmt_ctx, nullptr)) < 0)             goto end;

    // 2) open output ---------------------------------------------------------
    if ((ret = avformat_alloc_output_context2(&ofmt_ctx, nullptr, nullptr, outputPath)) < 0 || !ofmt_ctx) goto end;

    for (unsigned i = 0; i < ifmt_ctx->nb_streams; ++i) {
        AVStream* in_st  = ifmt_ctx->streams[i];
        AVStream* out_st = avformat_new_stream(ofmt_ctx, nullptr);
        if (!out_st) { ret = AVERROR_UNKNOWN; goto end; }
        if ((ret = avcodec_parameters_copy(out_st->codecpar, in_st->codecpar)) < 0) goto end;
        out_st->codecpar->codec_tag = 0;
        out_st->time_base = in_st->time_base;
    }
    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&ofmt_ctx->pb, outputPath, AVIO_FLAG_WRITE)) < 0) goto end;
    }
    if ((ret = avformat_write_header(ofmt_ctx, nullptr)) < 0) goto end;

    // 3) seek to start -------------------------------------------------------
    if ((ret = av_seek_frame(ifmt_ctx, -1, start_pts_global, AVSEEK_FLAG_BACKWARD)) < 0) goto end;

    // 4) copy packets within range ------------------------------------------
    while (av_read_frame(ifmt_ctx, &pkt) >= 0) {
        AVStream* in_st  = ifmt_ctx->streams[pkt.stream_index];
        AVStream* out_st = ofmt_ctx->streams[pkt.stream_index];

        int64_t pkt_time = av_rescale_q(pkt.pts, in_st->time_base, AVRational{1, AV_TIME_BASE});
        if (pkt_time < start_pts_global) { av_packet_unref(&pkt); continue; }
        if (pkt_time > end_pts_global)   { av_packet_unref(&pkt); break; }

        // 0 始まりに詰め直し
        pkt.pts      = av_rescale_q_rnd(pkt.pts - in_st->start_time - (start_pts_global / AV_TIME_BASE) * in_st->time_base.den / in_st->time_base.num,
                                        in_st->time_base, out_st->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        pkt.dts      = av_rescale_q_rnd(pkt.dts - in_st->start_time - (start_pts_global / AV_TIME_BASE) * in_st->time_base.den / in_st->time_base.num,
                                        in_st->time_base, out_st->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        pkt.duration = av_rescale_q(pkt.duration, in_st->time_base, out_st->time_base);
        pkt.pos      = -1;

        if ((ret = av_interleaved_write_frame(ofmt_ctx, &pkt)) < 0) { av_packet_unref(&pkt); goto end; }
        av_packet_unref(&pkt);
    }
    av_write_trailer(ofmt_ctx);

    end:
    if (ofmt_ctx) {
        if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt_ctx->pb);
        avformat_free_context(ofmt_ctx);
    }
    if (ifmt_ctx) avformat_close_input(&ifmt_ctx);
    av_packet_unref(&pkt);

    release_utf(env, inputPath_,  inputPath);
    release_utf(env, outputPath_, outputPath);
    return ret; // 0 == success (FFmpeg の慣習に合わせて負値がエラー)
}
// ---- ② cropFace : 動画A → 顔アップの動画B ------------------------------
//    パラメータは検出済みバウンディングボックス (x,y,w,h)
extern "C"
JNIEXPORT jint JNICALL
Java_com_example_try_1ffmpeg_FFmpegBridge_cropFace(JNIEnv* env, jclass, jstring inPath_, jstring outPath_, jint x, jint y, jint w, jint h){
    init_ffmpeg_once();
    const char* inPath=env->GetStringUTFChars(inPath_,nullptr);
    const char* outPath=env->GetStringUTFChars(outPath_,nullptr);
    char filter[64]; snprintf(filter,sizeof(filter),"crop=%d:%d:%d:%d",w,h,x,y);
    int ret=process_with_filter(inPath,outPath,filter);
    release_utf(env,inPath_,inPath); release_utf(env,outPath_,outPath);
    return ret;
}

// ---- ③ burnSubtitles : 動画A + .srt → 字幕焼き込み動画C ---------------
extern "C"
JNIEXPORT jint JNICALL
Java_com_example_try_1ffmpeg_FFmpegBridge_burnSubtitles(
        JNIEnv* env, jclass /*clazz*/,
        jstring inPath_, jstring subsPath_, jstring outPath_) {

    init_ffmpeg_once();
    const char* inPath   = env->GetStringUTFChars(inPath_,   nullptr);
    const char* subsPath = env->GetStringUTFChars(subsPath_, nullptr);
    const char* outPath  = env->GetStringUTFChars(outPath_,  nullptr);

    char filter_descr[256];
    snprintf(filter_descr, sizeof(filter_descr), "subtitles=%s", subsPath);

    // TODO: avfilter_graph_simple("[in]subtitles=... [out]") の実装

    release_utf(env, inPath_,   inPath);
    release_utf(env, subsPath_, subsPath);
    release_utf(env, outPath_,  outPath);
    return 0; // stub OK
}

// ---- ④ composeShort : A と B を縦に並べ字幕Cを焼いて 9:16 動画出力 ----
extern "C"
JNIEXPORT jint JNICALL
Java_com_example_try_1ffmpeg_FFmpegBridge_composeShort(
        JNIEnv* env, jclass,
        jstring videoA_, jstring videoB_, jstring subsPath_, jstring outPath_,
        jint outW, jint outH) {

    init_ffmpeg_once();
    const char* videoA   = env->GetStringUTFChars(videoA_, nullptr);
    const char* videoB   = env->GetStringUTFChars(videoB_, nullptr);
    const char* subsPath = env->GetStringUTFChars(subsPath_, nullptr); // ← 使わない場合は "" に
    const char* outPath  = env->GetStringUTFChars(outPath_, nullptr);

    // フィルタ構築（縦に並べる）
    char filter[512];
    if (subsPath && strlen(subsPath) > 0) {
        snprintf(filter, sizeof(filter),
                 "[0:v]scale=%d:-1[a];[1:v]scale=%d:-1[b];[a][b]vstack=inputs=2[top];"
                 "[top]subtitles=%s[outv]",
                 outW, outW, subsPath
        );
    } else {
        snprintf(filter, sizeof(filter),
                 "[0:v]scale=%d:-1[a];[1:v]scale=%d:-1[b];[a][b]vstack=inputs=2[outv]",
                 outW, outW
        );
    }

    // TODO: process_multiple_inputs_with_filter(2入力, 1出力, filter) という関数を作るとキレイ
    //       または既存の process_with_filter() を拡張

    release_utf(env, videoA_, videoA);
    release_utf(env, videoB_, videoB);
    release_utf(env, subsPath_, subsPath);
    release_utf(env, outPath_, outPath);

    return 0; // 成功時は 0、失敗時はエラーコード
}

