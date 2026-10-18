/*
   Copyright 2026 kw495

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#ifndef OPUS_PACKER_HPP
#define OPUS_PACKER_HPP

// Collects float PCM at 48kHz into fixed 20ms frames and encodes each one as
// an Opus packet. Every packet is sent as its own UDP datagram, so packet
// boundaries are preserved without any extra framing.

#include <cstdint>
#include <vector>

#include <opus/opus.h>

class opus_packer {
public:
    static constexpr int sample_rate = 48000;
    static constexpr int frame_ms = 20;
    static constexpr int frame_size = sample_rate * frame_ms / 1000; // samples per channel

    using packet_list = std::vector<std::vector<uint8_t>>;

    opus_packer() = default;
    opus_packer(const opus_packer&) = delete;
    opus_packer& operator=(const opus_packer&) = delete;
    ~opus_packer() { destroy(); }

    // Returns the libopus error code (OPUS_OK on success).
    int configure(int channels, int bitrate)
    {
        destroy();
        _channels = channels;
        int err = OPUS_OK;
        _enc = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_AUDIO, &err);
        if (err != OPUS_OK) {
            _enc = nullptr;
            return err;
        }
        opus_encoder_ctl(_enc, OPUS_SET_BITRATE(bitrate));
        _pending.clear();
        return OPUS_OK;
    }

    // Starts a new stream after a gap, so the encoder does not blend old audio into it.
    void reset()
    {
        if (_enc) {
            opus_encoder_ctl(_enc, OPUS_RESET_STATE);
        }
        _pending.clear();
    }

    // Feeds interleaved samples and returns the packets completed by them.
    // The reference stays valid until the next call.
    const packet_list& push(const float* data, size_t samples)
    {
        _packets.clear();
        if (!_enc) {
            return _packets;
        }
        _pending.insert(_pending.end(), data, data + samples);
        const size_t frame_samples = (size_t)frame_size * _channels;
        size_t offset = 0;
        while (_pending.size() - offset >= frame_samples) {
            encode_frame(&_pending[offset]);
            offset += frame_samples;
        }
        _pending.erase(_pending.begin(), _pending.begin() + offset);
        return _packets;
    }

    // Pads the partial frame with silence and encodes it, so the tail of a
    // sound is not held back until the next one starts.
    const packet_list& flush()
    {
        _packets.clear();
        if (_enc && !_pending.empty()) {
            _pending.resize((size_t)frame_size * _channels, 0.0f);
            encode_frame(_pending.data());
            _pending.clear();
        }
        return _packets;
    }

private:
    void encode_frame(const float* frame)
    {
        // 1275 bytes is the largest packet a single Opus frame can produce.
        std::vector<uint8_t> packet(1275);
        const int len = opus_encode_float(_enc, frame, frame_size, packet.data(), (opus_int32)packet.size());
        if (len > 0) {
            packet.resize(len);
            _packets.push_back(std::move(packet));
        }
    }

    void destroy()
    {
        if (_enc) {
            opus_encoder_destroy(_enc);
            _enc = nullptr;
        }
    }

    OpusEncoder* _enc = nullptr;
    int _channels = 1;
    std::vector<float> _pending;
    packet_list _packets;
};

#endif // OPUS_PACKER_HPP
