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

#ifndef TEST_TONE_HPP
#define TEST_TONE_HPP

// Synthesizes a Morse code test signal, so the server can be checked end to
// end on a machine without any sound device.
//
// Standard timing in dot units: dot 1, dash 3, gap inside a character 1,
// between characters 3, between words 7; the message repeats after
// `repeat_gap_ms` of silence. Each key-down is shaped with a 5ms raised-cosine
// ramp so the tone starts and stops without clicks.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

class morse_tone {
public:
    struct config {
        std::string text = "TEST";
        int wpm = 20;           // words per minute ("PARIS" = 50 dot units)
        int frequency = 700;    // Hz
        double level_db = -12;  // peak level in dBFS
        int repeat_gap_ms = 3000;
    };

    // Characters without a Morse code are skipped. Returns false when nothing
    // in `text` can be sent, in which case "TEST" is used instead.
    bool configure(const config& cfg, int sample_rate)
    {
        _sample_rate = (std::max)(sample_rate, 8000);
        _wpm = (std::clamp)(cfg.wpm, 5, 60);
        _frequency = (std::clamp)(cfg.frequency, 100, 4000);
        _amplitude = (float)std::pow(10.0, (std::min)(cfg.level_db, 0.0) / 20.0);
        _ramp_frames = (std::max)(1, _sample_rate * 5 / 1000);

        bool valid = build(cfg.text, cfg.repeat_gap_ms);
        if (!valid) {
            build("TEST", cfg.repeat_gap_ms);
        }
        _segment = 0;
        _segment_pos = 0;
        _phase = 0.0;
        _envelope = 0.0f;
        return valid;
    }

    int64_t dot_frames() const { return (int64_t)_sample_rate * 1200 / (_wpm * 1000); }

    // Length of one full repetition (message + gap), in frames.
    int64_t cycle_frames() const
    {
        int64_t total = 0;
        for (auto& s : _segments) {
            total += s.frames;
        }
        return total;
    }

    // Writes `frames` mono float samples.
    void generate(float* out, size_t frames)
    {
        constexpr double two_pi = 6.28318530717958647692;
        const double step = two_pi * _frequency / _sample_rate;
        const float ramp_step = 1.0f / (float)_ramp_frames;
        for (size_t i = 0; i < frames; ++i) {
            const bool key_down = _segments[_segment].on;
            _envelope = key_down ? (std::min)(1.0f, _envelope + ramp_step) : (std::max)(0.0f, _envelope - ramp_step);
            const float gain = 0.5f - 0.5f * (float)std::cos(3.14159265358979323846 * _envelope);
            out[i] = gain == 0.0f ? 0.0f : _amplitude * gain * (float)std::sin(_phase);
            _phase += step;
            if (_phase >= two_pi) {
                _phase -= two_pi;
            }
            if (++_segment_pos >= _segments[_segment].frames) {
                _segment_pos = 0;
                _segment = (_segment + 1) % _segments.size();
            }
        }
    }

private:
    struct segment {
        bool on;
        int64_t frames;
    };

    static const char* code_of(char c)
    {
        static const char* letters[26] = {
            ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
            "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--..",
        };
        static const char* digits[10] = {
            "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----.",
        };
        if (c >= 'A' && c <= 'Z') {
            return letters[c - 'A'];
        }
        if (c >= '0' && c <= '9') {
            return digits[c - '0'];
        }
        switch (c) {
        case '.': return ".-.-.-";
        case ',': return "--..--";
        case '?': return "..--..";
        case '/': return "-..-.";
        case '=': return "-...-";
        case '-': return "-....-";
        case '+': return ".-.-.";
        case '@': return ".--.-.";
        case ':': return "---...";
        case '(': return "-.--.";
        case ')': return "-.--.-";
        case '\'': return ".----.";
        case '"': return ".-..-.";
        case '!': return "-.-.--";
        default: return nullptr;
        }
    }

    bool build(const std::string& text, int repeat_gap_ms)
    {
        _segments.clear();
        const int64_t dot = dot_frames();
        bool any = false;
        bool pending_word_gap = false;
        for (char raw : text) {
            const char c = (char)std::toupper((unsigned char)raw);
            if (c == ' ') {
                pending_word_gap = any;
                continue;
            }
            const char* code = code_of(c);
            if (!code) {
                continue;
            }
            if (any) {
                add(false, (pending_word_gap ? 7 : 3) * dot);
            }
            pending_word_gap = false;
            for (const char* p = code; *p; ++p) {
                if (p != code) {
                    add(false, dot);
                }
                add(true, (*p == '-' ? 3 : 1) * dot);
            }
            any = true;
        }
        if (!any) {
            return false;
        }
        // word gap after the message, then the repeat pause
        add(false, 7 * dot + (int64_t)(std::max)(repeat_gap_ms, 0) * _sample_rate / 1000);
        return true;
    }

    void add(bool on, int64_t frames)
    {
        if (!_segments.empty() && _segments.back().on == on) {
            _segments.back().frames += frames;
        } else {
            _segments.push_back({ on, frames });
        }
    }

    int _sample_rate = 48000;
    int _wpm = 20;
    int _frequency = 700;
    float _amplitude = 0.25f;
    int _ramp_frames = 240;

    std::vector<segment> _segments;
    size_t _segment = 0;
    int64_t _segment_pos = 0;
    double _phase = 0.0;
    float _envelope = 0.0f;
};

#endif // TEST_TONE_HPP
