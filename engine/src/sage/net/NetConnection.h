#pragma once
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

#include "sage/net/NetAddress.h"
#include "sage/net/UdpSocket.h"

// ---------------------------------------------------------------------------
// NetConnection — состояние одного соединения поверх UDP: пакетная обвязка
// (seq/ack/ack-биты), НАДЁЖНЫЙ канал (доставка гарантирована, порядок
// сохраняется: msgId + переотправка по таймеру + окно дедупликации/сборки на
// приёме) и ненадёжный канал (доставляется как дошло — снапшоты, где важна
// только свежесть). Большие сообщения прозрачно фрагментируются.
//
// Формат пакета (little-endian, всё вручную — независимо от платформы):
//   [u32 magic 'SGN1'][u8 type][u16 seq][u16 ack][u32 ackBits] + payload
// Payload (type == Payload) — последовательность сообщений:
//   [u8 flags][u16 msgId?][u16 len][len байт]
//   flags: bit0 — надёжное (есть msgId), bit1 — фрагмент (в теле заголовок
//   фрагмента: [u32 fragKey][u16 index][u16 count]).
//
// Время передаётся явно (double, секунды) — тесты управляют часами сами.
// ---------------------------------------------------------------------------
namespace sage::net {

constexpr uint32_t kProtocolMagic = 0x53474E31u; // 'SGN1'
constexpr size_t kMaxPacket = 1200;              // безопасно ниже типичного MTU
constexpr size_t kMaxFragmentBody = 1024;        // тело одного фрагмента
constexpr size_t kMaxMessage = 256 * 1024;       // лимит сообщения (фрагментация)
constexpr double kResendInterval = 0.12;         // переотправка неподтверждённого
constexpr double kKeepAliveInterval = 0.25;
constexpr double kTimeout = 5.0;

enum class PacketType : uint8_t {
    ConnectRequest = 0,
    ConnectAccept = 1,
    ConnectDeny = 2,
    Payload = 3,
    KeepAlive = 4,
    Disconnect = 5,
};

class NetConnection {
public:
    void Reset(const NetAddress& remote, double now);
    const NetAddress& Remote() const { return m_remote; }

    // Постановка сообщения в исходящую очередь (надёжно/ненадёжно). Большие
    // сообщения нарезаются на фрагменты здесь же.
    bool Send(const uint8_t* data, size_t bytes, bool reliable);

    // Обработка ВХОДЯЩЕГО Payload/KeepAlive-пакета (без magic/type — их снял
    // вызывающий). Готовые к доставке сообщения кладутся во внутреннюю очередь.
    void OnPacket(PacketType type, const uint8_t* data, size_t bytes, double now);

    // Отправка накопленного: собирает пакеты (свежие сообщения + переотправки
    // + ack-и + keepalive при простое) и шлёт их через сокет.
    void Flush(UdpSocket& socket, double now);

    // Забрать доставленные сообщения (надёжные — строго по порядку отправки).
    std::vector<std::vector<uint8_t>> DrainReceived();

    double LastRecvTime() const { return m_lastRecv; }
    bool TimedOut(double now) const { return now - m_lastRecv > kTimeout; }

private:
    struct OutMessage {
        std::vector<uint8_t> Body; // уже с флагами/заголовком фрагмента
        bool Reliable = false;
        uint16_t MsgId = 0;        // только надёжные
        double LastSent = -1.0;    // -1 — ещё не отправлялось
        bool Acked = false;
    };

    void QueueBody(std::vector<uint8_t> body, bool reliable);
    void HandleMessage(const uint8_t* body, size_t len);   // после дедупликации
    void DeliverOrBuffer(uint16_t msgId, std::vector<uint8_t> body); // надёжные по порядку
    void AckPacket(uint16_t seq);                          // подтверждение нашего пакета

    NetAddress m_remote;
    double m_lastRecv = 0.0;
    double m_lastSent = -1e9;

    // --- Отправка ---
    uint16_t m_nextSeq = 1;        // seq исходящих пакетов
    uint16_t m_nextMsgId = 1;      // id исходящих надёжных сообщений
    std::deque<OutMessage> m_outbox;
    // Какие надёжные сообщения летели в каком пакете (для подтверждения по ack).
    std::unordered_map<uint16_t, std::vector<uint16_t>> m_packetContents;

    // --- Приём ---
    uint16_t m_remoteSeq = 0;      // последний seq удалённой стороны (для ack)
    uint32_t m_remoteAckBits = 0;  // битовая история 32 предыдущих seq
    bool m_remoteSeqValid = false;
    uint16_t m_expectedMsgId = 1;  // следующий надёжный к доставке (порядок)
    std::map<uint16_t, std::vector<uint8_t>> m_heldReliable; // пришедшие вне порядка
    std::vector<std::vector<uint8_t>> m_received;

    // Сборка фрагментов: ключ -> {count, собрано, тела}.
    struct FragmentBuffer {
        uint16_t Count = 0;
        uint16_t Have = 0;
        std::vector<std::vector<uint8_t>> Parts;
    };
    std::unordered_map<uint32_t, FragmentBuffer> m_fragments;
    uint32_t m_nextFragKey = 1;
};

// --- Утилиты записи/чтения little-endian (общие для sage/net) --------------
inline void WriteU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
inline void WriteU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back((uint8_t)(v & 0xFF));
    b.push_back((uint8_t)(v >> 8));
}
inline void WriteU32(std::vector<uint8_t>& b, uint32_t v) {
    WriteU16(b, (uint16_t)(v & 0xFFFF));
    WriteU16(b, (uint16_t)(v >> 16));
}
inline void WriteF32(std::vector<uint8_t>& b, float v) {
    uint32_t u;
    static_assert(sizeof(u) == sizeof(v));
    std::memcpy(&u, &v, 4);
    WriteU32(b, u);
}

struct ByteReader {
    const uint8_t* Data = nullptr;
    size_t Size = 0;
    size_t Pos = 0;
    bool Ok = true;

    uint8_t U8() { return Pos + 1 <= Size ? Data[Pos++] : (Ok = false, 0); }
    uint16_t U16() {
        if (Pos + 2 > Size) { Ok = false; return 0; }
        uint16_t v = (uint16_t)(Data[Pos] | (Data[Pos + 1] << 8));
        Pos += 2;
        return v;
    }
    uint32_t U32() {
        uint32_t lo = U16(), hi = U16();
        return lo | (hi << 16);
    }
    float F32() {
        uint32_t u = U32();
        float v;
        std::memcpy(&v, &u, 4);
        return v;
    }
    bool Bytes(void* out, size_t n) {
        if (Pos + n > Size) { Ok = false; return false; }
        std::memcpy(out, Data + Pos, n);
        Pos += n;
        return true;
    }
};

} // namespace sage::net
