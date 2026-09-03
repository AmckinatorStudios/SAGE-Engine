#pragma once
#include <cstdint>
#include <cstddef>
#include "sage/net/NetAddress.h"

// ---------------------------------------------------------------------------
// UdpSocket — кроссплатформенный неблокирующий UDP-сокет (POSIX / Winsock).
// ЕДИНСТВЕННОЕ место движка с системными заголовками сети (как rhi/opengl для
// GL): весь остальной sage/net работает через этот класс и NetAddress.
//
// Для детерминированных тестов надёжного канала встроена симуляция потерь:
// SetSimulatedLoss(долю) — входящие датаграммы отбрасываются детерминированным
// генератором (по счётчику пакетов + сиду), сеть при этом настоящая (loopback).
// ---------------------------------------------------------------------------
namespace sage::net {

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Открывает и биндит сокет на порт (0 — эфемерный порт от ОС), переводит в
    // неблокирующий режим. false — ошибка (лог).
    bool Open(uint16_t port);
    void Close();
    bool IsOpen() const { return m_handle != kInvalid; }

    // Фактический локальный порт (нужен после Open(0)).
    uint16_t LocalPort() const;

    // Отправка датаграммы. false — системная ошибка отправки.
    bool SendTo(const NetAddress& to, const void* data, size_t bytes);

    // Приём одной датаграммы (неблокирующий): false — очередь пуста.
    // outBytes — фактический размер, буфер должен вмещать до 64КБ.
    bool RecvFrom(NetAddress& from, void* buffer, size_t capacity, size_t& outBytes);

    // Симуляция потерь входящих (0..1); детерминирована (счётчик + сид).
    void SetSimulatedLoss(float lossRatio, uint32_t seed = 1);

private:
#ifdef _WIN32
    using Handle = uintptr_t;
    static constexpr Handle kInvalid = ~(uintptr_t)0;
#else
    using Handle = int;
    static constexpr Handle kInvalid = -1;
#endif
    Handle m_handle = kInvalid;
    float m_loss = 0.0f;
    uint32_t m_lossSeed = 1;
    uint32_t m_recvCounter = 0;
};

} // namespace sage::net
