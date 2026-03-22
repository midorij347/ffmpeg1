package com.example.try_ffmpeg

import android.content.ContentValues
import android.media.MediaMetadataRetriever
import android.widget.MediaController
import android.net.Uri
import android.os.Bundle
import android.provider.MediaStore
import android.view.View
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import android.widget.VideoView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.material.slider.RangeSlider
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

class CreateActivity : AppCompatActivity() {

    private lateinit var selectButton: Button
    private lateinit var previewPlaceholder: FrameLayout
    private lateinit var videoPreview: VideoView
    private lateinit var startTimeText: TextView
    private lateinit var endTimeText: TextView
    private lateinit var clipDurationText: TextView
    private lateinit var trimSlider: RangeSlider
    private lateinit var trimButton: Button
    private lateinit var progressBar: ProgressBar
    private lateinit var progressText: TextView

    private var selectedUri: Uri? = null
    private var videoDurationSec: Float = 0f

    private val videoPickerLauncher =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
            uri?.let { handleVideoSelection(it) }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_create)

        selectButton       = findViewById(R.id.selectButton)
        previewPlaceholder = findViewById(R.id.previewPlaceholder)
        videoPreview       = findViewById(R.id.videoPreview)
        startTimeText      = findViewById(R.id.startTimeText)
        endTimeText        = findViewById(R.id.endTimeText)
        clipDurationText   = findViewById(R.id.clipDurationText)
        trimSlider         = findViewById(R.id.trimSlider)
        trimButton         = findViewById(R.id.trimButton)
        progressBar        = findViewById(R.id.progressBar)
        progressText       = findViewById(R.id.progressText)

        // 初期スライダー値（動画選択前のプレースホルダー）
        trimSlider.valueFrom = 0f
        trimSlider.valueTo   = 100f
        trimSlider.values    = listOf(0f, 100f)

        selectButton.setOnClickListener {
            videoPickerLauncher.launch(arrayOf("video/*"))
        }

        trimSlider.addOnChangeListener { slider, _, _ ->
            val start = slider.values[0]
            val end   = slider.values[1]
            startTimeText.text    = formatTime(start)
            endTimeText.text      = formatTime(end)
            clipDurationText.text = "クリップの長さ: ${formatTime(end - start)}"
        }

        trimButton.setOnClickListener {
            executeTrim()
        }
    }

    private fun handleVideoSelection(uri: Uri) {
        selectedUri = uri

        // 動画の長さを取得
        val retriever = MediaMetadataRetriever()
        retriever.setDataSource(this, uri)
        val durationMs = retriever.extractMetadata(
            MediaMetadataRetriever.METADATA_KEY_DURATION
        )?.toLongOrNull() ?: 0L
        retriever.release()

        videoDurationSec = durationMs / 1000f
        val durationRounded = kotlin.math.ceil(videoDurationSec.toDouble()).toFloat()

        // スライダーを動画の長さに合わせる（1秒刻み、valueTo は切り上げ整数）
        trimSlider.valueFrom = 0f
        trimSlider.valueTo   = durationRounded
        trimSlider.stepSize  = 1f
        trimSlider.values    = listOf(0f, durationRounded)
        trimSlider.isEnabled = true

        startTimeText.text    = formatTime(0f)
        endTimeText.text      = formatTime(videoDurationSec)
        clipDurationText.text = "クリップの長さ: ${formatTime(videoDurationSec)}"

        // プレビュー表示
        previewPlaceholder.visibility = View.GONE
        videoPreview.visibility       = View.VISIBLE
        videoPreview.setVideoURI(uri)
        val mc = MediaController(this)
        mc.setAnchorView(videoPreview)
        videoPreview.setMediaController(mc)
        videoPreview.setOnPreparedListener { mp ->
            mp.isLooping = true
            mp.start()
        }

        trimButton.isEnabled = true
    }

    private fun executeTrim() {
        val uri = selectedUri ?: return
        val startSec    = trimSlider.values[0].toDouble()
        val endSec      = trimSlider.values[1].toDouble()
        val durationSec = endSec - startSec

        if (durationSec <= 0) {
            Toast.makeText(this, "終了時間を開始時間より後に設定してください", Toast.LENGTH_SHORT).show()
            return
        }

        // UI をロック
        setUiEnabled(false)
        progressBar.visibility = View.VISIBLE
        progressText.visibility = View.VISIBLE
        progressText.text = "ファイルをコピー中..."

        val inputFile  = File(getExternalFilesDir(null), "input_from_saf.mp4")
        val outputFile = File(getExternalFilesDir(null), "trimmed_from_saf.mp4")
        outputFile.delete()

        lifecycleScope.launch(Dispatchers.IO) {
            // SAF URI → ファイルにコピー
            contentResolver.openInputStream(uri)?.use { input ->
                inputFile.outputStream().use { output -> input.copyTo(output) }
            }

            withContext(Dispatchers.Main) { progressText.text = "トリミング中..." }

            val ret = FFmpegBridge.trimVideo(
                inputFile.absolutePath,
                outputFile.absolutePath,
                startSec,
                durationSec
            )

            withContext(Dispatchers.Main) {
                progressBar.visibility  = View.GONE
                progressText.visibility = View.GONE
                setUiEnabled(true)

                if (ret == 0 && outputFile.exists()) {
                    saveToGallery(outputFile)
                } else {
                    Toast.makeText(
                        this@CreateActivity,
                        "失敗しました (ret=$ret)",
                        Toast.LENGTH_SHORT
                    ).show()
                }
            }
        }
    }

    private fun saveToGallery(file: File) {
        val values = ContentValues().apply {
            put(MediaStore.Video.Media.DISPLAY_NAME, "cutclip_${System.currentTimeMillis()}.mp4")
            put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
            put(MediaStore.Video.Media.RELATIVE_PATH, "Movies/CutClip")
        }
        val uri = contentResolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values)
        if (uri != null) {
            contentResolver.openOutputStream(uri)?.use { out ->
                file.inputStream().use { it.copyTo(out) }
            }
            Toast.makeText(this, "ギャラリーに保存しました", Toast.LENGTH_SHORT).show()
        } else {
            Toast.makeText(this, "ギャラリーへの保存に失敗しました", Toast.LENGTH_SHORT).show()
        }
    }

    private fun setUiEnabled(enabled: Boolean) {
        selectButton.isEnabled = enabled
        trimButton.isEnabled   = enabled
        trimSlider.isEnabled   = enabled
    }

    private fun formatTime(seconds: Float): String {
        val s = seconds.toInt()
        return "%02d:%02d".format(s / 60, s % 60)
    }
}
