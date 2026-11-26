package com.comfortquest

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.*

class MainActivity : Activity() {
    private val prefs by lazy { getSharedPreferences("cfg", MODE_PRIVATE) }

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(48, 32, 48, 32) }

        fun header(t: String) = root.addView(TextView(this).apply { text = t; textSize = 22f; setPadding(0, 36, 0, 8) })

        fun section(title: String, onKey: String, valKey: String, min: Float, max: Float, def: Float, unit: String) {
            header(title)
            val sw = Switch(this).apply { text = "Enabled"; isChecked = prefs.getBoolean(onKey, true) }
            val label = TextView(this)
            val bar = SeekBar(this).apply { this.max = 100 }
            fun show(v: Float) { label.text = String.format("%.2f %s", v, unit) }
            val cur = prefs.getFloat(valKey, def)
            bar.progress = (((cur - min) / (max - min)) * 100).toInt(); show(cur)
            sw.setOnCheckedChangeListener { _, c -> prefs.edit().putBoolean(onKey, c).apply() }
            bar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(s: SeekBar?, p: Int, u: Boolean) {
                    val v = min + (max - min) * p / 100f
                    show(v); prefs.edit().putFloat(valKey, v).apply()
                }
                override fun onStartTrackingTouch(s: SeekBar?) {}
                override fun onStopTrackingTouch(s: SeekBar?) {}
            })
            root.addView(sw); root.addView(label); root.addView(bar)
        }

        section("Arm reach", "armOn", "armScale", 0.8f, 2.5f, 1.3f, "x arm length")
        section("Joystick walking (left stick)", "walkOn", "walkSpeed", 0.5f, 3.0f, 1.4f, "m/s")
        section("Jump", "jumpOn", "jumpH", 0.1f, 0.8f, 0.35f, "m high")

        val names = arrayOf("A", "B", "X", "Y", "Right stick click", "Left stick click")
        root.addView(TextView(this).apply { text = "Jump button" })
        root.addView(Spinner(this).apply {
            adapter = ArrayAdapter(this@MainActivity, android.R.layout.simple_spinner_dropdown_item, names)
            setSelection(prefs.getInt("jumpBtn", 4))
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(p: AdapterView<*>?, v: View?, pos: Int, id: Long) {
                    prefs.edit().putInt("jumpBtn", pos).apply()
                }
                override fun onNothingSelected(p: AdapterView<*>?) {}
            }
        })
        root.addView(Button(this).apply {
            text = "Reset walk position to start"
            setOnClickListener { prefs.edit().putInt("reset", prefs.getInt("reset", 0) + 1).apply() }
        })
        root.addView(TextView(this).apply {
            text = "\nSettings apply live to games patched with ComfortLayer. Keep this app running (see notification)."
        })
        setContentView(ScrollView(this).apply { addView(root) })
        startForegroundService(Intent(this, SenderService::class.java))
    }
}
