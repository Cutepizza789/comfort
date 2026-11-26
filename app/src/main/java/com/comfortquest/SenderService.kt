package com.comfortquest

import android.app.*
import android.content.Intent
import android.os.IBinder
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.util.Locale

/** Streams the current settings to patched games over loopback UDP. */
class SenderService : Service() {
    @Volatile private var running = false
    override fun onBind(i: Intent?): IBinder? = null

    override fun onStartCommand(i: Intent?, flags: Int, id: Int): Int {
        if (!running) {
            running = true
            val nm = getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(NotificationChannel("c", "Comfort Quest", NotificationManager.IMPORTANCE_LOW))
            startForeground(1, Notification.Builder(this, "c").setContentTitle("Comfort Quest active")
                .setSmallIcon(android.R.drawable.ic_menu_compass).build())
            Thread {
                val sock = DatagramSocket()
                val addr = InetAddress.getByName("127.0.0.1")
                val p = getSharedPreferences("cfg", MODE_PRIVATE)
                while (running) {
                    val m = String.format(Locale.US, "C1 %d %.2f %d %.2f %d %.2f %d %d",
                        if (p.getBoolean("armOn", true)) 1 else 0, p.getFloat("armScale", 1.3f),
                        if (p.getBoolean("walkOn", true)) 1 else 0, p.getFloat("walkSpeed", 1.4f),
                        if (p.getBoolean("jumpOn", true)) 1 else 0, p.getFloat("jumpH", 0.35f),
                        p.getInt("jumpBtn", 4), p.getInt("reset", 0))
                    val d = m.toByteArray()
                    try { sock.send(DatagramPacket(d, d.size, addr, 47321)) } catch (e: Exception) {}
                    Thread.sleep(250)
                }
            }.start()
        }
        return START_STICKY
    }

    override fun onDestroy() { running = false }
}
