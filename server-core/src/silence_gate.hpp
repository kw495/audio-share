/*
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

#ifndef SILENCE_GATE_HPP
#define SILENCE_GATE_HPP

// Decides, packet by packet, whether captured audio is worth sending.
//
// The gate opens as soon as a packet's peak exceeds the threshold and stays
// open for `hold_ms` of audio after the last loud packet, so decaying tails
// and short pauses between words are not chopped. While closed, the most
// recent `preroll_ms` of audio is remembered and sent just before the packet
// that reopens the gate, so soft attacks are not clipped.
//
// Durations are measured in audio frames, not wall-clock time, so the
// behaviour is deterministic and independent of capture timing.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

class silence_gate {
public:
    enum class sample_format {
        f32,  // 32-bit IEEE float
        u8,   // 8-bit unsigned PCM (WAV convention, silence = 128)
        s16,
        s24,  // 24-bit packed little endian
        s32,
        unknown,
    };

    struct config {
        bool enabled = true;
        double threshold_db = -60.0; // peak level in dBFS below which audio counts as silence
        int hold_ms = 500;           // keep sending this long after the last loud packet
        int preroll_ms = 20;         // audio sent ahead of the packet that reopens the gate
    };

    void configure(const config& cfg, sample_format fmt, int sample_rate, int block_align)
    {
        _cfg = cfg;
        _fmt = fmt;
        _block_align = (std::max)(block_align, 1);
        _threshold = (float)std::pow(10.0, cfg.threshold_db / 20.0);
        _hold_frames = (int64_t)cfg.hold_ms * sample_rate / 1000;
        _preroll_bytes = (size_t)((int64_t)cfg.preroll_ms * sample_rate / 1000) * _block_align;
        reset();
    }

    void reset()
    {
        _open = false;
        _frames_since_loud = 0;
        _preroll.clear();
    }

    bool is_open() const { return _open; }

    // Feed one captured packet. Returns the bytes that should be sent, which
    // is empty while the gate is closed, the packet itself while open, and the
    // preroll followed by the packet at the moment the gate reopens.
    //
    // `known_silent` lets the caller pass the capture API's own silence flag
    // (WASAPI AUDCLNT_BUFFERFLAGS_SILENT); the data is then not inspected.
    //
    // The returned reference stays valid until the next call.
    const std::vector<uint8_t>& process(const uint8_t* data, size_t count, bool known_silent = false)
    {
        _out.clear();
        if (count == 0) {
            return _out;
        }

        if (!_cfg.enabled || _fmt == sample_format::unknown) {
            _out.assign(data, data + count);
            return _out;
        }

        const int64_t frames = (int64_t)(count / _block_align);
        const bool loud = !known_silent && exceeds_threshold(data, count);

        if (loud) {
            if (!_open) {
                _out.insert(_out.end(), _preroll.begin(), _preroll.end());
                _preroll.clear();
                _open = true;
            }
            _frames_since_loud = 0;
            append_packet(data, count, known_silent);
            return _out;
        }

        if (_open) {
            _frames_since_loud += frames;
            if (_frames_since_loud <= _hold_frames) {
                append_packet(data, count, known_silent);
                return _out;
            }
            _open = false;
        }

        remember_preroll(data, count, known_silent);
        return _out;
    }

private:
    // A packet flagged silent by the capture API may carry garbage, so send
    // real zeros (the format's silence value) instead of its contents.
    void append_packet(const uint8_t* data, size_t count, bool known_silent)
    {
        if (known_silent) {
            _out.insert(_out.end(), count, silence_byte());
        } else {
            _out.insert(_out.end(), data, data + count);
        }
    }

    void remember_preroll(const uint8_t* data, size_t count, bool known_silent)
    {
        if (_preroll_bytes == 0) {
            return;
        }
        if (known_silent) {
            _preroll.insert(_preroll.end(), count, silence_byte());
        } else {
            _preroll.insert(_preroll.end(), data, data + count);
        }
        if (_preroll.size() > _preroll_bytes) {
            // trim from the front on a frame boundary
            size_t excess = _preroll.size() - _preroll_bytes;
            excess += (_block_align - excess % _block_align) % _block_align;
            _preroll.erase(_preroll.begin(), _preroll.begin() + (std::min)(excess, _preroll.size()));
        }
    }

    uint8_t silence_byte() const { return _fmt == sample_format::u8 ? 128 : 0; }

    bool exceeds_threshold(const uint8_t* data, size_t count) const
    {
        switch (_fmt) {
        case sample_format::f32: {
            const size_t n = count / 4;
            for (size_t i = 0; i < n; ++i) {
                float v;
                std::memcpy(&v, data + i * 4, 4);
                if (std::fabs(v) > _threshold) {
                    return true;
                }
            }
            return false;
        }
        case sample_format::u8: {
            for (size_t i = 0; i < count; ++i) {
                if (std::abs((int)data[i] - 128) / 128.0f > _threshold) {
                    return true;
                }
            }
            return false;
        }
        case sample_format::s16: {
            const size_t n = count / 2;
            for (size_t i = 0; i < n; ++i) {
                int16_t v;
                std::memcpy(&v, data + i * 2, 2);
                if (std::abs((int)v) / 32768.0f > _threshold) {
                    return true;
                }
            }
            return false;
        }
        case sample_format::s24: {
            const size_t n = count / 3;
            for (size_t i = 0; i < n; ++i) {
                const uint8_t* p = data + i * 3;
                int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
                if (std::abs(v) / 8388608.0f > _threshold) {
                    return true;
                }
            }
            return false;
        }
        case sample_format::s32: {
            const size_t n = count / 4;
            for (size_t i = 0; i < n; ++i) {
                int32_t v;
                std::memcpy(&v, data + i * 4, 4);
                if (std::fabs((double)v) / 2147483648.0 > _threshold) {
                    return true;
                }
            }
            return false;
        }
        default:
            return true;
        }
    }

    config _cfg;
    sample_format _fmt = sample_format::unknown;
    int _block_align = 1;
    float _threshold = 0.001f;
    int64_t _hold_frames = 0;
    size_t _preroll_bytes = 0;

    bool _open = false;
    int64_t _frames_since_loud = 0;
    std::vector<uint8_t> _preroll;
    std::vector<uint8_t> _out;
};

#endif // SILENCE_GATE_HPP
