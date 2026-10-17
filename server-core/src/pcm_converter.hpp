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

#ifndef PCM_CONVERTER_HPP
#define PCM_CONVERTER_HPP

// Turns captured PCM into the audio that is sent: any supported sample format
// is decoded to float, optionally mixed to mono or stereo, and resampled.
//
// Conversion happens here rather than in the capture API because WASAPI
// loopback only delivers the device mix format; asking it for another channel
// count or sample rate is either silently ignored or refused outright.
//
// The resampler is a windowed-sinc filter whose cutoff follows the lower of the
// two rates, so downsampling (48k -> 24k) does not fold high frequencies back
// into the audible band. Its coefficients are tabulated once per configure().

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "silence_gate.hpp"

class pcm_converter {
public:
    using sample_format = silence_gate::sample_format;

    static int bytes_per_sample(sample_format fmt)
    {
        switch (fmt) {
        case sample_format::u8:
            return 1;
        case sample_format::s16:
            return 2;
        case sample_format::s24:
            return 3;
        case sample_format::f32:
        case sample_format::s32:
            return 4;
        default:
            return 0;
        }
    }

    // Channel count produced for a requested `out_channels`: 1 (mono), 2 (stereo,
    // from mono by duplication or from more channels by downmix), anything else
    // keeps the input layout.
    static int resolve_channels(int in_channels, int out_channels)
    {
        if (out_channels == 1 || out_channels == 2) {
            return out_channels;
        }
        return (std::max)(in_channels, 1);
    }

    void configure(sample_format in_fmt, int in_channels, int in_rate, int out_channels, int out_rate)
    {
        _in_fmt = in_fmt;
        _in_channels = (std::max)(in_channels, 1);
        _out_channels = resolve_channels(_in_channels, out_channels);
        build_stereo_mix();
        _in_rate = (std::max)(in_rate, 1);
        _out_rate = out_rate > 0 ? out_rate : _in_rate;
        _bypass_resample = _in_rate == _out_rate;
        if (_bypass_resample) {
            _half = 0;
            _filter.clear();
        } else {
            build_filter();
        }
        reset();
    }

    void reset()
    {
        _history.assign((size_t)(std::max)(_half - 1, 0) * _out_channels, 0.0f);
        _pos_int = (std::max)(_half - 1, 0);
        _pos_frac = 0;
    }

    int out_channels() const { return _out_channels; }
    int out_rate() const { return _out_rate; }

    // Converts one captured packet. Returns interleaved float frames at the
    // output channel count and rate; the reference stays valid until the next call.
    // A packet flagged silent by the capture API is treated as zeros.
    const std::vector<float>& process(const uint8_t* data, size_t count, bool known_silent = false)
    {
        _out.clear();
        const int bps = bytes_per_sample(_in_fmt);
        if (bps == 0 || count == 0) {
            return _out;
        }
        const size_t frames = count / ((size_t)bps * _in_channels);

        // decode + downmix into _mixed
        _mixed.resize(frames * _out_channels);
        for (size_t f = 0; f < frames; ++f) {
            const uint8_t* frame = data + f * bps * _in_channels;
            if (_out_channels == 1) {
                float sum = 0.0f;
                for (int c = 0; c < _in_channels; ++c) {
                    sum += known_silent ? 0.0f : read_sample(frame + c * bps);
                }
                _mixed[f] = sum / (float)_in_channels;
            } else if (_out_channels == 2 && _in_channels != 2) {
                float l = 0.0f, r = 0.0f;
                if (!known_silent) {
                    for (int c = 0; c < _in_channels; ++c) {
                        const float v = read_sample(frame + c * bps);
                        l += _mix_l[c] * v;
                        r += _mix_r[c] * v;
                    }
                }
                _mixed[f * 2] = l;
                _mixed[f * 2 + 1] = r;
            } else {
                for (int c = 0; c < _in_channels; ++c) {
                    _mixed[f * _out_channels + c] = known_silent ? 0.0f : read_sample(frame + c * bps);
                }
            }
        }

        if (_bypass_resample) {
            _out.swap(_mixed);
            return _out;
        }
        resample();
        return _out;
    }

    // Quantizes float samples to `fmt`, replacing the contents of `out`.
    static void encode(const float* in, size_t samples, sample_format fmt, std::vector<uint8_t>& out)
    {
        const int bps = bytes_per_sample(fmt);
        out.resize(samples * bps);
        uint8_t* p = out.data();
        for (size_t i = 0; i < samples; ++i, p += bps) {
            const float v = (std::clamp)(in[i], -1.0f, 1.0f);
            switch (fmt) {
            case sample_format::f32:
                std::memcpy(p, &in[i], 4);
                break;
            case sample_format::u8:
                *p = (uint8_t)std::lrint(128.0f + v * 127.0f);
                break;
            case sample_format::s16: {
                const int16_t s = (int16_t)std::lrint(v * 32767.0f);
                std::memcpy(p, &s, 2);
                break;
            }
            case sample_format::s24: {
                const int32_t s = (int32_t)std::lrint(v * 8388607.0f);
                p[0] = (uint8_t)(s & 0xff);
                p[1] = (uint8_t)((s >> 8) & 0xff);
                p[2] = (uint8_t)((s >> 16) & 0xff);
                break;
            }
            case sample_format::s32: {
                const int32_t s = (int32_t)std::llrint((double)v * 2147483647.0);
                std::memcpy(p, &s, 4);
                break;
            }
            default:
                break;
            }
        }
    }

private:
    float read_sample(const uint8_t* p) const
    {
        switch (_in_fmt) {
        case sample_format::f32: {
            float v;
            std::memcpy(&v, p, 4);
            return v;
        }
        case sample_format::u8:
            return ((int)p[0] - 128) / 128.0f;
        case sample_format::s16: {
            int16_t v;
            std::memcpy(&v, p, 2);
            return v / 32768.0f;
        }
        case sample_format::s24: {
            const int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
            return v / 8388608.0f;
        }
        case sample_format::s32: {
            int32_t v;
            std::memcpy(&v, p, 4);
            return (float)(v / 2147483648.0);
        }
        default:
            return 0.0f;
        }
    }

