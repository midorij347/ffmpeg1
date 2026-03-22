package com.example.try_ffmpeg

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.util.Log
import android.widget.Button
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.material.bottomnavigation.BottomNavigationView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

class MainActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        val bottomNavigation = findViewById<BottomNavigationView>(R.id.bottom_navigation)
        bottomNavigation.setOnNavigationItemSelectedListener { item ->
            when (item.itemId) {
                R.id.watch_view_history -> {
                    val intent = Intent(this, ViewHistoryActivity::class.java)
                    startActivity(intent)
                    true
                }
                R.id.create2 -> {
                    val intent = Intent(this,CreateActivity::class.java)
                    startActivity(intent)
                    true
                }
                R.id.your_library -> {
                    val intent= Intent(this,LibraryActivity::class.java)
                    startActivity(intent)
                    true
                }
                else -> false
            }
        }

    }
}
