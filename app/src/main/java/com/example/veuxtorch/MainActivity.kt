package com.example.veuxtorch

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.runtime.*
import androidx.lifecycle.lifecycleScope
import com.example.veuxtorch.ui.BlockedScreen
import com.example.veuxtorch.ui.TorchScreen
import com.example.veuxtorch.ui.theme.VeuxTorchTheme
import com.topjohnwu.superuser.ipc.RootService
import kotlinx.coroutines.*

class MainActivity : ComponentActivity() {

    private var flashService: IFlashService? = null
    private var serviceConnected by mutableStateOf(false)
    private var maxLevel by mutableStateOf(44)
    private var status by mutableStateOf(FlashStatus(false, 0, 0, 0))
    private var errorMessage by mutableStateOf<String?>(null)

    private var heartbeatJob: Job? = null
    private var pollJob: Job? = null

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            flashService = IFlashService.Stub.asInterface(binder)
            serviceConnected = true
            lifecycleScope.launch(Dispatchers.IO) {
                try {
                    val max = flashService?.maxLevel ?: 44
                    withContext(Dispatchers.Main) { maxLevel = max }
                } catch (_: Exception) {}
            }
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            flashService = null
            serviceConnected = false
            status = FlashStatus(false, 0, 0, 0)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        setContent {
            VeuxTorchTheme {
                // 1. Device Gate Check
                if (!DeviceGate.isDeviceSupported()) {
                    BlockedScreen(
                        reasonTitle = "Device Unsupported",
                        reasonDetails = "This application strictly requires Xiaomi Redmi Note 11 Pro 5G / POCO X4 Pro 5G (veux / peux).\n" +
                                "Detected device model: '${Build.DEVICE}' (Allowed: ${DeviceGate.ALLOWED_DEVICES.joinToString()}).\n" +
                                "Execution is blocked to protect incompatible hardware."
                    )
                    return@VeuxTorchTheme
                }

                // 2. Root Access Gate Check
                var rootChecked by remember { mutableStateOf(false) }
                var hasRoot by remember { mutableStateOf(false) }

                LaunchedEffect(Unit) {
                    val isRoot = withContext(Dispatchers.IO) {
                        DeviceGate.isRootAvailable()
                    }
                    hasRoot = isRoot
                    rootChecked = true
                    if (isRoot) {
                        bindRootService()
                    }
                }

                if (!rootChecked) {
                    BlockedScreen(
                        reasonTitle = "Checking Root Access",
                        reasonDetails = "Requesting root permission via libsu..."
                    )
                    return@VeuxTorchTheme
                }

                if (!hasRoot) {
                    BlockedScreen(
                        reasonTitle = "Root Access Denied",
                        reasonDetails = "This application requires root permissions to access Qualcomm CCI flash nodes (/dev/v4l-subdev15 and /dev/video0).\n" +
                                "Please grant root privileges via Magisk or KernelSU."
                    )
                    return@VeuxTorchTheme
                }

                // Main Torch Control UI
                TorchScreen(
                    status = status,
                    maxLevel = maxLevel,
                    errorMessage = errorMessage,
                    onToggle = { turnOn, level ->
                        handleToggle(turnOn, level)
                    },
                    onLevelChange = { newLevel ->
                        handleLevelChange(newLevel)
                    }
                )
            }
        }
    }

    private fun bindRootService() {
        val intent = Intent(this, FlashRootService::class.java)
        RootService.bind(intent, connection)
    }

    private fun handleToggle(turnOn: Boolean, level: Int) {
        lifecycleScope.launch(Dispatchers.IO) {
            val service = flashService
            if (service == null) {
                withContext(Dispatchers.Main) {
                    errorMessage = "Root service not yet connected"
                }
                return@launch
            }

            if (turnOn) {
                val rc = service.turnOn(level)
                withContext(Dispatchers.Main) {
                    if (rc == 0) {
                        errorMessage = null
                        startHeartbeatAndPoll()
                    } else {
                        errorMessage = parseErrorMessage(rc)
                        status = status.copy(isOn = false)
                    }
                }
            } else {
                service.turnOff()
                withContext(Dispatchers.Main) {
                    stopHeartbeat()
                    status = status.copy(isOn = false, secondsOn = 0)
                }
            }
        }
    }

    private fun handleLevelChange(level: Int) {
        lifecycleScope.launch(Dispatchers.IO) {
            val service = flashService ?: return@launch
            val rc = service.setLevel(level)
            if (rc != 0) {
                withContext(Dispatchers.Main) {
                    errorMessage = parseErrorMessage(rc)
                }
            }
        }
    }

    private fun startHeartbeatAndPoll() {
        stopHeartbeat()

        // 1s Heartbeat coroutine
        heartbeatJob = lifecycleScope.launch(Dispatchers.IO) {
            while (isActive) {
                delay(1000)
                val service = flashService ?: break
                val rc = service.heartbeat()
                if (rc != 0) {
                    withContext(Dispatchers.Main) {
                        status = status.copy(isOn = false)
                        errorMessage = parseErrorMessage(rc)
                    }
                    break
                }
            }
        }

        // Status poll coroutine (every 500ms)
        pollJob = lifecycleScope.launch(Dispatchers.IO) {
            while (isActive) {
                delay(500)
                val service = flashService ?: break
                val raw = service.status
                if (raw != null && raw.size >= 4) {
                    val updated = FlashStatus(
                        isOn = raw[0] != 0,
                        currentLevel = raw[1],
                        secondsOn = raw[2],
                        cooldownRemaining = raw[3]
                    )
                    withContext(Dispatchers.Main) {
                        status = updated
                        if (!updated.isOn) {
                            stopHeartbeat()
                        }
                    }
                }
            }
        }
    }

    private fun stopHeartbeat() {
        heartbeatJob?.cancel()
        heartbeatJob = null
        pollJob?.cancel()
        pollJob = null
    }

    private fun parseErrorMessage(code: Int): String {
        return when (code) {
            -1 -> "Camera or stock flashlight in use. Please close the camera app."
            -2 -> "Brightness level out of valid range (1..$maxLevel)."
            -3 -> "Invalid hardware state transition."
            -4 -> "Hardware I/O error occurred during kernel ioctl."
            -5 -> "Thermal or safety cooldown active. Please wait a moment."
            -6 -> "Kernel memory allocation error."
            -7 -> "Watchdog thread initialization error."
            -8 -> "Hardware unsupported (cam-flash node missing)."
            else -> "Operation failed with code: $code"
        }
    }

    override fun onStop() {
        super.onStop()
        // Turn off torch when app goes to background
        stopHeartbeat()
        lifecycleScope.launch(Dispatchers.IO) {
            try {
                flashService?.turnOff()
            } catch (_: Exception) {}
        }
    }

    override fun onDestroy() {
        stopHeartbeat()
        try {
            RootService.unbind(connection)
        } catch (_: Exception) {}
        super.onDestroy()
    }
}
