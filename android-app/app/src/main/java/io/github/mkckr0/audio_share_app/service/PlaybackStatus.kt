/*
 *    Copyright 2026 kw495
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

package io.github.mkckr0.audio_share_app.service

import android.content.Context
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import io.github.mkckr0.audio_share_app.R
import io.github.mkckr0.audio_share_app.pb.Client

/**
 * What the player is doing, for the Home screen: connection state, the format
 * the server sends, receive statistics and a short log.
 *
 * The service and the UI run in the same process, so this is plain Compose
 * state. Everything here is written from the main thread only.
 */
object PlaybackStatus {

    enum class State {
        STOPPED,      // not playing
        CONNECTING,   // connecting / handshaking
        SILENT,       // connected, nothing received recently (server is gating silence)
        RECEIVING,    // audio is arriving
        RETRYING,     // connection failed, waiting to reconnect
    }

    data class Format(
        val encoding: Client.AudioFormat.Encoding,
        val channels: Int,
        val sampleRate: Int,
        val bitrate: Int, // bits per second, as announced by the server (0 if unknown)
        val usage: String, // AudioUsageValues.*
    )

    data class Stats(
        val bytesPerSecond: Long = 0,
        val packetsPerSecond: Long = 0,
        val totalBytes: Long = 0,
        val totalPackets: Long = 0,
        val lastPacketAtMillis: Long = 0, // System.currentTimeMillis(), 0 = never
    )

    data class LogEntry(val timeMillis: Long, val message: String)

    private const val MAX_LOG = 200
    private const val TAG = "PlaybackStatus"

    var state by mutableStateOf(State.STOPPED)
    var server by mutableStateOf("")
    var format by mutableStateOf<Format?>(null)
    var stats by mutableStateOf(Stats())
    val logs = mutableStateListOf<LogEntry>()

    fun log(message: String) {
        Log.i(TAG, message)
        logs.add(LogEntry(System.currentTimeMillis(), message))
        if (logs.size > MAX_LOG) {
            logs.removeRange(0, logs.size - MAX_LOG)
        }
    }

    fun clearLog() {
        logs.clear()
    }

    /** e.g. "Opus 32 kbps · 48 kHz · Mono" */
    fun describe(context: Context, format: Format): String {
        val encoding = when (format.encoding) {
            Client.AudioFormat.Encoding.ENCODING_OPUS -> "Opus"
            Client.AudioFormat.Encoding.ENCODING_PCM_FLOAT -> "PCM 32-bit float"
            Client.AudioFormat.Encoding.ENCODING_PCM_8BIT -> "PCM 8-bit"
            Client.AudioFormat.Encoding.ENCODING_PCM_16BIT -> "PCM 16-bit"
            Client.AudioFormat.Encoding.ENCODING_PCM_24BIT -> "PCM 24-bit"
            Client.AudioFormat.Encoding.ENCODING_PCM_32BIT -> "PCM 32-bit"
            else -> context.getString(R.string.label_unsupported_format)
        }
        val channels = when (format.channels) {
            1 -> context.getString(R.string.label_mono)
            2 -> context.getString(R.string.label_stereo)
            else -> context.getString(R.string.label_channels_n, format.channels)
        }
        val rate = "%s kHz".format(formatNumber(format.sampleRate / 1000.0))
        val bitrate = if (format.bitrate > 0) " " + formatBitrate(format.bitrate) else ""
        return "$encoding$bitrate · $rate · $channels"
    }

    fun formatBitrate(bitsPerSecond: Int): String =
        if (bitsPerSecond >= 1_000_000) "%s Mbps".format(formatNumber(bitsPerSecond / 1_000_000.0))
        else "%s kbps".format(formatNumber(bitsPerSecond / 1000.0))

    fun formatBytes(bytes: Long): String = when {
        bytes >= 1_000_000 -> "%.1f MB".format(bytes / 1_000_000.0)
        bytes >= 1_000 -> "%.1f KB".format(bytes / 1_000.0)
        else -> "$bytes B"
    }

    // 48 -> "48", 44.1 -> "44.1"
    private fun formatNumber(value: Double): String =
        if (value == Math.floor(value)) value.toLong().toString() else "%.1f".format(value)

    /** Starts a new connection: the old format and counters no longer apply. */
    fun resetSession(server: String) {
        this.server = server
        format = null
        stats = Stats()
    }
}
