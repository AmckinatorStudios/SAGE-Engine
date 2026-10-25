#include "sage/assets/Pack.h"
#include <array>
#include <filesystem>
#include "ResourceManager.h"
#include "sage/core/EngineContext.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/assets/Quarantine.h"
#include "sage/assets/import/ModelProbe.h"
#include "sage/render/AlphaBleed.h"
#include "sage/render/SkinnedModel.h"

#include <stb_image.h> // реализация STB_IMAGE_IMPLEMENTATION живёт в Texture.cpp

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include "sage/render/ResourceManagerDetail.h"

using namespace resource_detail;

// Бюджет памяти кэша ресурсов: вытеснение, понижение текстур, сборка мусора, статистика (см. ResourceManager.h).

void ResourceManager::EvictToBudget() {
    if (m_textureBudget == 0 || m_textureBytes <= m_textureBudget) return;

    // Кандидаты: резидентные, не в полёте, не ссылаемые извне (use_count()==1 —
    // держит только кэш). Ссылающиеся текстуры не трогаем — это была бы висячая
    // ссылка у материала/UI.
    std::vector<EvictCandidate> cands;
    std::vector<std::string> keys;
    cands.reserve(m_textures.size());
    keys.reserve(m_textures.size());
    for (auto& kv : m_textures) {
        const TextureRecord& rec = kv.second;
        EvictCandidate c;
        c.Index = keys.size();
        c.Bytes = rec.Bytes;
        c.Tick = rec.Tick;
        c.Evictable = rec.Tex && !rec.Pending && !rec.Generated && rec.Tex.use_count() == 1;
        cands.push_back(c);
        keys.push_back(kv.first);
    }

    std::vector<size_t> toEvict = SelectEvictions(cands, m_textureBytes, m_textureBudget);
    for (size_t idx : toEvict) {
        auto it = m_textures.find(keys[idx]);
        if (it == m_textures.end()) continue;
        m_textureBytes -= std::min(m_textureBytes, it->second.Bytes);
        m_textures.erase(it);
        ++m_evictions;
    }

    // СТРИМИНГ МИПОВ. Вытеснение бессильно, когда все текстуры кому-то нужны:
    // выгрузить их нельзя (это висячая ссылка), и бюджет просто молча
    // превышается — то есть настройка «бюджет VRAM» не работает ровно в том
    // случае, ради которого её заводили, в тяжёлой сцене.
    //
    // Понижение разрешения работает и здесь: объект Texture остаётся жив, у
    // держателей ничего не рвётся, меняется только картинка внутри.
    if (!m_mipStreaming || m_textureBytes <= m_textureBudget) return;

    std::vector<DowngradeCandidate> dcands;
    dcands.reserve(m_textures.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        const TextureRecord& rec = m_textures.count(keys[i]) ? m_textures[keys[i]] : TextureRecord{};
        if (!rec.Tex) continue;
        DowngradeCandidate d;
        d.Index = i;
        d.Bytes = rec.Bytes;
        d.Tick = rec.Tick;
        d.Side = std::max(rec.Tex->Width(), rec.Tex->Height());
        d.Streamable = !rec.Pending && !rec.Generated;
        dcands.push_back(d);
    }
    for (size_t idx : SelectDowngrades(dcands, m_textureBytes, m_textureBudget)) {
        DowngradeTexture(keys[idx]);
    }
}

void ResourceManager::DowngradeTexture(const std::string& key) {
    auto it = m_textures.find(key);
    if (it == m_textures.end() || !it->second.Tex) return;
    TextureRecord& rec = it->second;
    // Пиксели читаются ПО ПУТИ записи: ключ теперь несёт ещё и настройки
    // выборки, и открыть его как файл нельзя.
    const std::string path = rec.Source;

    // Пиксели берём заново с диска: держать их копию в оперативной памяти ради
    // возможного понижения значило бы экономить видеопамять за счёт обычной,
    // то есть менять шило на мыло. Чтение синхронное — понижение случается
    // редко (только при исчерпанном бюджете) и стоит дешевле, чем срыв кадра
    // из-за нехватки видеопамяти.
    std::vector<unsigned char> pixels;
    int w = 0, h = 0;
    if (!DecodeImageFile(path, pixels, w, h)) return;
    if (rec.Bleed) sage::render::BleedTransparentColor(pixels, w, h);

    // Целевая сторона — вдвое меньше текущей, а не исходной: понижения
    // накапливаются, и второй проход должен уменьшать то, что уже уменьшено.
    int targetSide = std::max(rec.Tex->Width(), rec.Tex->Height()) / 2;
    targetSide = std::max(targetSide, 64);

    std::vector<unsigned char> scaled;
    int sw = w, sh = h;
    while (std::max(sw, sh) > targetSide && std::max(sw, sh) > 1) {
        DownscaleRGBA(pixels, sw, sh, scaled, sw, sh);
        pixels.swap(scaled);
    }

    const size_t before = rec.Bytes;
    rec.Tex->ReplacePixels(pixels.data(), sw, sh, rec.Filter, rec.Mipmaps);
    rec.Bytes = rec.Tex->GpuBytes();
    rec.MaxSide = std::max(sw, sh);
    m_textureBytes -= std::min(m_textureBytes, before);
    m_textureBytes += rec.Bytes;
    ++m_downgrades;
}

int ResourceManager::GarbageCollectUnused() {
    int freed = 0;

    // Материалы первыми: отпускают ссылки на свои текстуры, после чего те могут
    // стать неиспользуемыми и тоже выгрузиться этим же проходом.
    for (auto it = m_materials.begin(); it != m_materials.end();) {
        if (!it->second || it->second.use_count() == 1) {
            it = m_materials.erase(it);
            ++freed;
        } else {
            ++it;
        }
    }
    for (auto it = m_textures.begin(); it != m_textures.end();) {
        TextureRecord& rec = it->second;
        // Посчитанная текстура остаётся даже неиспользуемой: перечитать её
        // неоткуда, а собрана она была ровно затем, чтобы ею вот-вот покрасили
        // объект (материал заводят и назначают разными вызовами).
        const bool unused = !rec.Generated &&
                            (!rec.Tex || (!rec.Pending && rec.Tex.use_count() == 1));
        if (unused) {
            m_textureBytes -= std::min(m_textureBytes, rec.Bytes);
            it = m_textures.erase(it);
            ++freed;
        } else {
            ++it;
        }
    }
    for (auto it = m_models.begin(); it != m_models.end();) {
        if (!it->second || it->second.use_count() == 1) {
            it = m_models.erase(it);
            ++freed;
        } else {
            ++it;
        }
    }
    return freed;
}

ResourceManager::Stats ResourceManager::GetStats() const {
    Stats s;
    s.Downgrades = m_downgrades;
    s.Textures = m_textures.size();
    s.Models = m_models.size();
    s.Materials = m_materials.size();
    s.TextureBytes = m_textureBytes;
    s.TextureBudget = m_textureBudget;
    s.PendingAsync = m_async ? (size_t)std::max(0, m_async->Pending.load()) : 0;
    s.Evictions = m_evictions;
    return s;
}
