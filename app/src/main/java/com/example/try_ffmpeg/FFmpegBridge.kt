package com.example.try_ffmpeg

object FFmpegBridge {
    init {
        System.loadLibrary("avutil")
        System.loadLibrary("swresample")
        System.loadLibrary("swscale")
        System.loadLibrary("avcodec")
        System.loadLibrary("avformat")
        System.loadLibrary("avfilter")
        System.loadLibrary("native-lib")
    }
    @JvmStatic
    external fun trimVideo(
        inputPath: String,
        outputPath: String,
        startSec: Double,
        durationSec: Double
    ): Int
    @JvmStatic  // ← 明示しておくと安心
    external fun cropFace(inputPath: String, outputPath: String,
                          x: Int, y: Int, w: Int, h: Int): Int
    /* burnSubtitles / composeShort も同様 */
    @JvmStatic
    external fun burnSubtitles(inPath: String, subsPath: String, outPath: String): Int
    @JvmStatic
    external fun composeShort(aPath: String, bPath: String, subs: String, outPath: String, w: Int = 1080, h: Int = 1920): Int
}
