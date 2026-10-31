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
    private val stoppedServices = mutableSetOf<String>()

    /**
     * Dynamically finds any active camera provider init service on this ROM.
     * Prevents hardcoding issues across MIUI/HyperOS, AOSP, LineageOS, or different Android versions
     * (e.g. vendor.camera-provider-2-4, vendor.camera-provider-2-5, camera-provider-default).
     */
    private fun discoverCameraProviderServices(): List<String> {
        val out = Shell.cmd("getprop").exec().out
        val services = mutableListOf<String>()
        val regex = Regex("""\[init\.svc\.(.*camera.*provider.*)\]:\s*\[running\]""")
        for (line in out) {
            val match = regex.find(line)
            if (match != null) {
                val serviceName = match.groupValues[1]
                services.add(serviceName)
            }
        }
        // Fallback default if getprop pattern yielded nothing
        if (services.isEmpty()) {
            services.add("vendor.camera-provider-2-4")
        }
        return services
    }

    private fun ensureCameraProviderStopped() {
        val detected = discoverCameraProviderServices()
        for (svc in detected) {
            Shell.cmd("stop $svc").exec()
            stoppedServices.add(svc)
        }

        // Wait up to 350ms for camera provider process to exit and release /dev/video0 fd
        for (i in 0 until 7) {
            Thread.sleep(50)
            val lsofCheck = Shell.cmd("lsof /dev/video0").exec()
            if (lsofCheck.out.isEmpty()) {
                break
            }
        }
    }

    private fun restoreCameraProvider() {
        for (svc in stoppedServices) {
            Shell.cmd("start $svc").exec()
        }
        stoppedServices.clear()
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
                if (status.isNotEmpty() && status[0] == 0 && stoppedServices.isNotEmpty()) {
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
