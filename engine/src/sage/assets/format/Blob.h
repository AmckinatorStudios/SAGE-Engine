#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// SAGE Blob — общий бинарный контейнер для СВОИХ форматов движка.
//
// ЗАЧЕМ СВОЙ ФОРМАТ ВООБЩЕ. Чужие форматы (.obj, .gltf, .blend, .bbmodel) —
// это форматы ОБМЕНА: они описывают то, что удобно редактору-источнику, и
// платят за универсальность. .obj — текст, где каждое число разбирается из
// строки; .gltf — JSON плюс base64 или отдельный .bin; .blend — дамп памяти
// Blender'а со своей схемой. Ни один из них не описывает то, что нужно движку
// В МОМЕНТ ЗАГРУЗКИ: готовый к заливке в видеопамять буфер. Поэтому загрузка
// модели — это всегда разбор, перестройка и пересчёт, каждый запуск заново.
//
// Свой формат снимает ровно это: файл лежит на диске в том виде, в котором его
// примет видеокарта, и загрузка сводится к «прочитать и распаковать».
//
// ПОЧЕМУ КОНТЕЙНЕР, А НЕ ФОРМАТ НА ТИП. Меш, текстура, анимация и сцена — разные
// данные, но задачи у их файлов одинаковые: узнать версию не читая всё,
// прочитать один кусок не читая остальные, поймать порчу файла, дать сжатию
// работать по-разному на разных кусках. Один контейнер решает это один раз, а
// каждый тип описывает только СВОИ куски.
//
// УСТРОЙСТВО. Заголовок, таблица кусков, данные:
//
//   [Header]  магия, версия контейнера, тип ассета, версия схемы типа
//   [Chunk]*  для каждого куска: имя, кодек, смещение, размеры, контрольная сумма
//   [данные]  сами куски подряд
//
// Куски адресуются по ИМЕНИ (четыре байта), а не по порядку: добавление нового
// куска в новой версии движка не ломает чтение старых файлов, а старый движок
// просто не найдёт незнакомый кусок и пропустит его. Это то же решение, что в
// RIFF/PNG, и по той же причине — формат живёт дольше, чем код, который его
// придумал.
//
// ЧИСЛА — LITTLE-ENDIAN ЯВНО, побайтно. Не memcpy структуры: раскладка структуры
// зависит от компилятора и платформы, и файл, записанный одной сборкой, не
// обязан читаться другой. Ассеты переезжают между машинами всегда.
// ---------------------------------------------------------------------------

namespace sage::assets {

// Четырёхбуквенный код: 'MESH', 'POSI'. Пишется как есть, поэтому имена видны
// в hex-редакторе — по файлу можно понять, что в нём, без документации.
constexpr uint32_t FourCC(const char (&s)[5]) {
    return (uint32_t)(uint8_t)s[0] | ((uint32_t)(uint8_t)s[1] << 8) |
           ((uint32_t)(uint8_t)s[2] << 16) | ((uint32_t)(uint8_t)s[3] << 24);
}

std::string FourCCToString(uint32_t cc);

enum class Codec : uint32_t {
    Raw = 0,       // без сжатия
    Deflate = 1,   // общего назначения (miniz)
};

// Кусок в памяти. Data — всегда РАСПАКОВАННЫЕ байты: сжатие живёт только на
// диске, вызывающий с ним не работает.
struct BlobChunk {
    uint32_t Id = 0;
    std::vector<uint8_t> Data;
    Codec Preferred = Codec::Deflate;   // что попробовать при записи
};

// Собранный контейнер.
class Blob {
public:
    Blob() = default;
    Blob(uint32_t assetType, uint32_t assetVersion)
        : m_assetType(assetType), m_assetVersion(assetVersion) {}

    uint32_t AssetType() const { return m_assetType; }
    uint32_t AssetVersion() const { return m_assetVersion; }
    void SetAsset(uint32_t type, uint32_t version) { m_assetType = type; m_assetVersion = version; }

    void Add(uint32_t id, std::vector<uint8_t> data, Codec preferred = Codec::Deflate);
    // nullptr — куска нет. Именно так и проверяется наличие необязательных
    // данных (нормалей, второго UV): отсутствие куска — это не ошибка.
    const std::vector<uint8_t>* Find(uint32_t id) const;
    const std::vector<BlobChunk>& Chunks() const { return m_chunks; }

    // Сериализация. Сжатие применяется ПОКУСОЧНО и только если оно реально
    // выиграло: на уже сжатых или крошечных данных deflate раздувает, и хранить
    // такой кусок сжатым — значит платить распаковкой ни за что.
    std::vector<uint8_t> Serialize() const;
    bool Write(const std::string& path, std::string& err) const;

    // Разбор. Проверяет магию, версию и контрольную сумму КАЖДОГО куска: битый
    // ассет должен обнаружиться при загрузке, а не проявиться дырой в кадре.
    static bool Parse(const std::vector<uint8_t>& bytes, Blob& out, std::string& err);
    static bool Read(const std::string& path, Blob& out, std::string& err);

private:
    uint32_t m_assetType = 0;
    uint32_t m_assetVersion = 0;
    std::vector<BlobChunk> m_chunks;
};

// --- Примитивы записи/чтения чисел (явный little-endian) --------------------
//
// Публичные, потому что ими пользуются схемы конкретных типов: им нужно
// раскладывать свои куски теми же правилами, что и контейнер.
struct ByteWriter {
    std::vector<uint8_t> Bytes;

    void U8(uint8_t v) { Bytes.push_back(v); }
    void U16(uint16_t v);
    void U32(uint32_t v);
    void U64(uint64_t v);
    void I16(int16_t v) { U16((uint16_t)v); }
    void F32(float v);
    void Raw(const void* data, size_t size);
    // Переменная длина: маленькие числа занимают один байт. Для индексов и
    // счётчиков это заметная разница, а они и составляют большую часть меша.
    void VarU32(uint32_t v);
    void VarI32(int32_t v);   // зигзаг + VarU32: дельты бывают отрицательными
    void Str(const std::string& s);
};

struct ByteReader {
    const uint8_t* Data = nullptr;
    size_t Size = 0;
    size_t Pos = 0;
    bool Bad = false;   // выход за границу — липкий флаг, проверяется в конце

    ByteReader() = default;
    ByteReader(const std::vector<uint8_t>& v) : Data(v.data()), Size(v.size()) {}
    ByteReader(const uint8_t* d, size_t n) : Data(d), Size(n) {}

    bool Ok() const { return !Bad; }
    size_t Remaining() const { return Pos <= Size ? Size - Pos : 0; }

    uint8_t U8();
    uint16_t U16();
    uint32_t U32();
    uint64_t U64();
    int16_t I16() { return (int16_t)U16(); }
    float F32();
    void Raw(void* out, size_t size);
    uint32_t VarU32();
    int32_t VarI32();
    std::string Str();
};

// --- Сжатие общего назначения ------------------------------------------------
// Обёртки над miniz. Возвращают false, если библиотека отказала; вызывающий
// тогда хранит кусок как есть — сжатие не обязано удаться.
bool DeflateBytes(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, int level = 9);
bool InflateBytes(const std::vector<uint8_t>& in, size_t rawSize, std::vector<uint8_t>& out);

// Контрольная сумма содержимого куска (FNV-1a 64). Не криптография — задача
// поймать испорченный файл, а не подделку.
uint64_t HashBytes(const void* data, size_t size);

} // namespace sage::assets
