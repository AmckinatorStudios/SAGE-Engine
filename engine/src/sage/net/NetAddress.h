#pragma once
#include <cstdint>
#include <functional>
#include <string>

// ---------------------------------------------------------------------------
// NetAddress — IPv4-адрес + порт, бэкенд-независимо (без включения заголовков
// сокетов). Хранение в host byte order; преобразование в/из sockaddr — забота
// UdpSocket (единственного места с системными заголовками сети).
// ---------------------------------------------------------------------------
namespace sage::net {

struct NetAddress {
    uint32_t Ip = 0;   // host byte order (127.0.0.1 = 0x7F000001)
    uint16_t Port = 0;

    bool operator==(const NetAddress& o) const { return Ip == o.Ip && Port == o.Port; }
    bool operator!=(const NetAddress& o) const { return !(*this == o); }
    bool Valid() const { return Port != 0; }

    // «a.b.c.d» — только числовой IPv4 (DNS сознательно вне ядра сети движка).
    static NetAddress Parse(const std::string& ip, uint16_t port);
    static NetAddress Loopback(uint16_t port) { return {0x7F000001u, port}; }

    std::string ToString() const;
};

} // namespace sage::net

// Ключ для unordered_map<NetAddress, ...> (таблица соединений сервера).
template <>
struct std::hash<sage::net::NetAddress> {
    size_t operator()(const sage::net::NetAddress& a) const noexcept {
        return std::hash<uint64_t>{}(((uint64_t)a.Ip << 16) | a.Port);
    }
};
