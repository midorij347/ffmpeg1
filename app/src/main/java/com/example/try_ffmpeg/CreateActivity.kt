package com.example.try_ffmpeg

import android.net.Uri
import android.os.Bundle
import android.os.PersistableBundle
import android.util.Log
import android.widget.Button
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

class CreateActivity:AppCompatActivity() {
    // SAF: ユーザーが動画を選択する
    private val videoPickerLauncher = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        uri?.let {
            handleVideoSelection(it)
        } ?: Log.e("FFmpegTest", "❌ 動画が選ばれませんでした")
    }

    override fun onCreate(savedInstanceState: Bundle?, persistentState: PersistableBundle?) {
        super.onCreate(savedInstanceState, persistentState)
        setContentView(R.layout.activity_create)
        // ボタンで動画選択を起動
        findViewById<Button>(R.id.selectButton).setOnClickListener {
            videoPickerLauncher.launch(arrayOf("video/*"))
        }
    }
    // SAF で選択された動画を一時ファイルに保存してトリミング
    private fun handleVideoSelection(uri: Uri) {
        val inputFile = File(getExternalFilesDir(null), "input_from_saf.mp4")
        val outputFile = File(getExternalFilesDir(null), "trimmed_from_saf.mp4")
        outputFile.delete()

        // SAF URI → File にコピー
        contentResolver.openInputStream(uri)?.use { input ->
            inputFile.outputStream().use { output ->
                input.copyTo(output)
            }
        }

        // 非同期でFFmpegトリミング実行
        lifecycleScope.launch(Dispatchers.IO) {
            val ret = FFmpegBridge.trimVideo(
                inputFile.absolutePath,
                outputFile.absolutePath,
                startSec = 5.0,
                durationSec = 10.0
            )

            withContext(Dispatchers.Main) {
                if (ret == 0 && outputFile.exists()) {
                    Toast.makeText(this@CreateActivity, "✅ トリミング成功!", Toast.LENGTH_SHORT).show()
                } else {
                    Toast.makeText(this@CreateActivity, "❌ 失敗 ret=$ret", Toast.LENGTH_SHORT).show()
                }
            }
        }
    }
}