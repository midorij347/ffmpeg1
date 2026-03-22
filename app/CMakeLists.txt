cmake_minimum_required(VERSION 3.18.1)
project(ffmpeg_native)

set(CMAKE_CXX_STANDARD 17)

# FFmpeg ヘッダーファイルのパス（必要であれば）
include_directories(
        ${CMAKE_SOURCE_DIR}/src/main/cpp/include
        /home/midorinobuta/ffmpeg/android-build/arm64/include   # 例
)

# native-lib を定義
add_library(native-lib
        SHARED
        src/main/cpp/NativeLib.cpp
)

# FFmpeg ライブラリを IMPORTED で定義（.soの場所を指定）
add_library(avcodec SHARED IMPORTED)
set_target_properties(avcodec PROPERTIES IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/src/main/jniLibs/arm64-v8a/libavcodec.so)

add_library(avformat SHARED IMPORTED)
set_target_properties(avformat PROPERTIES IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/src/main/jniLibs/arm64-v8a/libavformat.so)

add_library(avfilter SHARED IMPORTED)
set_target_properties(avfilter PROPERTIES IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/src/main/jniLibs/arm64-v8a/libavfilter.so)

add_library(avutil SHARED IMPORTED)
set_target_properties(avutil PROPERTIES IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/src/main/jniLibs/arm64-v8a/libavutil.so)

add_library(swresample SHARED IMPORTED)
set_target_properties(swresample PROPERTIES IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/src/main/jniLibs/arm64-v8a/libswresample.so)

add_library(swscale SHARED IMPORTED)
set_target_properties(swscale PROPERTIES IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/src/main/jniLibs/arm64-v8a/libswscale.so)

# Androidログライブラリ
find_library(log-lib log)

# FFmpeg ライブラリ + ログをリンク
target_link_libraries(native-lib
        avutil
        swresample
        avcodec
        avformat
        avfilter
        swscale
        ${log-lib}
)
