/*
   Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>

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

#include "network_manager.hpp"
#include "formatter.hpp"
#include "audio_manager.hpp"

#include <list>
#include <ranges>
#include <coroutine>

#ifdef _WINDOWS
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Iphlpapi.lib")
#endif // _WINDOWS

#ifdef linux
#include <sys/types.h>
#include <ifaddrs.h>
#endif

#include <spdlog/spdlog.h>
#include <fmt/ranges.h>

namespace ip = asio::ip;
using namespace std::chrono_literals;

network_manager::network_manager(std::shared_ptr<audio_manager>& audio_manager)
    : _audio_manager(audio_manager)
{
}

std::vector<std::string> network_manager::get_address_list()
{
    std::vector<std::string> address_list;

#ifdef _WINDOWS
    ULONG family = AF_INET;
    ULONG flags = GAA_FLAG_INCLUDE_ALL_INTERFACES;

    ULONG size = 0;
    GetAdaptersAddresses(family, flags, nullptr, nullptr, &size);
    auto pAddresses = (PIP_ADAPTER_ADDRESSES)malloc(size);

    auto ret = GetAdaptersAddresses(family, flags, nullptr, pAddresses, &size);
    if (ret == ERROR_SUCCESS) {
        for (auto pCurrentAddress = pAddresses; pCurrentAddress; pCurrentAddress = pCurrentAddress->Next) {
            if (pCurrentAddress->OperStatus != IfOperStatusUp || pCurrentAddress->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                continue;
            }

            for (auto pUnicast = pCurrentAddress->FirstUnicastAddress; pUnicast; pUnicast = pUnicast->Next) {
                auto sockaddr = (sockaddr_in*)pUnicast->Address.lpSockaddr;
                char buf[50];
                if (inet_ntop(AF_INET, &sockaddr->sin_addr, buf, sizeof(buf))) {
                    address_list.emplace_back(buf);
                }
            }
        }
    }

    free(pAddresses);
#endif

#ifdef linux
    struct ifaddrs* ifaddrs;
    if (getifaddrs(&ifaddrs) == -1) {
        return address_list;
    }

    for (auto ifa = ifaddrs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) {
            continue;
        }
        if (ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (ifa->ifa_flags & IFF_LOOPBACK) {
            continue;
        }
        auto sockaddr = (sockaddr_in*)ifa->ifa_addr;
        char buf[50];
        if (inet_ntop(AF_INET, &sockaddr->sin_addr, buf, sizeof(buf))) {
            address_list.emplace_back(buf);
        }
    }

    freeifaddrs(ifaddrs);
#endif

    return address_list;
}

std::string network_manager::get_default_address()
{
    return select_default_address(get_address_list());
}

std::string network_manager::select_default_address(const std::vector<std::string>& address_list)
{
    if (address_list.empty()) {
        return {};
    }

    auto is_private_address = [](const std::string& address) {
        constexpr uint32_t private_addr_list[] = {
            0x0a000000,
            0xac100000,
            0xc0a80000,
        };

        uint32_t addr;
        inet_pton(AF_INET, address.c_str(), &addr);
        addr = ntohl(addr);
        for (auto&& private_addr : private_addr_list) {
            if ((addr & private_addr) == private_addr) {
                return true;
            }
        }

        return false;
    };

    for (auto&& address : address_list) {
        if (is_private_address(address)) {
            return address;
        }
    }
    return address_list.front();
}

void network_manager::start_server(const std::string& host, uint16_t port, const audio_manager::capture_config& capture_config)
{
    _ioc = std::make_shared<asio::io_context>();
    _capture_config = capture_config;
    _pipeline_ready = false;
    {
        ip::tcp::endpoint endpoint { ip::make_address(host), port };

        ip::tcp::acceptor acceptor(*_ioc, endpoint.protocol());
        acceptor.set_option(ip::tcp::acceptor::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen();

        _audio_manager->start_loopback_recording(shared_from_this(), capture_config);
        asio::co_spawn(*_ioc, accept_tcp_loop(std::move(acceptor)), asio::detached);

        // start tcp success
        spdlog::info("tcp listen success on {}", endpoint);
    }

    {
        ip::udp::endpoint endpoint { ip::make_address(host), port };
        _udp_server = std::make_unique<udp_socket>(*_ioc, endpoint.protocol());
        _udp_server->bind(endpoint);
        asio::co_spawn(*_ioc, accept_udp_loop(), asio::detached);

        // start udp success
        spdlog::info("udp listen success on {}", endpoint);
    }

    _net_thread = std::thread([self = shared_from_this()] {
        self->_ioc->run();
    });

    spdlog::info("server started");
}

void network_manager::stop_server()
{
    if (_ioc) {
        _ioc->stop();
    }
    _net_thread.join();
    _audio_manager->stop();
    _playing_peer_list.clear();
    _udp_server = nullptr;
    _ioc = nullptr;
    spdlog::info("server stopped");
}

void network_manager::wait_server()
{
    _net_thread.join();
}

bool network_manager::is_running() const
{
    return _ioc != nullptr;
}

asio::awaitable<void> network_manager::read_loop(std::shared_ptr<tcp_socket> peer)
{
    while (true) {
        cmd_t cmd = cmd_t::cmd_none;
        auto [ec, _] = co_await asio::async_read(*peer, asio::buffer(&cmd, sizeof(cmd)));
        if (ec) {
            close_session(peer);
            spdlog::trace("{} {}", __func__, ec);
            break;
        }

        spdlog::trace("cmd {}", (uint32_t)cmd);

        if (cmd == cmd_t::cmd_get_format) {
            auto format = wire_format(*_audio_manager->get_format(), _capture_config).SerializeAsString();
            auto size = (uint32_t)format.size();
            std::array<asio::const_buffer, 3> buffers = {
                asio::buffer(&cmd, sizeof(cmd)),
                asio::buffer(&size, sizeof(size)),
                asio::buffer(format),
            };
            auto [ec, _] = co_await asio::async_write(*peer, buffers);
            if (ec) {
                close_session(peer);
                spdlog::trace("{} {}", __func__, ec);
                break;
            }
        } else if (cmd == cmd_t::cmd_start_play) {
            int id = add_playing_peer(peer);
            if (id <= 0) {
                spdlog::error("{} id error", __func__);
                close_session(peer);
                spdlog::trace("{} {}", __func__, ec);
                break;
            }
            std::array<asio::const_buffer, 2> buffers = {
                asio::buffer(&cmd, sizeof(cmd)),
                asio::buffer(&id, sizeof(id)),
            };
            auto [ec, _] = co_await asio::async_write(*peer, buffers);
            if (ec) {
                spdlog::trace("{} {}", __func__, ec);
                close_session(peer);
                break;
            }
            asio::co_spawn(*_ioc, heartbeat_loop(peer), asio::detached);
        } else if (cmd == cmd_t::cmd_heartbeat) {
            auto it = _playing_peer_list.find(peer);
            if (it != _playing_peer_list.end()) {
                it->second->last_tick = std::chrono::steady_clock::now();
            }
        } else {
            spdlog::error("{} error cmd", __func__);
            close_session(peer);
            break;
        }
    }
    spdlog::trace("stop {}", __func__);
}

asio::awaitable<void> network_manager::heartbeat_loop(std::shared_ptr<tcp_socket> peer)
{
    std::error_code ec;
    size_t _;

    steady_timer timer(*_ioc);
    while (true) {
        timer.expires_after(3s);
        std::tie(ec) = co_await timer.async_wait();
        if (ec) {
            break;
        }

        if (!peer->is_open()) {
            break;
        }

        auto it = _playing_peer_list.find(peer);
        if (it == _playing_peer_list.end()) {
            spdlog::trace("{} it == _playing_peer_list.end()", __func__);
            close_session(peer);
            break;
        }
        if (std::chrono::steady_clock::now() - it->second->last_tick > _heartbeat_timeout) {
            spdlog::info("{} timeout", it->first->remote_endpoint());
            close_session(peer);
            break;
        }

        auto cmd = cmd_t::cmd_heartbeat;
        std::tie(ec, _) = co_await asio::async_write(*peer, asio::buffer(&cmd, sizeof(cmd)));
        if (ec) {
            spdlog::trace("{} {}", __func__, ec);
            close_session(peer);
            break;
        }
    }
    spdlog::trace("stop {}", __func__);
}

asio::awaitable<void> network_manager::accept_tcp_loop(tcp_acceptor acceptor)
{
    while (true) {
        auto peer = std::make_shared<tcp_socket>(acceptor.get_executor());
        auto [ec] = co_await acceptor.async_accept(*peer);
        if (ec) {
            spdlog::error("{} {}", __func__, ec);
            co_return;
        }

        spdlog::info("accept {}", peer->remote_endpoint());

        // No-Delay
        peer->set_option(ip::tcp::no_delay(true), ec);
        if (ec) {
            spdlog::info("{} {}", __func__, ec);
        }

        asio::co_spawn(acceptor.get_executor(), read_loop(peer), asio::detached);
    }
}

asio::awaitable<void> network_manager::accept_udp_loop()
{
    while (true) {
        int id = 0;
        ip::udp::endpoint udp_peer;
        auto [ec, _] = co_await _udp_server->async_receive_from(asio::buffer(&id, sizeof(id)), udp_peer);
        if (ec) {
            spdlog::info("{} {}", __func__, ec);
            co_return;
        }

        fill_udp_peer(id, udp_peer);
    }
}

auto network_manager::close_session(std::shared_ptr<tcp_socket>& peer) -> playing_peer_list_t::iterator
{
    spdlog::info("close {}", peer->remote_endpoint());
    auto it = remove_playing_peer(peer);
    peer->shutdown(ip::tcp::socket::shutdown_both);
    peer->close();
    return it;
}

int network_manager::add_playing_peer(std::shared_ptr<tcp_socket>& peer)
{
    if (_playing_peer_list.contains(peer)) {
        spdlog::error("{} repeat add tcp://{}", __func__, peer->remote_endpoint());
        return 0;
    }

    auto info = _playing_peer_list[peer] = std::make_shared<peer_info_t>();
    static int g_id = 0;
    info->id = ++g_id;
    info->last_tick = std::chrono::steady_clock::now();

    spdlog::trace("{} add id:{} tcp://{}", __func__, info->id, peer->remote_endpoint());
    return info->id;
}

auto network_manager::remove_playing_peer(std::shared_ptr<tcp_socket>& peer) -> playing_peer_list_t::iterator
{
    auto it = _playing_peer_list.find(peer);
    if (it == _playing_peer_list.end()) {
        spdlog::error("{} repeat remove tcp://{}", __func__, peer->remote_endpoint());
        return it;
    }

    it = _playing_peer_list.erase(it);
    spdlog::trace("{} remove tcp://{}", __func__, peer->remote_endpoint());
    return it;
}

void network_manager::fill_udp_peer(int id, asio::ip::udp::endpoint udp_peer)
{
    auto it = std::find_if(_playing_peer_list.begin(), _playing_peer_list.end(), [id](const playing_peer_list_t::value_type& e) {
        return e.second->id == id;
    });

    if (it == _playing_peer_list.cend()) {
        spdlog::error("{} no tcp peer id:{} udp://{}", __func__, id, udp_peer);
        return;
    }

    it->second->udp_peer = udp_peer;
    spdlog::info("{} fill udp peer id:{} tcp://{} udp://{}", __func__, id, it->first->remote_endpoint(), udp_peer);
}

static silence_gate::sample_format to_sample_format(audio_manager::AudioFormat::Encoding encoding)
{
    using AudioFormat = audio_manager::AudioFormat;
    switch (encoding) {
    case AudioFormat::ENCODING_PCM_FLOAT:
        return silence_gate::sample_format::f32;
    case AudioFormat::ENCODING_PCM_8BIT:
        return silence_gate::sample_format::u8;
    case AudioFormat::ENCODING_PCM_16BIT:
        return silence_gate::sample_format::s16;
    case AudioFormat::ENCODING_PCM_24BIT:
        return silence_gate::sample_format::s24;
    case AudioFormat::ENCODING_PCM_32BIT:
        return silence_gate::sample_format::s32;
    default:
        return silence_gate::sample_format::unknown;
    }
}

audio_manager::AudioFormat network_manager::wire_format(const audio_manager::AudioFormat& capture, const audio_manager::capture_config& config)
{
    using AudioFormat = audio_manager::AudioFormat;
    using encoding_t = audio_manager::encoding_t;

    AudioFormat out; // encoding stays ENCODING_INVALID until capture has started
    if (capture.encoding() == AudioFormat::ENCODING_INVALID || capture.channels() <= 0 || capture.sample_rate() <= 0) {
        return out;
    }

    // Only mono or the capture layout can be produced (see pcm_converter).
    int channels = config.channels == 1 ? 1 : capture.channels();

    if (config.encoding == encoding_t::encoding_opus) {
        if (channels > 2) {
            channels = 1; // multichannel Opus is not supported, fold down to mono
        }
        out.set_encoding(AudioFormat::ENCODING_OPUS);
        out.set_channels(channels);
        out.set_sample_rate(opus_packer::sample_rate);
        return out;
    }

    switch (config.encoding) {
    case encoding_t::encoding_f32:
        out.set_encoding(AudioFormat::ENCODING_PCM_FLOAT);
        break;
    case encoding_t::encoding_s8:
        out.set_encoding(AudioFormat::ENCODING_PCM_8BIT);
        break;
    case encoding_t::encoding_s16:
        out.set_encoding(AudioFormat::ENCODING_PCM_16BIT);
        break;
    case encoding_t::encoding_s24:
        out.set_encoding(AudioFormat::ENCODING_PCM_24BIT);
        break;
    case encoding_t::encoding_s32:
        out.set_encoding(AudioFormat::ENCODING_PCM_32BIT);
        break;
    default:
        out.set_encoding(capture.encoding());
        break;
    }
    out.set_channels(channels);
    out.set_sample_rate(config.sample_rate > 0 ? config.sample_rate : capture.sample_rate());
    return out;
}

// The capture format is only known once recording has started, so the
// pipeline is configured on the first packet.
void network_manager::setup_pipeline()
{
    using AudioFormat = audio_manager::AudioFormat;
    const AudioFormat capture = *_audio_manager->get_format();
    const AudioFormat wire = wire_format(capture, _capture_config);
    const auto capture_sample_format = to_sample_format(capture.encoding());

    if (capture_sample_format == silence_gate::sample_format::unknown) {
        spdlog::error("unsupported capture encoding {}, nothing will be sent", (int)capture.encoding());
    }
    if (_capture_config.channels > 1 && _capture_config.channels != capture.channels()) {
        spdlog::warn("only mono or the capture layout ({} channels) can be sent, ignoring --channels={}",
            capture.channels(), _capture_config.channels);
    }

    _converter.configure(capture_sample_format, capture.channels(), capture.sample_rate(), wire.channels(), wire.sample_rate());
    _gate.configure(_capture_config.gate, silence_gate::sample_format::f32, wire.sample_rate(), (int)sizeof(float) * wire.channels());

    _use_opus = wire.encoding() == AudioFormat::ENCODING_OPUS;
    if (_use_opus) {
        const int err = _opus.configure(wire.channels(), _capture_config.opus_bitrate);
        if (err != OPUS_OK) {
            spdlog::error("opus encoder: {}", opus_strerror(err));
        }
        spdlog::info("send format: opus, {} channel(s), {}Hz, {}kbps",
            wire.channels(), wire.sample_rate(), _capture_config.opus_bitrate / 1000);
    } else {
        _wire_sample_format = to_sample_format(wire.encoding());
        _wire_block_align = pcm_converter::bytes_per_sample(_wire_sample_format) * wire.channels();
        spdlog::info("send format: {}, {} channel(s), {}Hz, {}kbps",
            AudioFormat::Encoding_Name(wire.encoding()), wire.channels(), wire.sample_rate(),
            (int64_t)wire.sample_rate() * _wire_block_align * 8 / 1000);
    }

    if (!_capture_config.gate.enabled) {
        spdlog::info("silence gate: disabled");
    } else {
        spdlog::info("silence gate: threshold {}dBFS, hold {}ms, preroll {}ms",
            _capture_config.gate.threshold_db, _capture_config.gate.hold_ms, _capture_config.gate.preroll_ms);
    }
    _pipeline_ready = true;
}

void network_manager::broadcast_audio_data(const char* data, size_t count, int block_align, bool known_silent)
{
    if (count <= 0) {
        return;
    }
    // spdlog::trace("broadcast_audio_data count: {}", count);

    if (!_pipeline_ready) {
        setup_pipeline();
    }

    const auto& pcm = _converter.process((const uint8_t*)data, count, known_silent);
    if (pcm.empty()) {
        return;
    }

    const bool was_open = _gate.is_open();
    const auto& gated = _gate.process((const uint8_t*)pcm.data(), pcm.size() * sizeof(float));
    const bool opened = !was_open && _gate.is_open();
    const bool closed = was_open && !_gate.is_open();
    if (opened || closed) {
        spdlog::debug("silence gate: {}", opened ? "open (sending)" : "closed (idle)");
    }

    std::vector<float> samples(gated.size() / sizeof(float));
    std::memcpy(samples.data(), gated.data(), samples.size() * sizeof(float));

    if (_use_opus) {
        if (opened) {
            _opus.reset();
        }
        if (!samples.empty()) {
            send_packets(_opus.push(samples.data(), samples.size()));
        }
        if (closed) {
            send_packets(_opus.flush());
        }
        return;
    }

    if (samples.empty()) {
        return;
    }
    pcm_converter::encode(samples.data(), samples.size(), _wire_sample_format, _wire_bytes);
    send_audio_data(_wire_bytes.data(), _wire_bytes.size(), _wire_block_align);
}

void network_manager::send_packets(const opus_packer::packet_list& packets)
{
    if (packets.empty()) {
        return;
    }
    auto list = std::make_shared<opus_packer::packet_list>(packets);
    _ioc->post([list, self = shared_from_this()] {
        for (const auto& packet : *list) {
            for (auto& [peer, info] : self->_playing_peer_list) {
                self->_udp_server->async_send_to(asio::buffer(packet), info->udp_peer, [list](const asio::error_code& ec, std::size_t bytes_transferred) { });
            }
        }
    });
}

void network_manager::send_audio_data(const uint8_t* data, size_t count, int block_align)
{
    // divide udp frame
    constexpr int mtu = 1492;
    int max_seg_size = mtu - 20 - 8;
    max_seg_size -= max_seg_size % block_align; // one single sample can't be divided

    std::list<std::shared_ptr<std::vector<uint8_t>>> seg_list;

    for (int begin_pos = 0; begin_pos < count;) {
        const int real_seg_size = std::min((int)count - begin_pos, max_seg_size);
        auto seg = std::make_shared<std::vector<uint8_t>>(real_seg_size);
        std::copy((const uint8_t*)data + begin_pos, (const uint8_t*)data + begin_pos + real_seg_size, seg->begin());
        seg_list.push_back(seg);
        begin_pos += real_seg_size;
    }

    _ioc->post([seg_list = std::move(seg_list), self = shared_from_this()] {
        for (const auto& seg : seg_list) {
            for (auto& [peer, info] : self->_playing_peer_list) {
                self->_udp_server->async_send_to(asio::buffer(*seg), info->udp_peer, [seg](const asio::error_code& ec, std::size_t bytes_transferred) { });
            }
        }
    });
}
