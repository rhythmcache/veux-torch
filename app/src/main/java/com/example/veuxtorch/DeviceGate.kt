package com.example.veuxtorch

import android.os.Build
import com.topjohnwu.superuser.Shell

object DeviceGate {

    // Single source of truth for supported hardware identifiers
    val ALLOWED_DEVICES = setOf("veux", "peux")

    fun isDeviceSupported(): Boolean {
        val currentDevice = Build.DEVICE.lowercase()
        return ALLOWED_DEVICES.contains(currentDevice)
    }

    fun isRootAvailable(): Boolean {
        return try {
            Shell.getShell().isRoot
        } catch (_: Exception) {
            false
        }
    }
}
