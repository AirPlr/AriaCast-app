package com.aria.ariacast

import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.security.MessageDigest
import java.util.Base64

/**
 * Minimal RFC 6455 WebSocket server that plays the "AriaCast Receiver" role
 * for AriaCompanion: the ESP32 WiFi sender board connects in as a client on
 * `/audio`, we reply with the {"status":"READY"} handshake AriaCompanion's
 * firmware waits for, then feed every binary frame it sends to [onAudioFrame].
 *
 * Only one sender is expected at a time; a new connection replaces whatever
 * session was active.
 */
class CompanionReceiverServer(
    private val port: Int,
    private val onAudioFrame: suspend (ByteArray) -> Unit
) {
    @Volatile var connected: Boolean = false
        private set

    private var serverSocket: ServerSocket? = null
    private var serverJob: Job? = null
    private var clientJob: Job? = null
    @Volatile private var activeClientSocket: Socket? = null

    fun start(scope: CoroutineScope) {
        stop()
        serverJob = scope.launch(Dispatchers.IO) {
            try {
                val socket = ServerSocket()
                socket.reuseAddress = true
                socket.bind(InetSocketAddress(port))
                serverSocket = socket
                Log.i(TAG, "Companion receiver listening on port $port")

                while (isActive) {
                    val client = try { socket.accept() } catch (e: Exception) { null } ?: continue
                    clientJob?.cancel()
                    activeClientSocket?.let { try { it.close() } catch (e: Exception) {} }
                    activeClientSocket = client
                    clientJob = launch { handleClient(client) }
                }
            } catch (e: Exception) {
                Log.e(TAG, "Companion receiver server error: ${e.message}")
            }
        }
    }

    fun stop() {
        serverJob?.cancel()
        serverJob = null
        clientJob?.cancel()
        clientJob = null
        try { activeClientSocket?.close() } catch (e: Exception) {}
        activeClientSocket = null
        try { serverSocket?.close() } catch (e: Exception) {}
        serverSocket = null
        connected = false
    }

    private suspend fun handleClient(socket: Socket) {
        try {
            socket.setTcpNoDelay(true)
            socket.soTimeout = 10000
            val input = DataInputStream(socket.getInputStream())
            val output = DataOutputStream(socket.getOutputStream())

            if (!performHandshake(input, output)) {
                Log.w(TAG, "Companion: WebSocket handshake failed")
                return
            }

            sendTextFrame(output, "{\"status\":\"READY\"}")
            connected = true
            Log.i(TAG, "Companion sender connected: ${socket.inetAddress?.hostAddress}")

            while (currentCoroutineContext().isActive) {
                val frame = readFrame(input) ?: break
                when (frame.opcode) {
                    OPCODE_BINARY -> onAudioFrame(frame.payload)
                    OPCODE_PING -> sendFrame(output, OPCODE_PONG, frame.payload)
                    OPCODE_CLOSE -> {
                        sendFrame(output, OPCODE_CLOSE, ByteArray(0))
                        break
                    }
                    else -> {}
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "Companion sender disconnected: ${e.message}")
        } finally {
            connected = false
            try { socket.close() } catch (e: Exception) {}
        }
    }

    private fun performHandshake(input: DataInputStream, output: DataOutputStream): Boolean {
        val headerLines = mutableListOf<String>()
        while (true) {
            val line = readLine(input) ?: return false
            if (line.isEmpty()) break
            headerLines.add(line)
        }

        val key = headerLines.firstOrNull { it.startsWith("Sec-WebSocket-Key:", ignoreCase = true) }
            ?.substringAfter(":")?.trim() ?: return false

        val accept = Base64.getEncoder().encodeToString(
            MessageDigest.getInstance("SHA-1").digest((key + WS_GUID).toByteArray(Charsets.US_ASCII))
        )

        val response = "HTTP/1.1 101 Switching Protocols\r\n" +
            "Upgrade: websocket\r\n" +
            "Connection: Upgrade\r\n" +
            "Sec-WebSocket-Accept: $accept\r\n\r\n"
        output.write(response.toByteArray(Charsets.US_ASCII))
        output.flush()
        return true
    }

    private fun readLine(input: DataInputStream): String? {
        val sb = StringBuilder()
        while (true) {
            val b = try { input.readByte() } catch (e: Exception) { return null }
            when (b) {
                '\r'.code.toByte() -> {}
                '\n'.code.toByte() -> return sb.toString()
                else -> sb.append(b.toInt().toChar())
            }
        }
    }

    private data class WsFrame(val opcode: Int, val payload: ByteArray)

    private fun readFrame(input: DataInputStream): WsFrame? {
        val b0 = try { input.readUnsignedByte() } catch (e: Exception) { return null }
        val opcode = b0 and 0x0F

        val b1 = input.readUnsignedByte()
        val masked = (b1 and 0x80) != 0
        var length = (b1 and 0x7F).toLong()
        if (length == 126L) {
            length = (input.readUnsignedByte().toLong() shl 8) or input.readUnsignedByte().toLong()
        } else if (length == 127L) {
            length = 0L
            repeat(8) { length = (length shl 8) or input.readUnsignedByte().toLong() }
        }

        val maskKey = ByteArray(4)
        if (masked) input.readFully(maskKey)

        val payload = ByteArray(length.toInt())
        input.readFully(payload)
        if (masked) {
            for (i in payload.indices) {
                payload[i] = (payload[i].toInt() xor maskKey[i % 4].toInt()).toByte()
            }
        }
        return WsFrame(opcode, payload)
    }

    private fun sendTextFrame(output: DataOutputStream, text: String) =
        sendFrame(output, OPCODE_TEXT, text.toByteArray(Charsets.UTF_8))

    private fun sendFrame(output: DataOutputStream, opcode: Int, payload: ByteArray) {
        output.writeByte(0x80 or opcode)
        when {
            payload.size < 126 -> output.writeByte(payload.size)
            payload.size <= 0xFFFF -> {
                output.writeByte(126)
                output.writeShort(payload.size)
            }
            else -> {
                output.writeByte(127)
                output.writeLong(payload.size.toLong())
            }
        }
        output.write(payload)
        output.flush()
    }

    companion object {
        private const val TAG = "CompanionReceiver"
        private const val WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
        private const val OPCODE_TEXT = 0x1
        private const val OPCODE_BINARY = 0x2
        private const val OPCODE_CLOSE = 0x8
        private const val OPCODE_PING = 0x9
        private const val OPCODE_PONG = 0xA
    }
}
