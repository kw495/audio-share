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

import android.media.MediaCodec
import android.media.MediaFormat
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Decodes the server's Opus stream (one packet per UDP datagram) to 16-bit PCM
 * with the platform decoder, so no native library is needed.
 *
 * The codec runs on its own thread, so output for a packet may only appear a
 * little after it was queued; callers drain again shortly afterwards (see
 * [drain]) so the end of a sound is not held back until the next one.
 */
class OpusDecoder(sampleRate: Int, channels: Int) {
    private val codec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS)
    private val bufferInfo = MediaCodec.BufferInfo()
    private var presentationTimeUs = 0L

    init {
        val format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, sampleRate, channels)
        format.setByteBuffer("csd-0", opusHead(sampleRate, channels))
        format.setByteBuffer("csd-1", nanos(0))           // codec delay
        format.setByteBuffer("csd-2", nanos(80_000_000))  // seek pre-roll
        codec.configure(format, null, null, 0)
        codec.start()
    }

    /** Queues one packet (dropped if the codec is busy) and passes on any PCM already decoded. */
    fun decode(packet: ByteBuffer, output: (ByteBuffer) -> Unit) {
        val index = codec.dequeueInputBuffer(0)
        if (index >= 0) {
            val size = packet.remaining()
            codec.getInputBuffer(index)!!.apply {
                clear()
                put(packet)
            }
            codec.queueInputBuffer(index, 0, size, presentationTimeUs, 0)
            presentationTimeUs += FRAME_US
        }
        drain(output)
    }

    /** Passes on all PCM decoded so far, without waiting. */
    fun drain(output: (ByteBuffer) -> Unit) {
        while (true) {
            val index = codec.dequeueOutputBuffer(bufferInfo, 0)
            when {
                index >= 0 -> {
                    if (bufferInfo.size > 0) {
                        val buffer = codec.getOutputBuffer(index)!!
                        buffer.position(bufferInfo.offset)
                        buffer.limit(bufferInfo.offset + bufferInfo.size)
                        output(buffer)
                    }
                    codec.releaseOutputBuffer(index, false)
                }
                index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> continue
                else -> return // INFO_TRY_AGAIN_LATER
            }
        }
    }

    /** Drops decoder state, for a new stream after a gap (the server resets its encoder too). */
    fun flush() {
        codec.flush()
    }

    fun release() {
        codec.stop()
        codec.release()
    }

    private companion object {
        const val FRAME_US = 20_000L // the server always sends 20ms frames

        // RFC 7845 identification header, mapping family 0 (mono or stereo)
        fun opusHead(sampleRate: Int, channels: Int): ByteBuffer =
            ByteBuffer.allocate(19).order(ByteOrder.LITTLE_ENDIAN).apply {
                put("OpusHead".toByteArray(Charsets.US_ASCII))
                put(1)                   // version
                put(channels.toByte())
                putShort(0)              // pre-skip
                putInt(sampleRate)       // original input rate, informational
                putShort(0)              // output gain
                put(0)                   // channel mapping family
                flip()
            }

        fun nanos(value: Long): ByteBuffer =
            ByteBuffer.allocate(8).order(ByteOrder.nativeOrder()).apply {
                putLong(value)
                flip()
            }
    }
}
