package com.example.veuxtorch

import android.content.Intent
import android.os.IBinder
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ipc.RootService
import java.util.concurrent.Callable
import java.util.concurrent.Executors

class FlashRootService : RootService() {

    // Single-threaded executor ensuring all native hardware calls are strictly serialized
    private val worker = Executors.newSingleThreadExecutor()
    private var stoppedCameraProvider = false

    private fun ensureCameraProviderStopped() {
        if (!stoppedCameraProvider) {
            // Stop background camera HAL so it releases its 24/7 exclusive lock on /dev/video0
            Shell.cmd("stop vendor.camera-provider-2-4").exec()
            stoppedCameraProvider = true
            // Allow up to 300ms for camera provider process to exit and release fd
            for (i in 0 until 6) {
                Thread.sleep(50)
                val fuserCheck = Shell.cmd("fuser /dev/video0").exec()
                if (fuserCheck.out.isEmpty()) {
                    break
                }
            }
        }
    }

    private fun restoreCameraProvider() {
        if (stoppedCameraProvider) {
            Shell.cmd("start vendor.camera-provider-2-4").exec()
            stoppedCameraProvider = false
        }
    }

    private val binder = object : IFlashService.Stub() {
        override fun turnOn(level: Int): Int {
            return worker.submit(Callable {
                ensureCameraProviderStopped()
                val rc = FlashNative.nativeOn(level)
                if (rc != 0) {
                    restoreCameraProvider()
                }
                rc
            }).get()
        }

        override fun setLevel(level: Int): Int {
            return worker.submit(Callable {
                FlashNative.nativeSetLevel(level)
            }).get()
        }

        override fun turnOff(): Int {
            return worker.submit(Callable {
                val rc = FlashNative.nativeOff()
                restoreCameraProvider()
                rc
            }).get()
        }

        override fun heartbeat(): Int {
            return worker.submit(Callable {
                val rc = FlashNative.nativeHeartbeat()
                // If watchdog or library turned off torch, restore provider
                val status = FlashNative.nativeStatus()
                if (status.isNotEmpty() && status[0] == 0) {
                    restoreCameraProvider()
                }
                rc
            }).get()
        }

        override fun getMaxLevel(): Int {
            return worker.submit(Callable {
                FlashNative.nativeMaxLevel()
            }).get()
        }

        override fun getStatus(): IntArray {
            return worker.submit(Callable {
                val status = FlashNative.nativeStatus()
                if (status.isNotEmpty() && status[0] == 0 && stoppedCameraProvider) {
                    restoreCameraProvider()
                }
                status
            }).get()
        }

        override fun getErrorMessage(err: Int): String {
            return worker.submit(Callable {
                FlashNative.nativeStrerror(err)
            }).get()
        }
    }

    override fun onBind(intent: Intent): IBinder {
        return binder
    }

    override fun onUnbind(intent: Intent): Boolean {
        // Ensure torch is powered off and camera provider restored when client unbinds
        worker.submit {
            FlashNative.nativeOff()
            restoreCameraProvider()
        }
        return false
    }

    override fun onDestroy() {
        worker.submit {
            FlashNative.nativeOff()
            restoreCameraProvider()
        }
        worker.shutdown()
        super.onDestroy()
    }
}
