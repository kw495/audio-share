#include "config.h"
#include "audio_manager.hpp"
#include "network_manager.hpp"

#include <cxxopts.hpp>
#include <iostream>
#include <spdlog/spdlog.h>

using string = std::string;

int main(int argc, char* argv[])
{
    auto default_address = network_manager::get_default_address();

    std::string help_string = "Example:\n";
    help_string += fmt::format("  {} -b\n", AUDIO_SHARE_BIN_NAME);
    help_string += fmt::format("  {} --bind={}\n", AUDIO_SHARE_BIN_NAME, default_address.empty() ? "192.168.3.2": default_address);
    help_string += fmt::format("  {} --bind={} --encoding=f32 --channels=2 --sample-rate=48000\n", AUDIO_SHARE_BIN_NAME, default_address.empty() ? "192.168.3.2": default_address);
    help_string += fmt::format("  {} --bind={} --encoding=opus --channels=1 --opus-bitrate=32000\n", AUDIO_SHARE_BIN_NAME, default_address.empty() ? "192.168.3.2": default_address);
    help_string += fmt::format("  {} -b --test-tone=\"CQ TEST\" --encoding=opus\n", AUDIO_SHARE_BIN_NAME);
    help_string += fmt::format("  {} -l\n", AUDIO_SHARE_BIN_NAME);
    help_string += fmt::format("  {} --list-encoding\n", AUDIO_SHARE_BIN_NAME);
    cxxopts::Options options(AUDIO_SHARE_BIN_NAME, help_string);

    // clang-format off
    options.add_options()
        ("h,help", "Print usage")
        ("l,list-endpoint", "List available endpoints")
        ("b,bind", "The server bind address. If not set, will use default", cxxopts::value<string>()->implicit_value(default_address), "[host][:<port>]")
        ("e,endpoint", "Specify the endpoint id. If not set or set \"default\", will use default", cxxopts::value<string>()->default_value("default"), "[endpoint]")
        ("encoding", "Specify the encoding sent to clients. If not set or set \"default\", will use the capture encoding", cxxopts::value<audio_manager::encoding_t>()->default_value("default"), "[encoding]")
        ("list-encoding", "List available encoding")
        ("channels", "Specify the channels sent to clients: 1 (mono) or the capture channel count. If not set or set \"0\", will use the capture channels", cxxopts::value<int>()->default_value("0"), "[channels]")
        ("sample-rate", "Specify the sample rate(Hz) sent to clients. If not set or set \"0\", will use the capture rate. The common values are 16000, 24000, 48000, etc. Ignored with opus (always 48000)", cxxopts::value<int>()->default_value("0"), "[sample_rate]")
        ("opus-bitrate", "Opus bitrate in bits per second, only used with --encoding=opus", cxxopts::value<int>()->default_value("32000"), "[bps]")
        ("test-tone", "Send a repeating Morse code test signal instead of capturing (no sound device needed)", cxxopts::value<string>()->implicit_value("TEST"), "[text]")
        ("test-tone-wpm", "Morse speed in words per minute", cxxopts::value<int>()->default_value("20"), "[wpm]")
        ("test-tone-freq", "Morse tone frequency in Hz", cxxopts::value<int>()->default_value("700"), "[Hz]")
        ("no-silence-gate", "Send audio continuously, even while it is silent")
        ("gate-threshold", "Peak level (dBFS) below which audio counts as silence and is not sent", cxxopts::value<double>()->default_value("-60"), "[dB]")
        ("gate-hold", "Keep sending this long (ms) after the last non-silent audio", cxxopts::value<int>()->default_value("500"), "[ms]")
        ("gate-preroll", "Audio (ms) sent ahead of the packet that ends a silence", cxxopts::value<int>()->default_value("20"), "[ms]")
        ("V,verbose", "Set log level to \"trace\"")
        ("v,version", "Show version")
        ;
    // clang-format on

    try {
        auto result = options.parse(argc, argv);
        if (result.count("help")) {
            std::cout << options.help();
            return EXIT_SUCCESS;
        }

        if (result.count("version")) {
            fmt::println("{}\nversion: {}\nurl: {}\n", AUDIO_SHARE_BIN_NAME, AUDIO_SHARE_VERSION, AUDIO_SHARE_HOMEPAGE);
            return EXIT_SUCCESS;
        }

        if (result.count("verbose")) {
            spdlog::set_level(spdlog::level::trace);
        }

        if (result.count("list-endpoint")) {
            auto audio_manager = std::make_shared<class audio_manager>();
            auto endpoint_list = audio_manager->get_endpoint_list();
            auto default_endpoint = audio_manager->get_default_endpoint();
            auto s = fmt::format("endpoint list:\n");
            for (auto&& [id, name] : endpoint_list) {
                s += fmt::format("\t{} id: {:4} name: {}\n", (id == default_endpoint ? '*' : ' '), id, name);
            }
            s += fmt::format("total: {}\n", endpoint_list.size());
            std::cout << s;
            return EXIT_SUCCESS;
        }

        if (result.count("list-encoding")) {
            std::vector<std::pair<string, string>> array = {
                { "default", "Default encoding" },
                { "f32", "32 bit floating-point PCM" },
                { "s8", "8 bit integer PCM" },
                { "s16", "16 bit integer PCM" },
                { "s24", "24 bit integer PCM" },
                { "s32", "32 bit integer PCM" },
                { "opus", "Opus compressed audio, see --opus-bitrate" },
            };
            fmt::println("encoding list:");
            for(auto&& e : array) {
                fmt::println("\t{}\t\t{}", e.first, e.second);
            }
            return EXIT_SUCCESS;
        }

        if (result.count("bind")) {
            auto s = result["bind"].as<string>();
            size_t pos = s.find(':');
            string host = s.substr(0, pos);
            uint16_t port;
            if (pos == string::npos) {
                port = 65530;
            } else {
                port = (uint16_t)std::stoi(s.substr(pos + 1));
            }

            auto audio_manager = std::make_shared<class audio_manager>();

            audio_manager::capture_config capture_config;

            capture_config.endpoint_id = result["endpoint"].as<string>();
            if (result.count("test-tone")) {
                capture_config.endpoint_id = audio_manager::test_tone_endpoint;
                capture_config.test_tone.text = result["test-tone"].as<string>();
            }
            capture_config.test_tone.wpm = result["test-tone-wpm"].as<int>();
            capture_config.test_tone.frequency = result["test-tone-freq"].as<int>();
            capture_config.encoding = result["encoding"].as<audio_manager::encoding_t>();
            capture_config.channels = result["channels"].as<int>();
            capture_config.sample_rate = result["sample-rate"].as<int>();
            // Opus accepts 6kbps..510kbps
            capture_config.opus_bitrate = (std::clamp)(result["opus-bitrate"].as<int>(), 6000, 510000);
            capture_config.gate.enabled = result.count("no-silence-gate") == 0;
            capture_config.gate.threshold_db = result["gate-threshold"].as<double>();
            capture_config.gate.hold_ms = (std::max)(0, result["gate-hold"].as<int>());
            capture_config.gate.preroll_ms = (std::max)(0, result["gate-preroll"].as<int>());

            auto network_manager = std::make_shared<class network_manager>(audio_manager);

            network_manager->start_server(host, port, capture_config);
            network_manager->wait_server();

            return EXIT_SUCCESS;
        }

        // no arg
        std::cerr << options.help();
    } catch (const cxxopts::exceptions::exception& e) {
        std::cerr << e.what() << '\n'
                  << options.help();
        return EXIT_FAILURE;
    } catch (const std::exception& ec) {
        std::cerr << ec.what() << '\n';
        return EXIT_FAILURE;
    }
}