    // Weights for producing stereo from a layout that is not stereo. Mono is
    // copied to both sides. 5.1 and 7.1 (WAVEFORMATEXTENSIBLE order FL FR FC
    // LFE BL BR [SL SR]) use the usual -3dB centre/surround downmix without LFE;
    // other layouts take their first two channels. Each side is normalized so
    // a full-scale input cannot clip.
    void build_stereo_mix()
    {
        _mix_l.assign(_in_channels, 0.0f);
        _mix_r.assign(_in_channels, 0.0f);
        if (_out_channels != 2 || _in_channels == 2) {
            return;
        }
        if (_in_channels == 1) {
            _mix_l[0] = _mix_r[0] = 1.0f;
            return;
        }
        constexpr float k = 0.7071f;
        if (_in_channels == 6 || _in_channels == 8) {
            _mix_l[0] = 1.0f;
            _mix_r[1] = 1.0f;
            _mix_l[2] = _mix_r[2] = k;
            _mix_l[4] = k;
            _mix_r[5] = k;
            if (_in_channels == 8) {
                _mix_l[6] = k;
                _mix_r[7] = k;
            }
        } else {
            _mix_l[0] = 1.0f;
            _mix_r[1] = 1.0f;
        }
        float sum_l = 0.0f, sum_r = 0.0f;
        for (int c = 0; c < _in_channels; ++c) {
            sum_l += _mix_l[c];
            sum_r += _mix_r[c];
        }
        for (int c = 0; c < _in_channels; ++c) {
            _mix_l[c] /= sum_l;
            _mix_r[c] /= sum_r;
        }
    }

    // Tabulates the filter at `_phases` + 1 fractional offsets; coefficients
    // in between are linearly interpolated. Each row is normalized to unity gain.
    void build_filter()
    {
        constexpr double pi = 3.14159265358979323846;
        constexpr int zero_crossings = 16;
        // Cutoff as a fraction of the input Nyquist frequency, with a margin for the transition band.
        const double cutoff = (std::min)(1.0, (double)_out_rate / _in_rate) * 0.9;
        _half = (int)std::ceil(zero_crossings / cutoff);
        const int taps = 2 * _half;
        _filter.assign((size_t)(_phases + 1) * taps, 0.0f);
        for (int p = 0; p <= _phases; ++p) {
            const double frac = (double)p / _phases;
            double sum = 0.0;
            float* row = &_filter[(size_t)p * taps];
            for (int k = 0; k < taps; ++k) {
                // distance from the output instant to input sample (i0 - half + 1 + k)
                const double x = k - _half + 1 - frac;
                const double u = x / _half;
                if (u <= -1.0 || u >= 1.0) {
                    continue;
                }
                const double t = cutoff * x;
                const double sinc = t == 0.0 ? 1.0 : std::sin(pi * t) / (pi * t);
                const double window = 0.42 + 0.5 * std::cos(pi * u) + 0.08 * std::cos(2.0 * pi * u);
                row[k] = (float)(cutoff * sinc * window);
                sum += row[k];
            }
            if (sum != 0.0) {
                for (int k = 0; k < taps; ++k) {
                    row[k] = (float)(row[k] / sum);
                }
            }
        }
    }

    void resample()
    {
        const int ch = _out_channels;
        const int taps = 2 * _half;
        _history.insert(_history.end(), _mixed.begin(), _mixed.end());
        const int64_t frames = (int64_t)(_history.size() / ch);

        // Output instant n sits at input position _pos_int + _pos_frac / _out_rate
        // and needs input frames _pos_int - half + 1 ... _pos_int + half.
        while (_pos_int + _half < frames) {
            const double t = (double)_pos_frac * _phases / _out_rate;
            const int p = (std::min)((int)t, _phases - 1);
            const float a = (float)(t - p);
            const float* row0 = &_filter[(size_t)p * taps];
            const float* row1 = row0 + taps;
            const float* src = &_history[(size_t)(_pos_int - _half + 1) * ch];
            for (int c = 0; c < ch; ++c) {
                float acc = 0.0f;
                for (int k = 0; k < taps; ++k) {
                    const float coef = row0[k] + (row1[k] - row0[k]) * a;
                    acc += coef * src[(size_t)k * ch + c];
                }
                _out.push_back(acc);
            }
            _pos_frac += _in_rate;
            _pos_int += _pos_frac / _out_rate;
            _pos_frac %= _out_rate;
        }

        // drop input that no future output needs
        const int64_t drop = (std::min)(_pos_int - _half + 1, frames);
        if (drop > 0) {
            _history.erase(_history.begin(), _history.begin() + (size_t)drop * ch);
            _pos_int -= drop;
        }
    }

    sample_format _in_fmt = sample_format::unknown;
    int _in_channels = 1;
    int _out_channels = 1;
    int _in_rate = 48000;
    int _out_rate = 48000;
    bool _bypass_resample = true;
    std::vector<float> _mix_l, _mix_r; // stereo downmix/upmix weights per input channel

    static constexpr int _phases = 256;
    int _half = 0;
    std::vector<float> _filter;

    std::vector<float> _history; // interleaved input frames still needed by the filter
    int64_t _pos_int = 0;
    int64_t _pos_frac = 0; // numerator over _out_rate

    std::vector<float> _mixed;
    std::vector<float> _out;
};

#endif // PCM_CONVERTER_HPP
