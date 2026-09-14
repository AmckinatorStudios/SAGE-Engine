// Реализация tinygltf развёрнута в render/Model.cpp — здесь только объявления
// (второе разворачивание дало бы дублирующиеся символы на линковке).
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include "sage/assets/import/GltfAccessor.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace sage::assets::gltf {
namespace {

int ComponentBytes(int componentType) {
    switch (componentType) {
        case TINYGLTF_COMPONENT_TYPE_BYTE:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  return 1;
        case TINYGLTF_COMPONENT_TYPE_SHORT:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return 2;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        case TINYGLTF_COMPONENT_TYPE_INT:
        case TINYGLTF_COMPONENT_TYPE_FLOAT:          return 4;
        case TINYGLTF_COMPONENT_TYPE_DOUBLE:         return 8;
        default: return 0;
    }
}

// Данные bufferView как отрезок байт. nullptr — индекс или границы не сошлись.
const unsigned char* ViewBytes(const tinygltf::Model& model, int viewIndex, size_t extraOffset,
                               size_t needed, size_t* strideOut, size_t defaultStride) {
    if (viewIndex < 0 || viewIndex >= (int)model.bufferViews.size()) return nullptr;
    const tinygltf::BufferView& view = model.bufferViews[(size_t)viewIndex];
    if (view.buffer < 0 || view.buffer >= (int)model.buffers.size()) return nullptr;
    const std::vector<unsigned char>& data = model.buffers[(size_t)view.buffer].data;

    // Границы считаем ПО bufferView, а не по всему буферу: view — это и есть
    // заявленный файлом кусок, и выход за него уже означает битую разметку.
    if (view.byteOffset > data.size()) return nullptr;
    const size_t viewBytes = std::min(view.byteLength, data.size() - view.byteOffset);
    if (extraOffset > viewBytes || needed > viewBytes - extraOffset) return nullptr;
    if (strideOut) *strideOut = defaultStride;
    return data.data() + view.byteOffset + extraOffset;
}

// Плотная копия аксессора «как лежит в файле»: count*components*compBytes байт.
// false — аксессор непригоден (нет, битый, запрошено больше компонент, чем в нём).
bool ReadDense(const tinygltf::Model& model, int accessorIndex, int components,
               std::vector<unsigned char>& out, int& componentType, bool& normalized,
               size_t& count) {
    if (accessorIndex < 0 || accessorIndex >= (int)model.accessors.size()) return false;
    const tinygltf::Accessor& acc = model.accessors[(size_t)accessorIndex];
    const int compBytes = ComponentBytes(acc.componentType);
    if (compBytes == 0 || components <= 0 || acc.count == 0) return false;

    // Запрошено больше компонент, чем в типе аксессора (VEC2 вместо VEC3) —
    // это чтение чужих байт, а не «недостающие нули». Отказываем.
    const int inType = tinygltf::GetNumComponentsInType((uint32_t)acc.type);
    if (inType <= 0 || components > inType) return false;

    componentType = acc.componentType;
    normalized = acc.normalized;
    count = acc.count;

    const size_t elemBytes = (size_t)compBytes * (size_t)components;
    const size_t inElemBytes = (size_t)compBytes * (size_t)inType;
    // Нули — база по спецификации: аксессор без bufferView целиком нулевой,
    // а sparse ниже подставит поверх то, что в нём есть.
    out.assign(acc.count * elemBytes, 0);

    if (acc.bufferView >= 0) {
        if (acc.bufferView >= (int)model.bufferViews.size()) return false;
        const tinygltf::BufferView& view = model.bufferViews[(size_t)acc.bufferView];
        const int rawStride = acc.ByteStride(view);
        const size_t stride = rawStride > 0 ? (size_t)rawStride : inElemBytes;
        if (stride < inElemBytes) return false;
        // Последний элемент обязан помещаться целиком.
        const size_t span = stride * (acc.count - 1) + inElemBytes;
        const unsigned char* base =
            ViewBytes(model, acc.bufferView, acc.byteOffset, span, nullptr, 0);
        if (!base) return false;
        for (size_t i = 0; i < acc.count; ++i)
            std::memcpy(out.data() + i * elemBytes, base + i * stride, elemBytes);
    }

    if (!acc.sparse.isSparse || acc.sparse.count <= 0) return true;

    // --- Подстановка sparse ---------------------------------------------
    // Номера вершин и значения лежат в СВОИХ bufferView и всегда плотно
    // (спецификация запрещает им byteStride).
    const size_t sparseCount = (size_t)acc.sparse.count;
    const int idxBytes = ComponentBytes(acc.sparse.indices.componentType);
    if (idxBytes == 0) return true; // блок битый — остаёмся на плотных данных
    const unsigned char* idxBase = ViewBytes(model, acc.sparse.indices.bufferView,
                                             acc.sparse.indices.byteOffset,
                                             sparseCount * (size_t)idxBytes, nullptr, 0);
    const unsigned char* valBase = ViewBytes(model, acc.sparse.values.bufferView,
                                             acc.sparse.values.byteOffset,
                                             sparseCount * inElemBytes, nullptr, 0);
    if (!idxBase || !valBase) return true;

    for (size_t s = 0; s < sparseCount; ++s) {
        uint32_t target = 0;
        switch (acc.sparse.indices.componentType) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: target = idxBase[s]; break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                uint16_t v = 0; std::memcpy(&v, idxBase + s * 2, sizeof(v)); target = v; break;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
                uint32_t v = 0; std::memcpy(&v, idxBase + s * 4, sizeof(v)); target = v; break;
            }
            default: continue;
        }
        // Номер вершины из файла — такое же непроверенное число, как и все
        // остальные: за границей массива он значит битый файл, а не запись
        // в чужую память.
        if ((size_t)target >= acc.count) continue;
        std::memcpy(out.data() + (size_t)target * elemBytes, valBase + s * inElemBytes, elemBytes);
    }
    return true;
}

