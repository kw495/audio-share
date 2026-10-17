#include "audio_manager.hpp"
#include "network_manager.hpp"

#include <chrono>
#include <spdlog/spdlog.h>

audio_manager::audio_manager()
{
    _format = std::make_unique<AudioFormat>();
}

audio_manager::~audio_manager() = default;

void audio_manager::start_loopback_recording(std::shared_ptr<network_manager> network_manager, const capture_config& config)
{
    _stopped = false;
    _record_thread = std::thread([network_manager = network_manager, config = config, self = shared_from_this()] {
        try {
            if (config.endpoint_id == test_tone_endpoint) {
                self->do_test_tone(network_manager, config);
            } else {
                self->do_loopback_recording(network_manager, config);
            }
        } catch (const std::exception& e) {
            spdlog::error("capture stopped: {}", e.what());
            if (!self->on_capture_error) {
                std::exit(-1);
            }
            self->on_capture_error(e.what());
        }
    });
}

void audio_manager::stop()
{
    _stopped = true;
    _record_thread.join();
}

std::string audio_manager::get_format_binary()
{
    return _format->SerializeAsString();
}

void audio_manager::do_test_tone(std::shared_ptr<network_manager> network_manager, const capture_config& config)
{
    using namespace std::chrono;
    constexpr int sample_rate = 48000;

    morse_tone tone;
    if (!tone.configure(config.test_tone, sample_rate)) {
        spdlog::warn("test tone: \"{}\" has no Morse characters, sending \"TEST\"", config.test_tone.text);
    }
    spdlog::info("test tone: \"{}\", {}wpm, {}Hz, repeats every {:.1f}s",
        config.test_tone.text, config.test_tone.wpm, config.test_tone.frequency,
        tone.cycle_frames() / (double)sample_rate);

    _format->set_encoding(AudioFormat::ENCODING_PCM_FLOAT);
    _format->set_channels(1);
    _format->set_sample_rate(sample_rate);

    // Paced by the clock: each round produces exactly the frames that are due,
    // so sleep jitter does not change the rate.
    const auto start = steady_clock::now();
    int64_t produced = 0;
    std::vector<float> buffer;
    while (!_stopped) {
        std::this_thread::sleep_for(10ms);
        const int64_t due = duration_cast<microseconds>(steady_clock::now() - start).count() * sample_rate / 1000000;
        int64_t frames = due - produced;
        if (frames <= 0) {
            continue;
        }
        if (frames > sample_rate) {
            // stalled (e.g. system sleep): skip ahead instead of bursting
            produced = due - sample_rate / 10;
            frames = sample_rate / 10;
        }
        buffer.resize((size_t)frames);
        tone.generate(buffer.data(), buffer.size());
        produced += frames;
        network_manager->broadcast_audio_data((const char*)buffer.data(), buffer.size() * sizeof(float), sizeof(float));
    }
}
