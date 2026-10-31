package com.example.veuxtorch

data class FlashStatus(
    val isOn: Boolean,
    val currentLevel: Int,
    val secondsOn: Int,
    val cooldownRemaining: Int
)

object FlashNative {
    init {
        System.loadLibrary("flashjni")
    }

    @JvmStatic
    external fun nativeOn(level: Int): Int

    @JvmStatic
    external fun nativeSetLevel(level: Int): Int

    @JvmStatic
    external fun nativeOff(): Int

    @JvmStatic
    external fun nativeHeartbeat(): Int

    @JvmStatic
    external fun nativeStatus(): IntArray

    @JvmStatic
    external fun nativeMaxLevel(): Int

    @JvmStatic
    external fun nativeStrerror(err: Int): String

    fun getStatus(): FlashStatus {
        val raw = nativeStatus()
        return if (raw.size >= 4) {
            FlashStatus(
                isOn = raw[0] != 0,
                currentLevel = raw[1],
                secondsOn = raw[2],
                cooldownRemaining = raw[3]
            )
        } else {
            FlashStatus(isOn = false, currentLevel = 0, secondsOn = 0, cooldownRemaining = 0)
        }
    }
}