float ToFloat(const unsigned char* p, int componentType, bool normalized) {
    switch (componentType) {
        case TINYGLTF_COMPONENT_TYPE_FLOAT: {
            float v = 0.0f; std::memcpy(&v, p, sizeof(v)); return v;
        }
        case TINYGLTF_COMPONENT_TYPE_DOUBLE: {
            double v = 0.0; std::memcpy(&v, p, sizeof(v)); return (float)v;
        }
        // Нормализованные целые — правило из спецификации glTF: так экспортёры
        // пакуют UV, цвета вершин и веса скина.
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
            const uint8_t v = *p;
            return normalized ? (float)v / 255.0f : (float)v;
        }
        case TINYGLTF_COMPONENT_TYPE_BYTE: {
            int8_t v = 0; std::memcpy(&v, p, sizeof(v));
            return normalized ? std::max((float)v / 127.0f, -1.0f) : (float)v;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
            uint16_t v = 0; std::memcpy(&v, p, sizeof(v));
            return normalized ? (float)v / 65535.0f : (float)v;
        }
        case TINYGLTF_COMPONENT_TYPE_SHORT: {
            int16_t v = 0; std::memcpy(&v, p, sizeof(v));
            return normalized ? std::max((float)v / 32767.0f, -1.0f) : (float)v;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
            uint32_t v = 0; std::memcpy(&v, p, sizeof(v)); return (float)v;
        }
        case TINYGLTF_COMPONENT_TYPE_INT: {
            int32_t v = 0; std::memcpy(&v, p, sizeof(v)); return (float)v;
        }
        default: return 0.0f;
    }
}

} // namespace

std::vector<float> ReadFloats(const tinygltf::Model& model, int accessorIndex, int components) {
    std::vector<unsigned char> raw;
    int componentType = 0;
    bool normalized = false;
    size_t count = 0;
    if (!ReadDense(model, accessorIndex, components, raw, componentType, normalized, count))
        return {};
    const size_t compBytes = (size_t)ComponentBytes(componentType);
    std::vector<float> out(count * (size_t)components);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = ToFloat(raw.data() + i * compBytes, componentType, normalized);
    return out;
}

std::vector<unsigned int> ReadUInts(const tinygltf::Model& model, int accessorIndex,
                                    int components) {
    std::vector<unsigned char> raw;
    int componentType = 0;
    bool normalized = false;
    size_t count = 0;
    if (!ReadDense(model, accessorIndex, components, raw, componentType, normalized, count))
        return {};
    const size_t compBytes = (size_t)ComponentBytes(componentType);
    std::vector<unsigned int> out(count * (size_t)components);
    for (size_t i = 0; i < out.size(); ++i) {
        const unsigned char* p = raw.data() + i * compBytes;
        switch (componentType) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: out[i] = *p; break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                uint16_t v = 0; std::memcpy(&v, p, sizeof(v)); out[i] = v; break;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
                uint32_t v = 0; std::memcpy(&v, p, sizeof(v)); out[i] = v; break;
            }
            case TINYGLTF_COMPONENT_TYPE_BYTE: {
                int8_t v = 0; std::memcpy(&v, p, sizeof(v)); out[i] = v < 0 ? 0u : (unsigned)v; break;
            }
            case TINYGLTF_COMPONENT_TYPE_SHORT: {
                int16_t v = 0; std::memcpy(&v, p, sizeof(v)); out[i] = v < 0 ? 0u : (unsigned)v; break;
            }
            default: out[i] = 0u; break;
        }
    }
    return out;
}

size_t AccessorCount(const tinygltf::Model& model, int accessorIndex) {
    if (accessorIndex < 0 || accessorIndex >= (int)model.accessors.size()) return 0;
    return model.accessors[(size_t)accessorIndex].count;
}

} // namespace sage::assets::gltf
