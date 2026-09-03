#include "sage/net/UdpSocket.h"

#include <cstring>

#include "sage/core/Log.h"

// Системные заголовки сети — только здесь (см. комментарий в UdpSocket.h).
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

namespace sage::net {

namespace {

#ifdef _WIN32
// Winsock требует однократной инициализации на процесс.
struct WinsockInit {
    WinsockInit() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockInit() { WSACleanup(); }
};
void EnsureWinsock() { static WinsockInit init; }
#endif

sockaddr_in ToSockaddr(const NetAddress& a) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(a.Port);
    sa.sin_addr.s_addr = htonl(a.Ip);
    return sa;
}

NetAddress FromSockaddr(const sockaddr_in& sa) {
    return {ntohl(sa.sin_addr.s_addr), ntohs(sa.sin_port)};
}

// Детерминированный хэш для симуляции потерь.
uint32_t LossHash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

} // namespace

NetAddress NetAddress::Parse(const std::string& ip, uint16_t port) {
    // Разбор «a.b.c.d» вручную — без inet_pton, чтобы не тянуть платформенные
    // заголовки в NetAddress.h (и с одинаковым поведением на всех платформах).
    NetAddress out{0, port};
    uint32_t parts[4] = {0, 0, 0, 0};
    int part = 0;
    bool digit = false;
    for (char c : ip) {
        if (c >= '0' && c <= '9') {
            parts[part] = parts[part] * 10 + (uint32_t)(c - '0');
            if (parts[part] > 255) return {};
            digit = true;
        } else if (c == '.' && digit && part < 3) {
            ++part;
            digit = false;
        } else {
            return {};
        }
    }
    if (part != 3 || !digit) return {};
    out.Ip = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return out;
}

std::string NetAddress::ToString() const {
    return std::to_string((Ip >> 24) & 0xFF) + "." + std::to_string((Ip >> 16) & 0xFF) + "." +
           std::to_string((Ip >> 8) & 0xFF) + "." + std::to_string(Ip & 0xFF) + ":" +
           std::to_string(Port);
}

UdpSocket::~UdpSocket() { Close(); }

bool UdpSocket::Open(uint16_t port) {
    Close();
#ifdef _WIN32
    EnsureWinsock();
    m_handle = (Handle)::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if ((SOCKET)m_handle == INVALID_SOCKET) { m_handle = kInvalid; return false; }
#else
    m_handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_handle < 0) { m_handle = kInvalid; return false; }
#endif

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind((
#ifdef _WIN32
            SOCKET
#else
            int
#endif
            )m_handle, (sockaddr*)&sa, sizeof(sa)) != 0) {
        LOG_ERROR("Net") << "UDP bind на порт " << port << " не удался";
        Close();
        return false;
    }

    // Неблокирующий режим: приём/отправка из главного цикла без ожиданий.
#ifdef _WIN32
    u_long nonblock = 1;
    ioctlsocket((SOCKET)m_handle, FIONBIO, &nonblock);
#else
    int flags = fcntl(m_handle, F_GETFL, 0);
    fcntl(m_handle, F_SETFL, flags | O_NONBLOCK);
#endif
    return true;
}

void UdpSocket::Close() {
    if (m_handle == kInvalid) return;
#ifdef _WIN32
    closesocket((SOCKET)m_handle);
#else
    ::close(m_handle);
#endif
    m_handle = kInvalid;
}

uint16_t UdpSocket::LocalPort() const {
    if (m_handle == kInvalid) return 0;
    sockaddr_in sa{};
#ifdef _WIN32
    int len = sizeof(sa);
    if (getsockname((SOCKET)m_handle, (sockaddr*)&sa, &len) != 0) return 0;
#else
    socklen_t len = sizeof(sa);
    if (getsockname(m_handle, (sockaddr*)&sa, &len) != 0) return 0;
#endif
    return ntohs(sa.sin_port);
}

bool UdpSocket::SendTo(const NetAddress& to, const void* data, size_t bytes) {
    if (m_handle == kInvalid) return false;
    sockaddr_in sa = ToSockaddr(to);
#ifdef _WIN32
    int sent = ::sendto((SOCKET)m_handle, (const char*)data, (int)bytes, 0,
                        (sockaddr*)&sa, sizeof(sa));
#else
    ssize_t sent = ::sendto(m_handle, data, bytes, 0, (sockaddr*)&sa, sizeof(sa));
#endif
    return sent == (decltype(sent))bytes;
}

bool UdpSocket::RecvFrom(NetAddress& from, void* buffer, size_t capacity, size_t& outBytes) {
    if (m_handle == kInvalid) return false;
    for (;;) {
        sockaddr_in sa{};
#ifdef _WIN32
        int len = sizeof(sa);
        int got = ::recvfrom((SOCKET)m_handle, (char*)buffer, (int)capacity, 0,
                             (sockaddr*)&sa, &len);
        if (got < 0) return false; // WSAEWOULDBLOCK и пр. — очередь пуста
#else
        socklen_t len = sizeof(sa);
        ssize_t got = ::recvfrom(m_handle, buffer, capacity, 0, (sockaddr*)&sa, &len);
        if (got < 0) return false; // EWOULDBLOCK — очередь пуста
#endif
        ++m_recvCounter;
        // Симуляция потерь (тесты надёжного канала): датаграмма «не дошла».
        if (m_loss > 0.0f) {
            uint32_t h = LossHash(m_recvCounter * 2654435761u + m_lossSeed);
            if ((h & 0xFFFF) / 65535.0f < m_loss) continue;
        }
        from = FromSockaddr(sa);
        outBytes = (size_t)got;
        return true;
    }
}

void UdpSocket::SetSimulatedLoss(float lossRatio, uint32_t seed) {
    m_loss = lossRatio;
    m_lossSeed = seed;
}

} // namespace sage::net
