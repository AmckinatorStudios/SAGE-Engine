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

// Материалы в кэше ресурсов: регистрация, загрузка, перезагрузка, текстуры материала (см. ResourceManager.h).

void ResourceManager::RegisterMaterial(const std::string& name, std::shared_ptr<Material> material) {
    if (name.empty() || !material) return;
    const std::string key = CacheKey(name);
    // Тот же объект, если имя уже занято: объекты, которым материал назначен,
    // держат указатель — заменить запись значило бы оставить их со старым.
    auto it = m_materials.find(key);
    if (it != m_materials.end() && it->second) *it->second = *material;
    else m_materials[key] = material;
    m_materialStamps.erase(key);   // не файл — перечитывать с диска нечего
    ResolveMaterialTextures(*m_materials[key]);
}

std::shared_ptr<Material> ResourceManager::GetMaterial(const std::string& path) {
    const std::string key = CacheKey(path);
    auto it = m_materials.find(key);
    if (it != m_materials.end()) {
        // ПУТИ МОГЛИ ПОМЕНЯТЬСЯ ПОСЛЕ ЗАГРУЗКИ — и меняет их не одно место (см.
        // Material::ResolvedFrom). Раньше кэш отдавал материал как есть, с
        // указателями от прежних путей: назначенная кнопкой «Обзор…» текстура
        // не появлялась НИКОГДА, потому что перерезолвить её было некому.
        // Сравнение шести строк дешевле любой попытки помнить это правило
        // руками — и, в отличие от правила, не забывается.
        RefreshMaterialTextures(*it->second);
        return it->second;
    }
    auto material = std::make_shared<Material>();
    try {
        *material = Material::LoadFromFile(Locate(path));
    } catch (const std::exception& e) {
        LOG_ERROR("Resources") << "Материал не загрузился, использую дефолт: " << e.what();
    }
    ResolveMaterialTextures(*material);
    if (material->HasCustomShader())
        material->ShaderPtr = GetShader(material->VertexShaderPath, material->FragmentShaderPath);
    m_materials[key] = material;
    m_materialStamps[key] = FileStamp(Locate(path));
    return material;
}

std::shared_ptr<Material> ResourceManager::MakeMaterial(const std::string& name) {
    // КЛЮЧ — ТОТ ЖЕ, ПО КОТОРОМУ ИЩЕТ GetMaterial. Материал лежал под именем
    // как его дали, а искался по CacheKey — и они совпадали только случайно:
    // на macOS (libc++) weakly_canonical делает из «test/lantern» абсолютный
    // путь, и SetMaterial вместо собранного скриптом материала назначал
    // пустой. То же везде с именем вида «./a» или «a/../b».
    const std::string key = CacheKey(name);
    auto it = m_materials.find(key);
    if (it != m_materials.end()) return it->second;
    auto material = std::make_shared<Material>();
    m_materials[key] = material;
    // Штамп НЕ ставим: у материала нет файла, и ReloadChangedAssets, увидев
    // штамп 0 против несуществующего файла, перечитывал бы его каждый кадр,
    // затирая всё, что скрипт в нём настроил.
    return material;
}

std::shared_ptr<Material> ResourceManager::ReloadMaterial(const std::string& path) {
    auto it = m_materials.find(CacheKey(path));
    if (it == m_materials.end()) return GetMaterial(path);
    try {
        *it->second = Material::LoadFromFile(Locate(path));
    } catch (const std::exception& e) {
        LOG_ERROR("Resources") << "Перезагрузка материала не удалась: " << e.what();
    }
    ResolveMaterialTextures(*it->second);
    if (it->second->HasCustomShader())
        it->second->ShaderPtr = GetShader(it->second->VertexShaderPath, it->second->FragmentShaderPath);
    return it->second;
}

// Карты материала — АНИЗОТРОПНЫЕ, а не трилинейные.
//
// Поддержка анизотропии в движке была с самого начала: и режим фильтрации, и
// запрос лимита у драйвера, и применение в бэкенде. Не было одного — её никто
// не просил: каждая карта грузилась трилинейной, потому что таково значение
// параметра по умолчанию. То есть код работал вхолостую, а пол под острым
// углом мылился на любой видеокарте — включая те, где резкость досталась бы
// даром.
//
// Почему именно карты материала, а не все текстуры подряд: анизотропия имеет
// смысл там, где поверхность видна ПОД УГЛОМ и сжата сильнее по одной оси, —
// это пол, стены, дорога. Картинки интерфейса всегда фронтальны, и платить за
// них нечем и незачем.
//
// Карта без расширения получает трилинейную фильтрацию сама (см.
// EffectiveFilter в Texture.cpp) — запрос не отказывает, а опускается.
//
// Фильтрация «ближайший пиксель» (MaterialRender::Filter) — Nearest С
// мипмапами: вблизи чёткие квадраты, вдали — уменьшенные копии, а не рябь.
void ResourceManager::ResolveMaterialTextures(Material& m) {
    const TextureFilter kSurface = m.Render.Filter == MaterialRender::Filtering::Nearest
                                       ? TextureFilter::Nearest
                                       : TextureFilter::Anisotropic;
    // Цвет под прозрачностью правится только у карты, которую материал режет
    // по альфе (см. AlphaBleed.h): у остальных альфа значит что-то своё.
    const bool cutout = m.Render.AlphaCutoff > 0.0f;
    auto get = [&](const std::string& path, bool bleed, bool colour) {
        return m_streamMaterialTextures ? GetTextureAsync(path, kSurface, true, bleed, colour)
                                        : GetTexture(path, kSurface, true, bleed, colour);
    };
    // ЦВЕТ — в sRGB, ДАННЫЕ — как есть. Альбедо и свечение — это картинки,
    // нарисованные для глаза (sRGB); нормали, металличность, шероховатость и
    // затенение — числа, и переводить их нельзя: нормаль «исказилась бы».
    m.AlbedoTex = get(m.TexturePath, cutout, true);
    m.NormalTex = get(m.NormalMapPath, false, false);
    m.MetallicTex = get(m.MetallicMapPath, false, false);
    m.RoughnessTex = get(m.RoughnessMapPath, false, false);
    m.AOTex = get(m.AOMapPath, false, false);
    m.EmissiveTex = get(m.EmissiveMap, false, true);
    m.TexturesFrom = PathsOf(m);
    m.TexturesCutout = cutout;
    m.TexturesFilter = m.Render.Filter;
}

// Слепок путей к картам — по нему видно, что указатели устарели.
Material::ResolvedFrom ResourceManager::PathsOf(const Material& m) {
    Material::ResolvedFrom f;
    f.Albedo = m.TexturePath;
    f.Normal = m.NormalMapPath;
    f.Metallic = m.MetallicMapPath;
    f.Roughness = m.RoughnessMapPath;
    f.AO = m.AOMapPath;
    f.Emissive = m.EmissiveMap;
    return f;
}

bool ResourceManager::RefreshMaterialTextures(Material& m) {
    // Включили или сняли вырез — карта альбедо нужна другая (с цветом под
    // прозрачностью или без), хотя пути не менялись.
    if (m.TexturesFrom == PathsOf(m) && m.TexturesCutout == (m.Render.AlphaCutoff > 0.0f) &&
        m.TexturesFilter == m.Render.Filter)
        return false;
    ResolveMaterialTextures(m);
    return true;
}
