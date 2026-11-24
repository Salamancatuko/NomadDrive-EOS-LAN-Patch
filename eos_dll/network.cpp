/*
 * Copyright (C) 2020 Nemirtingas
 * This file is part of the Nemirtingas's Epic Emulator
 *
 * The Nemirtingas's Epic Emulator is free software; you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 * 
 * The Nemirtingas's Epic Emulator is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the Nemirtingas's Epic Emulator; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "network.h"
#include "os_funcs.h"
#if defined(__WINDOWS__)
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <netdb.h>
#include <netinet/tcp.h>
#endif

using namespace PortableAPI;

static void configure_tcp_socket(PortableAPI::tcp_socket& sock, bool keepalive)
{
    auto native_sock = sock.get_native_socket();
    int nodelay = 1;
    ::setsockopt(native_sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    if (keepalive)
    {
        int ka = 1;
        ::setsockopt(native_sock, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&ka), sizeof(ka));
#if defined(__WINDOWS__)
        tcp_keepalive vals{};
        vals.onoff = 1;
        vals.keepalivetime = 10000;
        vals.keepaliveinterval = 2000;
        DWORD bytes_returned = 0;
        ::WSAIoctl(native_sock, SIO_KEEPALIVE_VALS, &vals, sizeof(vals), nullptr, 0, &bytes_returned, nullptr, nullptr);
#endif
    }
}

Network::Network():
    _discovery_port(default_network_port),
    _max_discovery_port(default_network_port + network_port_range),
    _advertise(false),
    _advertise_rate(2000),
    _tcp_port(0)
{
    uint16_t dp = Settings::Inst().discovery_port;
    if (dp >= 1024 && dp <= 65520)
    {
        _discovery_port = dp;
        _max_discovery_port = static_cast<uint16_t>(dp + network_port_range);
    }
    //APP_LOG(Log::LogLevel::DEBUG, "");
#if defined(NETWORK_COMPRESS)
    max_message_size = 0;
    max_compressed_message_size = 0;
    _zstd_ccontext = ZSTD_createCCtx();
    _zstd_dstream = ZSTD_createDStream();
#endif

    _network_task.run(&Network::network_thread, this);
}

Network::~Network()
{
#if defined(NETWORK_COMPRESS)
    APP_LOG(Log::LogLevel::DEBUG, "Shutting down Network, biggest message size was %llu, biggest compressed message size was %llu", max_message_size, max_compressed_message_size);
#else
    APP_LOG(Log::LogLevel::DEBUG, "Shutting down Network");
#endif

    _network_task.stop();
    if (is_dll_unloading())
    {
        _network_task.detach();
    }
    else
    {
        _network_task.join();
    }

#if defined(NETWORK_COMPRESS)
    ZSTD_freeCCtx(_zstd_ccontext);
    ZSTD_freeDStream(_zstd_dstream);
#endif

    //APP_LOG(Log::LogLevel::DEBUG, "Network Thread Joined");
}

#if defined(NETWORK_COMPRESS)

std::string Network::compress(void const* data, size_t len)
{
    std::string res(ZSTD_compressBound(len), '\0');
    res.resize(ZSTD_compressCCtx(_zstd_ccontext, &res[0], res.length(), data, len, ZSTD_CLEVEL_DEFAULT));
    return res;
}

std::string Network::decompress(void const* data, size_t len)
{
    static size_t decompress_block_size = ZSTD_DStreamOutSize();
    static std::string res;

    res.resize(decompress_block_size);
    ZSTD_inBuffer inbuff{ data, len, 0 };
    ZSTD_outBuffer outbuff{ const_cast<char*>(res.data()), res.length(), 0 };

    while (inbuff.pos < inbuff.size)
    {
        size_t x = 0;
        x = ZSTD_decompressStream(_zstd_dstream, &outbuff, &inbuff);
        if (ZSTD_isError(x))
        {
            if (x == size_t(-70))
            {
                res.resize(res.length() + decompress_block_size);
                outbuff.size = res.length();
                outbuff.dst = const_cast<char*>(res.data());
            }
            else
            {
                auto str_error = ZSTD_getErrorName(x);
                APP_LOG(Log::LogLevel::WARN, "Decompression error: %s", str_error);
                return std::string((char*)data, ((char*)data) + len);
            }
        }
    }

    ZSTD_initDStream(_zstd_dstream);
    res.resize(outbuff.pos);
    return res;
}

#endif

void Network::start_network()
{
    ipv4_addr addr;
    uint16_t port;
    addr.set_any_addr();

#if defined(__WINDOWS__)
    {
        BOOL bNewBehavior = FALSE;
        DWORD bytes_returned = 0;
        ::WSAIoctl(_udp_socket.get_native_socket(), SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), nullptr, 0, &bytes_returned, nullptr, nullptr);
    }
#endif

    for (port = _discovery_port; port < _max_discovery_port; ++port)
    {
        addr.set_port(port);
        try
        {
            _udp_socket.bind(addr);
            break;
        }
        catch (...)
        {
        }
    }
    if (port == _max_discovery_port)
    {
        APP_LOG(Log::LogLevel::ERR, "Failed to start udp socket in port range [%hu, %hu)", _discovery_port, _max_discovery_port);
        _network_task.stop();
    }
    else
    {
        APP_LOG(Log::LogLevel::INFO, "UDP socket started on port: %hu", port);
        int reuse = 1;
        ::setsockopt(_tcp_socket.get_native_socket(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        bool tcp_started = false;
        if (Settings::Inst().tcp_port != 0)
        {
            uint16_t fixed_port = Settings::Inst().tcp_port;
            addr.set_any_addr();
            addr.set_port(fixed_port);
            try
            {
                _tcp_socket.bind(addr);
                _tcp_socket.listen(32);
                addr.set_loopback_addr();
                _tcp_self_send.connect(addr);
                _tcp_self_recv.socket = std::move(_tcp_socket.accept());
                _tcp_self_recv.buffer.reserve(1024 * 10);
                configure_tcp_socket(_tcp_self_send, false);
                configure_tcp_socket(_tcp_self_recv.socket, false);
                _tcp_port = fixed_port;
                tcp_started = true;
                APP_LOG(Log::LogLevel::INFO, "TCP socket started on fixed port: %hu", fixed_port);
            }
            catch (std::exception& e)
            {
                APP_LOG(Log::LogLevel::WARN, "Failed to bind configured tcp_port %hu (%s), falling back to random port", fixed_port, e.what());
            }
        }

        if (!tcp_started)
        {
            std::uniform_int_distribution<int64_t> dis;
            std::mt19937_64& gen = get_gen();
            int x;
            for (x = 0, port = (dis(gen) % 30000 + 30000); x < 100; ++x, port = (dis(gen) % 30000 + 30000))
            {
                addr.set_any_addr();
                addr.set_port(port);
                try
                {
                    _tcp_socket.bind(addr);
                    _tcp_socket.listen(32);
                    addr.set_loopback_addr();
                    _tcp_self_send.connect(addr);
                    _tcp_self_recv.socket = std::move(_tcp_socket.accept());
                    _tcp_self_recv.buffer.reserve(1024 * 10);
                    configure_tcp_socket(_tcp_self_send, false);
                    configure_tcp_socket(_tcp_self_recv.socket, false);
                    break;
                }
                catch (...)
                {
                    APP_LOG(Log::LogLevel::WARN, "Failed to start tcp socket on port %hu", x);
                }
            }
            if (x == 100)
            {
                APP_LOG(Log::LogLevel::ERR, "Failed to start tcp socket");
                _udp_socket.close();
                _network_task.stop();
            }
            else
            {
                _tcp_port = port;
                APP_LOG(Log::LogLevel::INFO, "TCP socket started after %hu tries on port: %hu", x, port);
            }
        }
    }
}

void Network::stop_network()
{
    _advertise = false;
    _udp_socket.close();
    _tcp_socket.close();
    _tcp_clients.clear();
    _waiting_connect_tcp_clients.clear();
    _waiting_out_tcp_clients.clear();
    _waiting_in_tcp_clients.clear();
    _tcp_peers.clear();
    _peer_advertised_tcp_ports.clear();
    _network_msgs.clear();
    _udp_addrs.clear();
}

inline Network::next_packet_size_t Network::make_next_packet_size(std::string const& buff) const
{
    return utils::Endian::net_swap(next_packet_size_t(buff.length() - sizeof(next_packet_size_t)));
}

void Network::build_advertise_msg(Network_Message_pb& msg)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    Network_Advertise_pb* advertise = new Network_Advertise_pb;
    Network_Peer_pb* peer_pb = new Network_Peer_pb;

    APP_LOG(Log::LogLevel::DEBUG, "Advertising with peer ids: ");
    for (auto& id : _my_peer_ids)
    {
        APP_LOG(Log::LogLevel::DEBUG, "%s", id.c_str());
        peer_pb->add_peer_ids(id);
    }

    advertise->set_allocated_peer(peer_pb);
    msg.set_allocated_network_advertise(advertise);
    msg.set_source_id(*_my_peer_ids.begin());
}

std::pair<PortableAPI::tcp_socket*, std::vector<Network::peer_t>> Network::get_new_peer_ids(Network_Peer_pb const& peer_msg)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    std::pair<tcp_socket*, std::vector<peer_t>> peer_ids_to_add;
    peer_ids_to_add.first = nullptr;
    peer_ids_to_add.second.reserve(peer_msg.peer_ids_size());

    for (auto& peer_id : peer_msg.peer_ids())
    {
        auto it = _tcp_peers.find(peer_id);
        if (it == _tcp_peers.end())
        {
            peer_ids_to_add.second.emplace_back(peer_id);
        }
        else if (peer_ids_to_add.first == nullptr)
        {
            peer_ids_to_add.first = it->second;
        }
    }
    return peer_ids_to_add;
}

void Network::do_advertise()
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);
    if (!_advertise)
        return;

    auto now = std::chrono::steady_clock::now();
    if ((now - _last_advertise) < _advertise_rate)
        return;

    _last_advertise = now;
    
    try
    {
        if (!_my_peer_ids.empty())
        {
            Network_Message_pb msg;
            Network_Advertise_pb* network = new Network_Advertise_pb;
            Network_Port_pb* port = new Network_Port_pb;

            port->set_port(_tcp_port);
            network->set_allocated_port(port);
            msg.set_allocated_network_advertise(network);
            msg.set_source_id(*_my_peer_ids.begin());

            SendBroadcast(msg);
        }
    }
    catch (...)
    {
        //APP_LOG(Log::LogLevel::DEBUG, "Advertising, failed");
    }
}

void Network::set_advertise_rate(std::chrono::milliseconds rate)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);
    _advertise_rate = rate;
}

std::chrono::milliseconds Network::get_advertise_rate()
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);
    return _advertise_rate;
}

void Network::remove_orphaned_tcp_socket(PortableAPI::tcp_socket* cli)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);
    if (cli == nullptr)
        return;

    for (auto const& kv : _tcp_peers)
    {
        if (kv.second == cli)
            return;
    }

    for (auto it = _tcp_clients.begin(); it != _tcp_clients.end(); ++it)
    {
        if (&(it->socket) == cli)
        {
            auto native_sock = it->socket.get_native_socket();
            FD_CLR(native_sock, &readfds);
            FD_CLR(native_sock, &writefds);
            FD_CLR(native_sock, &exceptfds);
            APP_LOG(Log::LogLevel::INFO, "[NET] Removing orphaned TCP socket %s", it->socket.get_addr().to_string(true).c_str());
            _tcp_clients.erase(it);
            return;
        }
    }
}

void Network::add_new_tcp_client(PortableAPI::tcp_socket* cli, std::vector<peer_t> const& peer_ids, bool advertise_peer)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    FD_SET(cli->get_native_socket(), &readfds);

    FD_SET(cli->get_native_socket(), &exceptfds);

    Network_Message_pb msg;
    Network_Advertise_pb adv;
    Network_Peer_Connect_pb conn;

    adv.set_allocated_peer_connect(&conn);
    msg.set_allocated_network_advertise(&adv);

    std::vector<PortableAPI::tcp_socket*> replaced_sockets;

    for (auto& peerid : peer_ids)
    {// Map all clients peerids to the socket
        auto existing_it = _tcp_peers.find(peerid);
        if (existing_it != _tcp_peers.end())
        {
            if (existing_it->second != cli)
            {
                PortableAPI::tcp_socket* old_cli = existing_it->second;
                APP_LOG(Log::LogLevel::INFO, "[NET] peer %s reconnected from %s: rebinding to the new socket (was port %d)",
                    peerid.c_str(), cli->get_addr().to_string(true).c_str(),
                    old_cli != nullptr ? old_cli->get_addr().get_port() : 0);

                if (old_cli != nullptr &&
                    std::find(replaced_sockets.begin(), replaced_sockets.end(), old_cli) == replaced_sockets.end())
                {
                    replaced_sockets.emplace_back(old_cli);
                }

                _peer_advertised_tcp_ports.erase(peerid);
                _udp_addrs.erase(peerid);
                existing_it->second = cli;
            }
        }
        else
        {
            APP_LOG(Log::LogLevel::DEBUG, "Adding peer id %s to client %s", peerid.c_str(), cli->get_addr().to_string(true).c_str());
            _tcp_peers[peerid] = cli;
        }

        _waiting_connect_tcp_clients.erase(peerid);

        msg.set_source_id(peerid);

        {
            std::lock_guard<std::mutex> msg_lk(message_mutex);
            for (auto& channel : _default_channels)
            {
                _pending_network_msgs[channel.second].emplace_back(msg);
            }
        }
    }

    adv.release_peer_connect();
    msg.release_network_advertise();

    for (auto* old_cli : replaced_sockets)
    {
        remove_orphaned_tcp_socket(old_cli);
    }
    
    if(advertise_peer)
    {
        APP_LOG(Log::LogLevel::DEBUG, "New peer: id %s %s", (*peer_ids.begin()).c_str(), cli->get_addr().to_string(true).c_str());

        Network_Message_pb msg;
        Network_Advertise_pb* adv = new Network_Advertise_pb;
        Network_Peer_Accept_pb* accept_peer = new Network_Peer_Accept_pb;

        adv->set_allocated_accept(accept_peer);
        msg.set_allocated_network_advertise(adv);

        std::string buff(sizeof(next_packet_size_t), 0);
        // Don't compress the accept message, its only 4 bytes long
    //#if defined(NETWORK_COMPRESS)
    //    std::string data;
    //    msg.SerializeToString(&data);
    //    buff += std::move(compress(data.data(), data.length()));
    //    
    //    max_message_size = std::max<uint64_t>(max_message_size, data.length());
    //    max_compressed_message_size = std::max<uint64_t>(max_compressed_message_size, buff.length());
    //#else
        buff += std::move(msg.SerializeAsString());
    //#endif

        *reinterpret_cast<next_packet_size_t*>(&buff[0]) = make_next_packet_size(buff);

        cli->send(buff.data(), buff.length());
    }
}

void Network::remove_tcp_peer(tcp_buffer_t& tcp_buffer)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    APP_LOG(Log::LogLevel::DEBUG, "TCP Client %s gone", tcp_buffer.socket.get_addr().to_string().c_str());

    auto native_sock = tcp_buffer.socket.get_native_socket();
    FD_CLR(native_sock, &readfds);
    FD_CLR(native_sock, &writefds);
    FD_CLR(native_sock, &exceptfds);

    // Remove the peer mappings

    Network_Message_pb msg;
    Network_Advertise_pb adv;
    Network_Peer_Disconnect_pb disc;

    adv.set_allocated_peer_disconnect(&disc);
    msg.set_allocated_network_advertise(&adv);

    for (auto it = _tcp_peers.begin(); it != _tcp_peers.end();)
    {
        if (it->second == &(tcp_buffer.socket))
        {
            const peer_t disconnected_peer = it->first;
            msg.set_source_id(disconnected_peer);
            it = _tcp_peers.erase(it);

            _waiting_out_tcp_clients.erase(disconnected_peer);
            _waiting_connect_tcp_clients.erase(disconnected_peer);
            _peer_advertised_tcp_ports.erase(disconnected_peer);
            _udp_addrs.erase(disconnected_peer);

            {
                std::lock_guard<std::mutex> msg_lk(message_mutex);
                for (auto& channel : _default_channels)
                {
                    _pending_network_msgs[channel.second].emplace_back(msg);
                }
            }
        }
        else
            ++it;
    }

    (void)adv.release_peer_disconnect();
    (void)msg.release_network_advertise();
}

void Network::connect_to_peer(ipv4_addr &addr, peer_t const& peer_id)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    auto out_it = _waiting_out_tcp_clients.find(peer_id);
    if (out_it != _waiting_out_tcp_clients.end())
    {
        auto const& existing_addr = out_it->second.socket.get_addr();
        if (existing_addr.get_port() == addr.get_port())
        {
            APP_LOG(Log::LogLevel::DEBUG, "connect_to_peer: skipped %s, already pending", peer_id.c_str());
            return;
        }
        APP_LOG(Log::LogLevel::INFO, "[NET] connect_to_peer: resetting stale waiting_out for %s (%s -> %s)",
            peer_id.c_str(), existing_addr.to_string(true).c_str(), addr.to_string(true).c_str());
        _waiting_out_tcp_clients.erase(out_it);
    }

    auto now = std::chrono::steady_clock::now();
    bool connected = false;
    auto it = _waiting_connect_tcp_clients.find(peer_id);
    if (it != _waiting_connect_tcp_clients.end() &&
        ((now - it->second.created_time) > std::chrono::seconds(5) ||
         it->second.target_addr.get_port() != addr.get_port()))
    {
        APP_LOG(Log::LogLevel::DEBUG, "connect_to_peer: resetting pending connect for %s (%s -> %s)",
            peer_id.c_str(), it->second.target_addr.to_string(true).c_str(), addr.to_string(true).c_str());
        _waiting_connect_tcp_clients.erase(it);
        it = _waiting_connect_tcp_clients.end();
    }

    try
    {
        if (it == _waiting_connect_tcp_clients.end())
        {
            APP_LOG(Log::LogLevel::DEBUG, "Connecting to %s : %s", addr.to_string(true).c_str(), peer_id.c_str());
            
            pending_connect_t pending{};
            pending.target_addr = addr;
            pending.created_time = now;
            pending.socket.set_nonblocking(true);
            it = _waiting_connect_tcp_clients.emplace(peer_id, std::move(pending)).first;
        }
        it->second.socket.connect(addr);
        connected = true;
    }
    catch (is_connected &e)
    {
        connected = true;
    }
    catch (would_block &e)
    {}
    catch(in_progress &e)
    {}
    catch (std::exception &e)
    {
        _waiting_connect_tcp_clients.erase(it);
        APP_LOG(Log::LogLevel::WARN, "Failed to TCP connect to %s: %s", addr.to_string().c_str(), e.what());
    }

    if (connected)
    {
        finish_outgoing_tcp_connect(peer_id, it->second);
        _waiting_connect_tcp_clients.erase(it);
    }
}

bool Network::finish_outgoing_tcp_connect(peer_t const& peer_id, pending_connect_t& pending)
{
    Network_Message_pb msg;
    build_advertise_msg(msg);

    std::string buff(sizeof(next_packet_size_t), 0);

#if defined(NETWORK_COMPRESS)
    std::string data;
    (void)msg.SerializeToString(&data);
    buff += std::move(compress(data.data(), data.length()));

    max_message_size = std::max<uint64_t>(max_message_size, data.length());
    max_compressed_message_size = std::max<uint64_t>(max_compressed_message_size, buff.length());
#else
    buff += std::move(msg.SerializeAsString());
#endif
    *reinterpret_cast<next_packet_size_t*>(&buff[0]) = make_next_packet_size(buff);

    try
    {
        configure_tcp_socket(pending.socket, true);
        pending.socket.send(buff.data(), buff.length());

        APP_LOG(Log::LogLevel::DEBUG, "Connected to %s : %s", pending.socket.get_addr().to_string(true).c_str(), peer_id.c_str());

        tcp_buffer_t tcp_buffer{};
        tcp_buffer.socket = std::move(pending.socket);
        tcp_buffer.created_time = std::chrono::steady_clock::now();
        _waiting_out_tcp_clients.emplace(peer_id, std::move(tcp_buffer));
        return true;
    }
    catch (std::exception& e)
    {
        APP_LOG(Log::LogLevel::WARN, "Failed sending advertise on connect to %s: %s", peer_id.c_str(), e.what());
        return false;
    }
}

void Network::process_waiting_out_clients()
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    auto now = std::chrono::steady_clock::now();
    for (auto cit = _waiting_connect_tcp_clients.begin(); cit != _waiting_connect_tcp_clients.end(); )
    {
        if ((now - cit->second.created_time) > std::chrono::seconds(5))
        {
            APP_LOG(Log::LogLevel::DEBUG, "Pending TCP connect to %s timed out, cleaning up", cit->first.c_str());
            cit = _waiting_connect_tcp_clients.erase(cit);
            continue;
        }

        fd_set wfds, efds;
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        auto nsock = cit->second.socket.get_native_socket();
        FD_SET(nsock, &wfds);
        FD_SET(nsock, &efds);
        timeval zero_tv{0, 0};
        int sel = ::select(static_cast<int>(nsock) + 1, nullptr, &wfds, &efds, &zero_tv);
        if (sel > 0)
        {
            if (FD_ISSET(nsock, &efds))
            {
                APP_LOG(Log::LogLevel::DEBUG, "Pending TCP connect to %s failed, cleaning up", cit->first.c_str());
                cit = _waiting_connect_tcp_clients.erase(cit);
                continue;
            }
            if (FD_ISSET(nsock, &wfds))
            {
                int so_err = 0;
                socklen_t len = sizeof(so_err);
                ::getsockopt(nsock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &len);
                if (so_err == 0)
                {
                    finish_outgoing_tcp_connect(cit->first, cit->second);
                }
                else
                {
                    APP_LOG(Log::LogLevel::DEBUG, "Pending TCP connect to %s failed (SO_ERROR=%d), cleaning up", cit->first.c_str(), so_err);
                }
                cit = _waiting_connect_tcp_clients.erase(cit);
                continue;
            }
        }
        ++cit;
    }

    if (_waiting_out_tcp_clients.empty())
        return;

    Network_Message_pb msg;
    for (auto it = _waiting_out_tcp_clients.begin(); it != _waiting_out_tcp_clients.end(); )
    {
        try
        {
            if ((now - it->second.created_time) > std::chrono::seconds(5))
            {
                APP_LOG(Log::LogLevel::WARN, "Waiting out TCP peer %s handshake timed out, removing", it->first.c_str());
                it = _waiting_out_tcp_clients.erase(it);
                continue;
            }

            unsigned long count = 0;
            it->second.socket.ioctlsocket(Socket::cmd_name::fionread, &count);
            if (count == 0)
            {
                char tmp = 0;
                int peek_res = ::recv(it->second.socket.get_native_socket(), &tmp, 1, MSG_PEEK);
                if (peek_res == 0 || (peek_res == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK))
                {
                    APP_LOG(Log::LogLevel::DEBUG, "Waiting out TCP peer %s closed during handshake, removing", it->first.c_str());
                    it = _waiting_out_tcp_clients.erase(it);
                    continue;
                }
            }

            if (count > 0)
            {
                if (it->second.next_packet_size == 0 && count > sizeof(next_packet_size_t))
                {
                    it->second.socket.recv(&it->second.next_packet_size, sizeof(next_packet_size_t));
                    it->second.next_packet_size = utils::Endian::net_swap(it->second.next_packet_size);
                    count -= sizeof(next_packet_size_t);
                }
                if (it->second.next_packet_size > 0 && count >= it->second.next_packet_size)
                {
                    it->second.buffer.resize(it->second.next_packet_size);
                    it->second.socket.recv(it->second.buffer.data(), it->second.next_packet_size);

                    const void* message;
                    int message_size;

                    // Don't compress the accept message, its only 4 bytes long
                    message = it->second.buffer.data();
                    message_size = it->second.next_packet_size;
                    
                    if (msg.ParseFromArray(message, message_size) &&
                        msg.has_network_advertise() && 
                        msg.network_advertise().has_accept())
                    {
                        if (_tcp_peers.find(it->first) != _tcp_peers.end())
                        {
                            APP_LOG(Log::LogLevel::INFO, "[NET] peer %s already connected, dropping duplicate outgoing TCP socket %s",
                                it->first.c_str(), it->second.socket.get_addr().to_string(true).c_str());
                        }
                        else
                        {
                            it->second.next_packet_size = 0;
                            it->second.buffer.clear();
                            it->second.socket.set_nonblocking(false);

                            _tcp_clients.emplace_back(std::move(it->second));
                            add_new_tcp_client(&(_tcp_clients.rbegin()->socket), std::vector<peer_t>{it->first}, false);
                        }
                    }
                    it = _waiting_out_tcp_clients.erase(it);
                    continue;
                }
            }
            
            ++it;
        }
        catch (std::exception &e)
        {
            // Error while reading, connection closed ?
            APP_LOG(Log::LogLevel::WARN, "Failed peer pair: %s", e.what());
            it = _waiting_out_tcp_clients.erase(it);
        }
    }
}

void Network::process_waiting_in_client()
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    if (_waiting_in_tcp_clients.empty())
        return;

    auto now = std::chrono::steady_clock::now();
    Network_Message_pb msg;
    for (auto it = _waiting_in_tcp_clients.begin(); it != _waiting_in_tcp_clients.end(); )
    {
        try
        {
            if ((now - it->created_time) > std::chrono::seconds(5))
            {
                APP_LOG(Log::LogLevel::WARN, "Waiting in TCP client handshake timed out, removing");
                it = _waiting_in_tcp_clients.erase(it);
                continue;
            }

            unsigned long count = 0;
            it->socket.ioctlsocket(Socket::cmd_name::fionread, &count);
            if (count == 0)
            {
                char tmp = 0;
                int peek_res = ::recv(it->socket.get_native_socket(), &tmp, 1, MSG_PEEK);
                if (peek_res == 0 || (peek_res == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK))
                {
                    APP_LOG(Log::LogLevel::DEBUG, "Waiting in TCP client closed during handshake, removing");
                    it = _waiting_in_tcp_clients.erase(it);
                    continue;
                }
            }

            if (count > 0)
            {
                if (it->next_packet_size == 0 && count > sizeof(next_packet_size_t))
                {
                    it->socket.recv(&it->next_packet_size, sizeof(next_packet_size_t));
                    it->next_packet_size = utils::Endian::net_swap(it->next_packet_size);
                    count -= sizeof(next_packet_size_t);
                }
                if (it->next_packet_size > 0 && count >= it->next_packet_size)
                {
                    it->buffer.resize(it->next_packet_size);
                    it->socket.recv(it->buffer.data(), it->next_packet_size);

                    const void* message;
                    int message_size;

                #if defined(NETWORK_COMPRESS)
                    std::string buff = std::move(decompress(it->buffer.data(), it->next_packet_size));
                    message = buff.data();
                    message_size = buff.length();
                #else
                    message = it->buffer.data();
                    message_size = it->next_packet_size;
                #endif
                    
                    if (msg.ParseFromArray(message, message_size) &&
                        msg.has_network_advertise() && 
                        msg.network_advertise().has_peer())
                    {
                        it->next_packet_size = 0;
                        it->buffer.clear();
                        it->socket.set_nonblocking(false);

                        auto const& peer_msg = msg.network_advertise().peer();
                        std::vector<peer_t> peer_ids_to_bind;
                        peer_ids_to_bind.reserve(peer_msg.peer_ids_size());
                        bool has_existing_peer = false;
                        for (auto const& pid : peer_msg.peer_ids())
                        {
                            if (_my_peer_ids.count(pid) != 0)
                                continue;
                            peer_ids_to_bind.emplace_back(pid);
                            if (_tcp_peers.find(pid) != _tcp_peers.end())
                                has_existing_peer = true;
                        }

                        if (!peer_ids_to_bind.empty())
                        {// Bind (or rebind) all advertised remote peer IDs to this new incoming socket
                            _tcp_clients.emplace_back(std::move(*it));
                            tcp_socket* cli = &(_tcp_clients.rbegin()->socket);
                            if (has_existing_peer)
                            {
                                APP_LOG(Log::LogLevel::INFO, "[NET] Accepting reconnected peer connection from %s (%zu peer IDs)",
                                    cli->get_addr().to_string(true).c_str(), peer_ids_to_bind.size());
                            }
                            for (auto const& pid : peer_ids_to_bind)
                            {
                                _waiting_out_tcp_clients.erase(pid);
                            }
                            add_new_tcp_client(cli, peer_ids_to_bind, true);
                        }
                    }
                    it = _waiting_in_tcp_clients.erase(it);
                    continue;
                }
            }
            
            ++it;
        }
        catch (std::exception &e)
        {
            // Error while reading, connection closed ?
            APP_LOG(Log::LogLevel::WARN, "Failed peer pair: %s", e.what());
            it = _waiting_in_tcp_clients.erase(it);
        }
    }
}

void Network::process_network_message(Network_Message_pb &msg)
{
    if (msg.messages_case() == Network_Message_pb::MessagesCase::kP2P &&
        msg.p2p().message_case() == P2P_Message_pb::MessageCase::kDataMessage)
    {
        std::lock_guard<std::recursive_mutex> lk(local_mutex);
        if (_immediate_p2p_handler && _immediate_p2p_handler(msg))
            return;
    }

    std::lock_guard<std::mutex> lk(message_mutex);

    std::chrono::system_clock::time_point msg_time(std::chrono::milliseconds(msg.timestamp()));
    
    //if ((std::chrono::system_clock::now() - msg_time) > std::chrono::milliseconds(1500))
    //{
    //    APP_LOG(Log::LogLevel::WARN, "Message dropped because it was too old");
    //    return;
    //}

    if (msg.dest_id() == peer_t())
    {// If we received a message without a destination, then its a broadcast.
        // Add the message to all listeners queue
        for (auto& channel : _default_channels)
            _pending_network_msgs[channel.second].emplace_back(msg);
    }
    else
    {
        assert(_default_channels.find(msg.dest_id()) != _default_channels.end());
        _pending_network_msgs[_default_channels[msg.dest_id()]].emplace_back(std::move(msg));
    }
}

void Network::process_udp()
{
    try
    {
        ipv4_addr addr;
        std::array<uint8_t, 4096> buffer;
        Network_Message_pb msg;
        size_t len;
        
        len = _udp_socket.recvfrom(addr, buffer.data(), buffer.size());
        if (len > 0)
        {
            const void* message;
            int message_size;

            #if defined(NETWORK_COMPRESS)
                std::string buff(std::move(decompress(buffer.data(), len)));
                message = buff.data();
                message_size = buff.length();
            #else
                message = buffer.data();
                message_size = len;
            #endif

            if (msg.ParseFromArray(message, message_size))
            {
                if (msg.source_id() != peer_t())
                {
                    std::lock_guard<std::recursive_mutex> lk(local_mutex);
                    auto prev_udp = _udp_addrs.find(msg.source_id());
                    bool udp_peer_new = (prev_udp == _udp_addrs.end() || prev_udp->second.get_port() != addr.get_port());
                    _udp_addrs[msg.source_id()] = addr;

                    //APP_LOG(Log::LogLevel::TRACE, "Received UDP message from: %s - %s", addr.to_string().c_str(), msg.source_id().c_str());
                    if (msg.has_network_advertise())
                    {
                        if (_advertise)
                        {
                            auto const& advertise = msg.network_advertise();
                            if (advertise.has_port())
                            {
                                if (!_my_peer_ids.empty() && _my_peer_ids.count(msg.source_id()) == 0)
                                {
                                    uint16_t adv_port = advertise.port().port();
                                    bool port_changed = false;
                                    auto port_it = _peer_advertised_tcp_ports.find(msg.source_id());
                                    if (port_it != _peer_advertised_tcp_ports.end() && port_it->second != adv_port)
                                    {
                                        port_changed = true;
                                        APP_LOG(Log::LogLevel::INFO, "[NET] peer %s changed advertised TCP port (%u -> %u), closing stale connection",
                                            msg.source_id().c_str(), port_it->second, adv_port);
                                        auto peer_it = _tcp_peers.find(msg.source_id());
                                        if (peer_it != _tcp_peers.end())
                                        {
                                            PortableAPI::tcp_socket* stale_cli = peer_it->second;
                                            for (auto client_it = _tcp_clients.begin(); client_it != _tcp_clients.end(); ++client_it)
                                            {
                                                if (&(client_it->socket) == stale_cli)
                                                {
                                                    remove_tcp_peer(*client_it);
                                                    _tcp_clients.erase(client_it);
                                                    break;
                                                }
                                            }
                                        }
                                        _waiting_out_tcp_clients.erase(msg.source_id());
                                        _waiting_connect_tcp_clients.erase(msg.source_id());
                                        _udp_addrs[msg.source_id()] = addr;
                                    }
                                    _peer_advertised_tcp_ports[msg.source_id()] = adv_port;

                                    if (udp_peer_new || port_changed || _tcp_peers.count(msg.source_id()) == 0)
                                    {
                                        Network_Message_pb reply;
                                        Network_Advertise_pb* adv_reply = new Network_Advertise_pb;
                                        Network_Port_pb* port_reply = new Network_Port_pb;
                                        port_reply->set_port(_tcp_port);
                                        adv_reply->set_allocated_port(port_reply);
                                        reply.set_allocated_network_advertise(adv_reply);
                                        reply.set_source_id(*_my_peer_ids.begin());
                                        reply.set_dest_id(msg.source_id());
                                        UDPSendTo(reply);
                                    }

                                    if (_tcp_peers.count(msg.source_id()) == 0)
                                    {
                                        ipv4_addr peer_addr;
                                        peer_addr.set_ip(addr.get_ip());
                                        peer_addr.set_port(adv_port);
                                        connect_to_peer(peer_addr, msg.source_id());
                                    }
                                }
                                else if (!_my_peer_ids.empty())
                                {
                                    uint32_t src_ip = addr.get_ip();
                                    bool is_self = ((src_ip & 0xFF000000u) == 0x7F000000u);
                                    if (!is_self)
                                    {
                                        for (auto const& iface : get_ifaces_ip())
                                        {
                                            if (iface.ip == src_ip)
                                            {
                                                is_self = true;
                                                break;
                                            }
                                        }
                                    }
                                    if (!is_self)
                                    {
                                        auto now = std::chrono::steady_clock::now();
                                        auto& last_warn = _dup_id_warn_times[msg.source_id()];
                                        if (last_warn == std::chrono::steady_clock::time_point{} ||
                                            (now - last_warn) > std::chrono::seconds(10))
                                        {
                                            last_warn = now;
                                            APP_LOG(Log::LogLevel::WARN,
                                                "[NET] Duplicate peer/epicid '%s' detected from %s! Each PC must leave 'epicid' & 'productuserid' blank or use distinct IDs.",
                                                msg.source_id().c_str(), addr.to_string(true).c_str());
                                        }
                                    }
                                }
                            }
                            else if (advertise.has_peer())
                            {
                                std::pair<tcp_socket*, std::vector<peer_t>> peer_ids_to_add = std::move(get_new_peer_ids(advertise.peer()));

                                if (peer_ids_to_add.first != nullptr && !peer_ids_to_add.second.empty())
                                {// We have peer ids to add
                                    add_new_tcp_client(peer_ids_to_add.first, peer_ids_to_add.second, false);
                                }
                            }
                        }
                    }
                    else
                    {
                        //APP_LOG(Log::LogLevel::DEBUG, "Received UDP message from %s type %d", addr.to_string(true).c_str(), msg.messages_case());
                        process_network_message(msg);
                    }
                }
                else
                {
                    APP_LOG(Log::LogLevel::DEBUG, "Dropping UDP data: peer_id is null");
                }
            }
            else
            {
                APP_LOG(Log::LogLevel::DEBUG, "Dropping UDP data: failed to pase protobuf");
            }
        }
    }
    catch (socket_exception & e)
    {
        //APP_LOG(Log::LogLevel::WARN, "Udp socket exception: %s", e.what());
    }
}

void Network::process_tcp_listen()
{
    try
    {
        tcp_buffer_t tcp_buff({});
        tcp_buff.socket = std::move(_tcp_socket.accept());
        tcp_buff.socket.set_nonblocking(true);
        configure_tcp_socket(tcp_buff.socket, true);
        tcp_buff.created_time = std::chrono::steady_clock::now();
        _waiting_in_tcp_clients.emplace_back(std::move(tcp_buff));
    }
    catch (socket_exception & e)
    {
        APP_LOG(Log::LogLevel::WARN, "TCP Listen exception: %s", e.what());
    }
}

bool Network::process_tcp_data(tcp_buffer_t& tcp_buffer)
{
    // Don't lock here, its already locked in network_thread when needed
    Network_Message_pb msg;
    size_t len;

    try
    {
        unsigned long count = 0;
        tcp_buffer.socket.ioctlsocket(Socket::cmd_name::fionread, &count);
        if (count == 0)
        {
            char tmp = 0;
            int peek_res = ::recv(tcp_buffer.socket.get_native_socket(), &tmp, 1, MSG_PEEK);
            if (peek_res == 0)
                return false;
            if (peek_res == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
                return false;
            return true;
        }

        if (count > 0)
        {
            size_t buff_len = tcp_buffer.buffer.size();
            tcp_buffer.buffer.resize(buff_len + count); // We grow to the current size + stream size

            len = tcp_buffer.socket.recv(tcp_buffer.buffer.data() + buff_len, count);
            if (len == 0)
                return false;
            if (len < count)
                tcp_buffer.buffer.resize(buff_len + len);

            while(tcp_buffer.buffer.size() > 0)
            {
                if (tcp_buffer.next_packet_size == 0 && tcp_buffer.buffer.size() >= sizeof(next_packet_size_t))
                {
                    tcp_buffer.next_packet_size = *reinterpret_cast<next_packet_size_t*>(&tcp_buffer.buffer[0]);
                    tcp_buffer.next_packet_size = utils::Endian::net_swap(tcp_buffer.next_packet_size);
                    tcp_buffer.buffer.erase(tcp_buffer.buffer.begin(), tcp_buffer.buffer.begin() + sizeof(tcp_buffer.next_packet_size));
                }

                if (tcp_buffer.next_packet_size > 0 && tcp_buffer.buffer.size() >= tcp_buffer.next_packet_size)
                {
                    const void* message;
                    int message_size;
                #if defined(NETWORK_COMPRESS)
                    std::string buff = std::move(decompress(tcp_buffer.buffer.data(), tcp_buffer.next_packet_size));
                    message = buff.data();
                    message_size = buff.length();
                #else
                    message = tcp_buffer.buffer.data();
                    message_size = tcp_buffer.next_packet_size;
                #endif

                    if (msg.ParseFromArray(message, message_size))
                    {
                        //APP_LOG(Log::LogLevel::DEBUG, "Received TCP message from %s type %d", tcp_buffer.socket.get_addr().to_string(true).c_str(), msg.messages_case());
                        process_network_message(msg);
                    }
                    tcp_buffer.buffer.erase(tcp_buffer.buffer.begin(), tcp_buffer.buffer.begin() + tcp_buffer.next_packet_size);
                    tcp_buffer.next_packet_size = 0;
                }
                else
                {
                    break;
                }
            }
        }
    }
    catch (std::exception& e)
    {
        APP_LOG(Log::LogLevel::DEBUG, "process_tcp_data exception: %s", e.what());
        return false;
    }

    return true;
}

void Network::rebuild_fd_sets()
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_ZERO(&exceptfds);

    FD_SET(_udp_socket.get_native_socket(), &readfds);
    FD_SET(_tcp_socket.get_native_socket(), &readfds);
    FD_SET(_tcp_self_recv.socket.get_native_socket(), &readfds);

    FD_SET(_udp_socket.get_native_socket(), &exceptfds);
    FD_SET(_tcp_socket.get_native_socket(), &exceptfds);
    FD_SET(_tcp_self_recv.socket.get_native_socket(), &exceptfds);

    for (auto const& cli : _tcp_clients)
    {
        auto s = cli.socket.get_native_socket();
        FD_SET(s, &readfds);
        FD_SET(s, &exceptfds);
    }
}

void Network::network_thread()
{
    int broadcast = 1;

    start_network();
    if (_network_task.want_stop())
        return;

    _udp_socket.setsockopt(Socket::level::sol_socket, Socket::option_name::so_broadcast, &broadcast, sizeof(broadcast));
    //_udp_socket.set_nonblocking();

    rebuild_fd_sets();

    int consecutive_select_errors = 0;

    while (!_network_task.want_stop())
    {
        do_advertise();

        bool handshake_active = false;
        {
            std::lock_guard<std::recursive_mutex> lk(local_mutex);
            handshake_active = !_waiting_connect_tcp_clients.empty() ||
                               !_waiting_out_tcp_clients.empty() ||
                               !_waiting_in_tcp_clients.empty();
        }
        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = handshake_active ? 50000 : 200000;

        fd_set readfds_copy = readfds;
        fd_set writefds_copy = writefds;
        fd_set exceptfds_copy = exceptfds;

        int res = select(0, &readfds_copy, &writefds_copy, &exceptfds_copy, &timeout);
        if (res == SOCKET_ERROR) {
            int wsa_err = WSAGetLastError();
            APP_LOG(Log::LogLevel::WARN, "network_thread: select() failed (WSA error %d), rebuilding fd sets", wsa_err);
            if (++consecutive_select_errors > 50)
            {
                APP_LOG(Log::LogLevel::ERR, "network_thread: too many select() failures, stopping network");
                break;
            }
            rebuild_fd_sets();
            continue;
        }
        consecutive_select_errors = 0;

        if (res > 0)
        {
            if (FD_ISSET(_udp_socket.get_native_socket(), &readfds_copy)) {
                process_udp();
            }

            if (FD_ISSET(_tcp_socket.get_native_socket(), &readfds_copy)) {
                process_tcp_listen();
            }
            
            if (FD_ISSET(_tcp_self_recv.socket.get_native_socket(), &readfds_copy))
            {
                try
                {
                    process_tcp_data(_tcp_self_recv); // Process our TCP message, we are not considered as a classic client as we have 2 sockets for the same peer id
                }
                catch (...)
                {
                    assert(0 == 1 && "The local socket should not fail");
                }
            }
            
            {
                std::lock_guard<std::recursive_mutex> lk(local_mutex);
                for (auto it = _tcp_clients.begin(); it != _tcp_clients.end();)
                {// Process the multiple tcp clients we have
                    if (FD_ISSET(it->socket.get_native_socket(), &readfds_copy)) {
                        if (!process_tcp_data(*it)) {
                            remove_tcp_peer(*it);
                            it = _tcp_clients.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    else if (FD_ISSET(it->socket.get_native_socket(), &exceptfds_copy)) {
                        remove_tcp_peer(*it);
                        it = _tcp_clients.erase(it);
                    }
                    else
                        ++it;
                }
            }
        }
        
        process_waiting_in_client();
        // We might have found a peer while he didn't find us yet, so begin the connection procedure
        process_waiting_out_clients();
    }
}

void Network::advertise_peer_id(peer_t const& peerid)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    _my_peer_ids.insert(peerid);
    _tcp_peers[peerid] = &_tcp_self_send;
}

void Network::remove_advertise_peer_id(peer_t const& peerid)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    _my_peer_ids.erase(peerid);
    _tcp_peers.erase(peerid);
}

void Network::advertise(bool doit)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);
    _advertise = doit;
}

bool Network::is_advertising()
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    return _advertise;
}

void Network::set_default_channel(peer_t peerid, channel_t default_channel)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    _default_channels[peerid] = default_channel;
}

void Network::register_listener(IRunNetwork* listener, channel_t channel, Network_Message_pb::MessagesCase type)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    _network_listeners[type][channel].push_back(listener);
}

void Network::unregister_listener(IRunNetwork* listener, channel_t channel, Network_Message_pb::MessagesCase type)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    auto& listeners = _network_listeners[type][channel];
    listeners.erase(
        std::remove(listeners.begin(), listeners.end(), listener),
        listeners.end());
}

void Network::set_immediate_p2p_handler(std::function<bool(Network_Message_pb const&)> handler)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);
    _immediate_p2p_handler = std::move(handler);
}

bool Network::CBRunFrame(channel_t channel, Network_Message_pb::MessagesCase MessageFilter)
{
    bool rerun = false;
    auto& channel_messages = _network_msgs[channel];
    {
        std::lock_guard<std::mutex> lk(message_mutex);

        auto& pending_channel_messages = _pending_network_msgs[channel];
        if (!pending_channel_messages.empty())
        {
            std::move(pending_channel_messages.begin(), pending_channel_messages.end(), std::back_inserter(channel_messages));
            pending_channel_messages.clear();
        }
    }

    {
        std::lock_guard<std::recursive_mutex> lk(local_mutex);
        for (auto it = channel_messages.begin(); it != channel_messages.end(); )
        {
            auto msg_case = it->messages_case();
            if (msg_case != Network_Message_pb::MessagesCase::MESSAGES_NOT_SET)
            {
                if (MessageFilter == Network_Message_pb::MessagesCase::MESSAGES_NOT_SET || MessageFilter == msg_case)
                {
                    auto& listeners = _network_listeners[msg_case][channel];
                    for (auto& item : listeners)
                        item->RunNetwork(*it);

                    it = channel_messages.erase(it);

                    rerun = true;
                }
                else
                {
                    ++it;
                }
            }
            else
            {// Don't care about invalid message
                it = channel_messages.erase(it);
            }
        }
    }

    return rerun;
}

static bool parse_numeric_ipv4(std::string const& str, uint32_t& out_ip)
{
    if (str.empty() || str.find(':') != std::string::npos)
        return false;
    PortableAPI::ipv4_addr addr;
    if (!addr.from_string(str))
        return false;
    out_ip = addr.get_ip();
    return true;
}

static bool parse_uint_strict(std::string const& s, uint32_t max_val, uint32_t& out_val)
{
    if (s.empty())
        return false;
    uint64_t val = 0;
    for (unsigned char ch : s)
    {
        if (ch < '0' || ch > '9')
            return false;
        val = val * 10 + (ch - '0');
        if (val > max_val)
            return false;
    }
    out_val = static_cast<uint32_t>(val);
    return true;
}

static std::string ipv4_to_str(uint32_t ip)
{
    PortableAPI::ipv4_addr a;
    a.set_ip(ip);
    return a.to_string(false);
}

bool Network::resolve_ipv4_host(std::string const& host, uint32_t& out_ip)
{
    if (parse_numeric_ipv4(host, out_ip))
        return true;

    if (host.empty())
        return false;

    auto now = std::chrono::steady_clock::now();
    auto it = _dns_cache.find(host);
    if (it != _dns_cache.end() && (now - it->second.resolved_at) < std::chrono::seconds(30))
    {
        if (it->second.ok)
            out_ip = it->second.ip;
        return it->second.ok;
    }

    dns_cache_entry_t entry;
    entry.resolved_at = now;

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res != nullptr)
    {
        if (res->ai_addr != nullptr && res->ai_addr->sa_family == AF_INET)
        {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
            entry.ip = utils::Endian::net_swap(sin->sin_addr.s_addr);
            entry.ok = true;
        }
        ::freeaddrinfo(res);
    }

    _dns_cache[host] = entry;
    if (!entry.ok)
    {
        APP_LOG(Log::LogLevel::WARN, "[NET] Failed to resolve host '%s' in custom_broadcast", host.c_str());
        return false;
    }

    APP_LOG(Log::LogLevel::INFO, "[NET] Resolved custom_broadcast host '%s' -> %s",
        host.c_str(), ipv4_to_str(entry.ip).c_str());
    out_ip = entry.ip;
    return true;
}

bool Network::SendBroadcast(Network_Message_pb& msg)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    assert((msg.source_id() != peer_t() && "Source id cannot be null"));
    assert((msg.dest_id() == peer_t() && "Destination id should be null"));

    //if (msg.appid() == 0)
    //    msg.set_appid(Settings::Inst().gameid.AppID());

    msg.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());

    std::string buffer;
    msg.SerializeToString(&buffer);
#if defined(NETWORK_COMPRESS)
    max_message_size = std::max<uint64_t>(max_message_size, buffer.length());

    buffer = std::move(compress(buffer.data(), buffer.length()));
    max_compressed_message_size = std::max<uint64_t>(max_compressed_message_size, buffer.length());
#endif

    std::set<uint64_t> sent_targets;
    auto send_udp_once = [&](uint32_t ip, uint16_t port)
    {
        uint64_t key = (static_cast<uint64_t>(ip) << 16) | port;
        if (!sent_targets.insert(key).second)
            return;

        ipv4_addr dst;
        dst.set_ip(ip);
        dst.set_port(port);
        try
        {
            _udp_socket.sendto(dst, buffer.data(), buffer.length());
        }
        catch (socket_exception& e)
        {
            APP_LOG(Log::LogLevel::DEBUG, "Udp sendto %s failed: %s", dst.to_string(true).c_str(), e.what());
        }
    };

    auto send_ports = [&](uint32_t ip, uint16_t port_count)
    {
        uint16_t n = std::min<uint16_t>(port_count, network_port_range);
        for (uint16_t offset = 0; offset < n; ++offset)
            send_udp_once(ip, static_cast<uint16_t>(_discovery_port + offset));
    };

    auto handle_cidr = [&](uint32_t net_ip, int prefix)
    {
        if (prefix == 32)
        {
            send_ports(net_ip, network_port_range);
            return;
        }
        if (prefix == 31)
        {
            uint32_t base = net_ip & ~1u;
            send_ports(base, network_port_range);
            send_ports(base | 1u, network_port_range);
            return;
        }

        uint32_t host_mask = (~0u) >> prefix;
        uint32_t net = net_ip & (~host_mask);
        uint32_t bcast = net | host_mask;
        uint64_t host_count = (host_mask >= 2) ? (static_cast<uint64_t>(host_mask) - 1u) : 0u;

        send_ports(bcast, network_port_range);

        if (host_count > 1022)
        {
            std::string sub_key = ipv4_to_str(net) + "/" + std::to_string(prefix);
            if (_logged_large_subnets.insert(sub_key).second)
            {
                APP_LOG(Log::LogLevel::INFO, "[NET] Large subnet %s (%llu hosts): using directed broadcast %s only",
                    sub_key.c_str(), static_cast<unsigned long long>(host_count), ipv4_to_str(bcast).c_str());
            }
            return;
        }

        uint16_t ports_per_host = (host_count <= 32) ? network_port_range : ((host_count <= 254) ? 2 : 1);
        for (uint64_t offset = 1; offset <= host_count; ++offset)
            send_ports(net + static_cast<uint32_t>(offset), ports_per_host);
    };

    if (Settings::Inst().enable_lan_broadcast)
    {
        std::vector<ipv4_addr> broadcasts = get_broadcasts();
        for (auto const& brd : broadcasts)
            send_ports(brd.get_ip(), network_port_range);
    }

    if (!Settings::Inst().custom_broadcast.empty())
    {
        std::string const& raw_list = Settings::Inst().custom_broadcast;
        size_t start = 0;
        while (start < raw_list.size())
        {
            size_t delim = raw_list.find_first_of(",;", start);
            std::string entry = (delim == std::string::npos)
                ? raw_list.substr(start)
                : raw_list.substr(start, delim - start);
            start = (delim == std::string::npos) ? raw_list.size() : (delim + 1);

            size_t first = entry.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                continue;
            size_t last = entry.find_last_not_of(" \t\r\n");
            entry = entry.substr(first, last - first + 1);

            // 1. Wildcard form: a.b.c.*
            if (entry.size() > 2 && entry.compare(entry.size() - 2, 2, ".*") == 0)
            {
                std::string net_str = entry.substr(0, entry.size() - 2) + ".0";
                uint32_t net_ip = 0;
                if (!parse_numeric_ipv4(net_str, net_ip) || (net_ip & 0xFFFFFF00u) == 0)
                {
                    APP_LOG(Log::LogLevel::WARN, "Invalid wildcard custom_broadcast '%s', ignoring", entry.c_str());
                    continue;
                }
                handle_cidr(net_ip, 24);
                continue;
            }

            // 2. CIDR form: a.b.c.d/prefix
            size_t slash = entry.find('/');
            if (slash != std::string::npos)
            {
                std::string ip_part = entry.substr(0, slash);
                std::string prefix_part = entry.substr(slash + 1);
                uint32_t cidr_ip = 0;
                uint32_t prefix = 0;
                if (!parse_numeric_ipv4(ip_part, cidr_ip) ||
                    !parse_uint_strict(prefix_part, 32, prefix) ||
                    prefix == 0)
                {
                    APP_LOG(Log::LogLevel::WARN, "Invalid CIDR custom_broadcast '%s', ignoring", entry.c_str());
                    continue;
                }
                handle_cidr(cidr_ip, static_cast<int>(prefix));
                continue;
            }

            // 3. IPv4 range: a.b.c.d-e.f.g.h or a.b.c.start-end_octet
            // Only parse '-' as range when the left-hand side is a valid numeric IPv4 (so hostnames like DESKTOP-ABC pass through).
            size_t dash = entry.find('-');
            if (dash != std::string::npos)
            {
                std::string left = entry.substr(0, dash);
                std::string right = entry.substr(dash + 1);
                uint32_t start_ip = 0;
                if (parse_numeric_ipv4(left, start_ip))
                {
                    uint32_t end_ip = 0;
                    uint32_t end_octet = 0;
                    if (parse_numeric_ipv4(right, end_ip))
                    {
                        // full end IPv4
                    }
                    else if (parse_uint_strict(right, 255, end_octet))
                    {
                        end_ip = (start_ip & 0xFFFFFF00u) | end_octet;
                    }
                    else
                    {
                        APP_LOG(Log::LogLevel::WARN, "Invalid range custom_broadcast '%s', ignoring", entry.c_str());
                        continue;
                    }

                    if (end_ip < start_ip)
                    {
                        APP_LOG(Log::LogLevel::WARN, "Reversed range custom_broadcast '%s', ignoring", entry.c_str());
                        continue;
                    }

                    uint64_t cnt = static_cast<uint64_t>(end_ip) - static_cast<uint64_t>(start_ip) + 1u;
                    if (cnt > 1024)
                    {
                        if (_logged_large_subnets.insert(entry).second)
                            APP_LOG(Log::LogLevel::WARN, "[NET] Range '%s' exceeds 1024 hosts (%llu), capping to 1024", entry.c_str(), static_cast<unsigned long long>(cnt));
                        cnt = 1024;
                    }

                    uint16_t ports_per_host = (cnt <= 32) ? network_port_range : ((cnt <= 256) ? 2 : 1);
                    for (uint64_t i = 0; i < cnt; ++i)
                        send_ports(start_ip + static_cast<uint32_t>(i), ports_per_host);
                    continue;
                }
            }

            // 4. Single IP or Hostname with optional :port
            std::string host_part = entry;
            uint32_t explicit_port = 0;
            bool has_explicit_port = false;
            size_t colon = entry.find(':');
            if (colon != std::string::npos)
            {
                if (entry.find(':', colon + 1) != std::string::npos)
                {
                    APP_LOG(Log::LogLevel::WARN, "Invalid custom_broadcast '%s', ignoring", entry.c_str());
                    continue;
                }
                host_part = entry.substr(0, colon);
                if (!parse_uint_strict(entry.substr(colon + 1), 65535, explicit_port) || explicit_port == 0)
                {
                    APP_LOG(Log::LogLevel::WARN, "Invalid port in custom_broadcast '%s', ignoring", entry.c_str());
                    continue;
                }
                has_explicit_port = true;
            }

            uint32_t resolved_ip = 0;
            if (!resolve_ipv4_host(host_part, resolved_ip))
                continue;

            if (has_explicit_port)
            {
                send_udp_once(resolved_ip, static_cast<uint16_t>(explicit_port));
            }
            else if ((resolved_ip & 0xFF000000u) != 0x7F000000u &&
                     resolved_ip != 0xFFFFFFFFu &&
                     (resolved_ip & 0xFFFFFF00u) != 0 &&
                     ((resolved_ip & 0xFFu) == 0 || (resolved_ip & 0xFFu) == 255))
            {
                handle_cidr(resolved_ip, 24);
            }
            else
            {
                send_ports(resolved_ip, network_port_range);
            }
        }
    }

    // Always probe local loopback so same-PC multi-instance testing and fallback always work
    send_ports(0x7F000001u, network_port_range);

    return true;
}

std::set<Network::peer_t> Network::UDPSendToAllPeers(Network_Message_pb& msg)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    assert((msg.source_id() != peer_t() && "Source id cannot be null"));

    std::set<peer_t> peers_sent_to;

    //if (msg.appid() == 0)
    //    msg.set_appid(Settings::Inst().gameid.AppID());

    std::for_each(_udp_addrs.begin(), _udp_addrs.end(), [&](std::pair<peer_t const, PortableAPI::ipv4_addr>& peer_infos)
    {
        msg.set_dest_id(peer_infos.first);
        msg.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());

        std::string buffer;
        msg.SerializeToString(&buffer);

    #if defined(NETWORK_COMPRESS)
        buffer = std::move(compress(buffer.data(), buffer.length()));

        max_message_size = std::max<uint64_t>(max_message_size, buffer.length());
        max_compressed_message_size = std::max<uint64_t>(max_compressed_message_size, buffer.length());
    #endif

        try
        {
            _udp_socket.sendto(peer_infos.second, buffer.data(), buffer.length());
            peers_sent_to.insert(peer_infos.first);
            //APP_LOG(Log::LogLevel::TRACE, "Sent message to %s", peer_infos.second.to_string().c_str());
        }
        catch (socket_exception & e)
        {
            //APP_LOG(Log::LogLevel::WARN, "Udp socket exception: %s on %s", e.what(), peer_infos.second.to_string().c_str());
        }
    });

    return peers_sent_to;
}

bool Network::UDPSendTo(Network_Message_pb& msg)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    assert((msg.source_id() != peer_t() && "Source id cannot be null"));

    auto it = _udp_addrs.find(msg.dest_id());
    if (it == _udp_addrs.end())
    {
        APP_LOG(Log::LogLevel::DEBUG, "UDPSendTo: no UDP route to %s yet, falling back to TCP", msg.dest_id().c_str());
        return TCPSendTo(msg);
    }

    //if (msg.appid() == 0)
    //    msg.set_appid(Settings::Inst().gameid.AppID());

    msg.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());

    std::string buffer;
    msg.SerializeToString(&buffer);

#if defined(NETWORK_COMPRESS)
    max_message_size = std::max<uint64_t>(max_message_size, buffer.length());

    buffer = std::move(compress(buffer.data(), buffer.length()));
    max_compressed_message_size = std::max<uint64_t>(max_compressed_message_size, buffer.length());
#endif

    try
    {
        _udp_socket.sendto(it->second, buffer.data(), buffer.length());
        //APP_LOG(Log::LogLevel::DEBUG, "Sent message to peer_id: %s, addr: %s", msg.dest_id().c_str(), it->second.to_string().c_str());
    }
    catch (socket_exception & e)
    {
        APP_LOG(Log::LogLevel::DEBUG, "UDPSendTo to %s failed (%s), falling back to TCP", it->second.to_string(true).c_str(), e.what());
        return TCPSendTo(msg);
    }

    return true;
}

std::set<Network::peer_t> Network::TCPSendToAllPeers(Network_Message_pb& msg)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    std::set<peer_t> peers_sent_to;

    assert((msg.source_id() != peer_t() && "Source id cannot be null"));

    //if (msg.appid() == 0)
    //    msg.set_appid(Settings::Inst().gameid.AppID());

    std::for_each(_tcp_peers.begin(), _tcp_peers.end(), [&](std::pair<peer_t const, tcp_socket*>& client)
    {
        msg.set_dest_id(client.first);
        msg.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());

        std::string buffer(sizeof(next_packet_size_t), 0);

    #if defined(NETWORK_COMPRESS)
        std::string data;
        msg.SerializeToString(&data);

        max_message_size = std::max<uint64_t>(max_message_size, data.length());

        buffer += std::move(compress(data.data(), data.length()));
        max_compressed_message_size = std::max<uint64_t>(max_compressed_message_size, buffer.length());
    #else
        buffer += std::move(msg.SerializeAsString());
    #endif

        *reinterpret_cast<next_packet_size_t*>(&buffer[0]) = make_next_packet_size(buffer);

        try
        {
            client.second->send(buffer.data(), buffer.length());
            peers_sent_to.insert(client.first);
            //APP_LOG(Log::LogLevel::TRACE, "Sent message to %s", peer_infos.second.to_string().c_str());
        }
        catch (socket_exception & e)
        {
            //APP_LOG(Log::LogLevel::WARN, "Tcp socket exception: %s on %s", e.what(), client.second->get_addr().to_string().c_str());
        }
    });

    return peers_sent_to;
}

bool Network::TCPSendTo(Network_Message_pb& msg)
{
    std::lock_guard<std::recursive_mutex> lk(local_mutex);

    assert((msg.source_id() != peer_t() && "Source id cannot be null"));

    auto it = _tcp_peers.find(msg.dest_id());
    if (it == _tcp_peers.end())
    {
        //APP_LOG(Log::LogLevel::ERR, "No route to %llu", msg.dest_id());
        return false;
    }

    //if (msg.appid() == 0)
    //    msg.set_appid(Settings::Inst().gameid.AppID());

    msg.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());

    std::string buffer(sizeof(next_packet_size_t), 0);

#if defined(NETWORK_COMPRESS)
    std::string data;
    msg.SerializeToString(&data);

    max_message_size = std::max<uint64_t>(max_message_size, data.length());

    buffer += std::move(compress(data.data(), data.length()));
    max_compressed_message_size = std::max<uint64_t>(max_compressed_message_size, buffer.length());
#else
    buffer += std::move(msg.SerializeAsString());
#endif

    *reinterpret_cast<next_packet_size_t*>(&buffer[0]) = make_next_packet_size(buffer);

    try
    {
        it->second->send(buffer.data(), buffer.length());
        //APP_LOG(Log::LogLevel::TRACE, "Sent message to %s", it->second->get_addr().to_string());
    }
    catch (socket_exception & e)
    {
        //APP_LOG(Log::LogLevel::WARN, "Tcp socket exception: %s on %s", e.what(), it->second->get_addr().to_string().c_str());
        return false;
    }

    return true;
}
