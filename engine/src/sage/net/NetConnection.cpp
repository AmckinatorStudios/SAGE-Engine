#include "sage/net/NetConnection.h"

#include <algorithm>

#include "sage/core/Log.h"

namespace sage::net {

namespace {

// Сравнение 16-битных счётчиков с заворотом (seq/msgId).
inline bool SeqGreater(uint16_t a, uint16_t b) {
    return ((a > b) && (a - b <= 32768)) || ((a < b) && (b - a > 32768));
}

constexpr uint8_t kFlagReliable = 1 << 0;

} // namespace

void NetConnection::Reset(const NetAddress& remote, double now) {
    *this = NetConnection{};
    m_remote = remote;
    m_lastRecv = now;
}

// Тело в очереди ВСЕГДА с однобайтовым префиксом-маркером: 0 — обычное
// сообщение, 1 — фрагмент ([u32 key][u16 idx][u16 count] + кусок). Маркер
// уходит на провод как первый байт тела и читается приёмником в HandleMessage.
bool NetConnection::Send(const uint8_t* data, size_t bytes, bool reliable) {
    if (bytes > kMaxMessage) {
        LOG_ERROR("Net") << "Сообщение " << bytes << " байт превышает лимит " << kMaxMessage;
        return false;
    }
    if (bytes <= kMaxFragmentBody) {
        std::vector<uint8_t> body;
        body.reserve(bytes + 1);
        body.push_back(0); // не фрагмент
        body.insert(body.end(), data, data + bytes);
        QueueBody(std::move(body), reliable);
        return true;
    }

    // Фрагментация. Ключ общий для фрагментов сообщения; count в каждом
    // (сборщик не обязан увидеть первый фрагмент раньше остальных).
    uint32_t key = m_nextFragKey++;
    uint16_t count = (uint16_t)((bytes + kMaxFragmentBody - 1) / kMaxFragmentBody);
    for (uint16_t i = 0; i < count; ++i) {
        size_t off = (size_t)i * kMaxFragmentBody;
        size_t len = std::min(kMaxFragmentBody, bytes - off);
        std::vector<uint8_t> body;
        body.reserve(9 + len);
        body.push_back(1); // фрагмент
        WriteU32(body, key);
        WriteU16(body, i);
        WriteU16(body, count);
        body.insert(body.end(), data + off, data + off + len);
        QueueBody(std::move(body), reliable);
    }
    return true;
}

void NetConnection::QueueBody(std::vector<uint8_t> body, bool reliable) {
    OutMessage m;
    m.Body = std::move(body);
    m.Reliable = reliable;
    if (reliable) m.MsgId = m_nextMsgId++;
    m_outbox.push_back(std::move(m));
}

void NetConnection::OnPacket(PacketType type, const uint8_t* data, size_t bytes, double now) {
    ByteReader r{data, bytes, 0, true};
    uint16_t seq = r.U16();
    uint16_t ack = r.U16();
    uint32_t ackBits = r.U32();
    if (!r.Ok) return;
    m_lastRecv = now;

    // 1) Регистрируем их seq в нашей ack-истории (уйдёт обратным пакетом).
    if (!m_remoteSeqValid) {
        m_remoteSeq = seq;
        m_remoteAckBits = 0;
        m_remoteSeqValid = true;
    } else if (SeqGreater(seq, m_remoteSeq)) {
        uint16_t delta = (uint16_t)(seq - m_remoteSeq);
        m_remoteAckBits = delta >= 32 ? 0 : (m_remoteAckBits << delta);
        if (delta >= 1 && delta <= 32) m_remoteAckBits |= 1u << (delta - 1);
        m_remoteSeq = seq;
    } else {
        uint16_t delta = (uint16_t)(m_remoteSeq - seq);
        if (delta >= 1 && delta <= 32) m_remoteAckBits |= 1u << (delta - 1);
        // Дубликат/устаревший пакет: надёжные сообщения отсеет дедупликация msgId.
    }

    // 2) Их подтверждения НАШИХ пакетов -> помечаем надёжные доставленными.
    AckPacket(ack);
    for (int i = 0; i < 32; ++i)
        if (ackBits & (1u << i)) AckPacket((uint16_t)(ack - 1 - i));

    if (type != PacketType::Payload) return; // KeepAlive нёс только ack-и

    // 3) Сообщения пакета: [u8 flags][u16 msgId?][u16 len][len байт].
    while (r.Ok && r.Pos < r.Size) {
        uint8_t flags = r.U8();
        uint16_t msgId = (flags & kFlagReliable) ? r.U16() : 0;
        uint16_t len = r.U16();
        if (!r.Ok || r.Pos + len > r.Size) break;
        const uint8_t* body = r.Data + r.Pos;
        r.Pos += len;

        if (flags & kFlagReliable)
            DeliverOrBuffer(msgId, std::vector<uint8_t>(body, body + len));
        else
            HandleMessage(body, len);
    }
}

void NetConnection::DeliverOrBuffer(uint16_t msgId, std::vector<uint8_t> body) {
    if (msgId == m_expectedMsgId) {
        HandleMessage(body.data(), body.size());
        ++m_expectedMsgId;
        // Доставляем всё, что ждало своей очереди (строгий порядок отправки).
        auto it = m_heldReliable.find(m_expectedMsgId);
        while (it != m_heldReliable.end()) {
            HandleMessage(it->second.data(), it->second.size());
            m_heldReliable.erase(it);
            ++m_expectedMsgId;
            it = m_heldReliable.find(m_expectedMsgId);
        }
    } else if (SeqGreater(msgId, m_expectedMsgId)) {
        m_heldReliable.emplace(msgId, std::move(body)); // вне порядка — придержать
    }
    // Иначе — дубликат уже доставленного, игнорируем.
}

void NetConnection::HandleMessage(const uint8_t* body, size_t len) {
    if (len < 1) return;
    uint8_t isFragment = body[0]; // маркер-префикс (см. Send)
    ++body;
    --len;

    if (!isFragment) {
        m_received.emplace_back(body, body + len);
        return;
    }

    // Фрагмент: [u32 key][u16 idx][u16 count] + кусок.
    ByteReader r{body, len, 0, true};
    uint32_t key = r.U32();
    uint16_t idx = r.U16();
    uint16_t count = r.U16();
    if (!r.Ok || count == 0 || idx >= count) return;

    FragmentBuffer& fb = m_fragments[key];
    if (fb.Count == 0) {
        fb.Count = count;
        fb.Parts.resize(count);
    }
    if (fb.Count != count || !fb.Parts[idx].empty()) return; // мусор/дубликат
    fb.Parts[idx].assign(body + r.Pos, body + len);
    ++fb.Have;

    if (fb.Have == fb.Count) {
        std::vector<uint8_t> full;
        for (auto& part : fb.Parts) full.insert(full.end(), part.begin(), part.end());
        m_fragments.erase(key);
        m_received.push_back(std::move(full));
    }

    // Защита от накопления брошенных сборок (потерянные ненадёжные фрагменты).
    if (m_fragments.size() > 64) m_fragments.clear();
}

void NetConnection::AckPacket(uint16_t seq) {
    auto it = m_packetContents.find(seq);
    if (it == m_packetContents.end()) return;
    for (uint16_t msgId : it->second) {
        for (OutMessage& m : m_outbox)
            if (m.Reliable && m.MsgId == msgId) m.Acked = true;
    }
    m_packetContents.erase(it);
}

void NetConnection::Flush(UdpSocket& socket, double now) {
    // Подтверждённые надёжные больше не нужны.
    m_outbox.erase(std::remove_if(m_outbox.begin(), m_outbox.end(),
                                  [](const OutMessage& m) { return m.Reliable && m.Acked; }),
                   m_outbox.end());

    auto writeHeader = [&](std::vector<uint8_t>& p, PacketType type, uint16_t seq) {
        WriteU32(p, kProtocolMagic);
        WriteU8(p, (uint8_t)type);
        WriteU16(p, seq);
        WriteU16(p, m_remoteSeq);
        WriteU32(p, m_remoteSeqValid ? m_remoteAckBits : 0);
    };

    bool sentAnything = false;
    int packetsThisFlush = 0;
    for (;;) {
        std::vector<uint8_t> packet;
        std::vector<uint16_t> reliableInPacket;
        std::vector<size_t> unreliableSent;
        uint16_t seq = m_nextSeq;
        const size_t headerSize = 4 + 1 + 2 + 2 + 4;
        size_t used = headerSize;

        // Кандидаты: не отправлявшиеся + надёжные с истёкшей переотправкой.
        for (size_t i = 0; i < m_outbox.size(); ++i) {
            OutMessage& m = m_outbox[i];
            bool due = m.LastSent < 0.0 ||
                       (m.Reliable && !m.Acked && now - m.LastSent >= kResendInterval);
            if (!due) continue;

            size_t need = 1 + (m.Reliable ? 2u : 0u) + 2 + m.Body.size();
            if (used + need > kMaxPacket) continue; // в следующий пакет

            if (packet.empty()) writeHeader(packet, PacketType::Payload, seq);
            WriteU8(packet, m.Reliable ? kFlagReliable : 0);
            if (m.Reliable) WriteU16(packet, m.MsgId);
            WriteU16(packet, (uint16_t)m.Body.size());
            packet.insert(packet.end(), m.Body.begin(), m.Body.end());
            used += need;

            m.LastSent = now;
            if (m.Reliable) reliableInPacket.push_back(m.MsgId);
            else unreliableSent.push_back(i);
        }

        if (packet.empty()) break; // отправлять нечего

        ++m_nextSeq;
        if (!reliableInPacket.empty()) {
            m_packetContents[seq] = std::move(reliableInPacket);
            // Страховка от роста при систематических потерях: старые записи
            // бесполезны (сообщения уже переехали в новые пакеты).
            if (m_packetContents.size() > 512) m_packetContents.clear();
        }
        socket.SendTo(m_remote, packet.data(), packet.size());
        m_lastSent = now;
        sentAnything = true;

        // Ненадёжные — однократные: убираем отправленные (сзади наперёд, чтобы
        // не сдвигать индексы).
        for (auto it = unreliableSent.rbegin(); it != unreliableSent.rend(); ++it)
            m_outbox.erase(m_outbox.begin() + (long)*it);

        if (++packetsThisFlush >= 64) break; // защита от бесконечного цикла
    }

    // Простой — keepalive (несёт наши ack-и и держит соединение живым).
    if (!sentAnything && now - m_lastSent >= kKeepAliveInterval) {
        std::vector<uint8_t> packet;
        writeHeader(packet, PacketType::KeepAlive, m_nextSeq++);
        socket.SendTo(m_remote, packet.data(), packet.size());
        m_lastSent = now;
    }
}

std::vector<std::vector<uint8_t>> NetConnection::DrainReceived() {
    std::vector<std::vector<uint8_t>> out;
    out.swap(m_received);
    return out;
}

} // namespace sage::net
