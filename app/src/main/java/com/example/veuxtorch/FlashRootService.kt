package com.example.veuxtorch

import android.content.Intent
import android.os.IBinder
import com.topjohnwu.superuser.ipc.RootService
import java.util.concurrent.Callable
import java.util.concurrent.Executors

class FlashRootService : RootService() {

    // Single-threaded executor ensuring all native hardware calls are strictly serialized
    private val worker = Executors.newSingleThreadExecutor()

    private val binder = object : IFlashService.Stub() {
        override fun turnOn(level: Int): Int {
            return worker.submit(Callable {
                FlashNative.nativeOn(level)
            }).get()
        }

        override fun setLevel(level: Int): Int {
            return worker.submit(Callable {
                FlashNative.nativeSetLevel(level)
            }).get()
        }

        override fun turnOff(): Int {
            return worker.submit(Callable {
                FlashNative.nativeOff()
            }).get()
        }

        override fun heartbeat(): Int {
            return worker.submit(Callable {
                FlashNative.nativeHeartbeat()
            }).get()
        }

        override fun getMaxLevel(): Int {
            return worker.submit(Callable {
                FlashNative.nativeMaxLevel()
            }).get()
        }

        override fun getStatus(): IntArray {
            return worker.submit(Callable {
                FlashNative.nativeStatus()
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
        // Ensure torch is powered off when client unbinds
        worker.submit {
            FlashNative.nativeOff()
        }
        return false
    }

    override fun onDestroy() {
        worker.submit {
            FlashNative.nativeOff()
        }
        worker.shutdown()
        super.onDestroy()
    }
}
