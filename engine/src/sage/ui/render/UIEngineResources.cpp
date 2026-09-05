#include "sage/ui/render/UIEngineResources.h"

#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/render/Font.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Texture.h"

namespace sage::ui {

namespace {
// Те же кандидаты, что у штатного UIRenderer: интерфейс не должен выглядеть
// по-разному в зависимости от того, кто его рисует.
const char* kDefaultFonts[] = {
    "assets/fonts/sage-default.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "C:/Windows/Fonts/segoeui.ttf",
    "C:/Windows/Fonts/arial.ttf",
};
// Кегль запекания. Раскладка работает в долях кегля, поэтому одного
// запечённого размера хватает на все размеры текста — он лишь задаёт качество.
constexpr float kBakePixels = 64.0f;
} // namespace

UIEngineFonts::UIEngineFonts(std::string fallbackPath) {
    if (!fallbackPath.empty()) m_fallback = Open(fallbackPath);
    if (m_fallback < 0) {
        for (const char* p : kDefaultFonts) {
            m_fallback = Open(sage::EngineAssetPath(p));
            if (m_fallback >= 0) break;
        }
    }
    if (m_fallback < 0)
        LOG_WARN("UI") << "шрифт по умолчанию не найден — текст интерфейса не будет измерен";
}

UIEngineFonts::~UIEngineFonts() = default;

int UIEngineFonts::Open(const std::string& path) {
    auto it = m_byPath.find(path);
    if (it != m_byPath.end()) return it->second;
    std::unique_ptr<Font> font;
    try {
        font = Font::Load(path, kBakePixels);
    } catch (const std::exception& e) {
        LOG_DEBUG("UI") << "шрифт не открылся (" << path << "): " << e.what();
        return -1;
    }
    if (!font) return -1;
    m_fonts.push_back({path, std::move(font)});
    const int handle = (int)m_fonts.size() - 1;
    m_byPath[path] = handle;
    return handle;
}

int UIEngineFonts::Resolve(const std::string& family, int weight, bool italic) {
    (void)weight;
    (void)italic;
    if (family.empty()) return m_fallback;
    const int handle = Open(family);
    // Отсутствующий шрифт — не пустой экран, а запасной вариант (§134). Игра
    // получает предупреждение в лог один раз, при первой попытке.
    return handle >= 0 ? handle : m_fallback;
}

const Font* UIEngineFonts::Get(int handle) const {
    if (handle < 0 || handle >= (int)m_fonts.size()) return nullptr;
    return m_fonts[(size_t)handle].Data.get();
}

UIFontMetrics UIEngineFonts::Metrics(int font) const {
    UIFontMetrics m;
    const Font* f = Get(font);
    if (!f) {
        // Разумные значения вместо нулей: без них текст «занимает ноль» и
        // раскладка схлопывается вместо того, чтобы просто выглядеть иначе.
        m.LineHeight = 1.2f;
        m.Ascent = 0.8f;
        m.Descent = 0.2f;
        return m;
    }
    const float px = f->PixelHeight() > 0.0f ? f->PixelHeight() : 1.0f;
    m.LineHeight = f->LineHeight(1.0f) / px;
    m.Ascent = f->Ascent(1.0f) / px;
    m.Descent = m.LineHeight - m.Ascent;
    return m;
}

float UIEngineFonts::Advance(int font, uint32_t codepoint) const {
    const Font* f = Get(font);
    if (!f) return 0.5f; // моноширинное приближение: лучше, чем ноль
    std::string s;
    if (codepoint < 0x80) s.push_back((char)codepoint);
    else if (codepoint < 0x800) {
        s.push_back((char)(0xC0 | (codepoint >> 6)));
        s.push_back((char)(0x80 | (codepoint & 0x3F)));
    } else if (codepoint < 0x10000) {
        s.push_back((char)(0xE0 | (codepoint >> 12)));
        s.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (codepoint & 0x3F)));
    } else {
        s.push_back((char)(0xF0 | (codepoint >> 18)));
        s.push_back((char)(0x80 | ((codepoint >> 12) & 0x3F)));
        s.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (codepoint & 0x3F)));
    }
    const float px = f->PixelHeight() > 0.0f ? f->PixelHeight() : 1.0f;
    return f->MeasureWidth(s, 1.0f) / px;
}

bool UIEngineFonts::HasGlyph(int font, uint32_t codepoint) const {
    const Font* f = Get(font);
    return f && f->HasGlyph(codepoint);
}

const Texture* UIEngineTextures::Get(const std::string& path) {
    if (path.empty()) return nullptr;
    auto it = m_cache.find(path);
    if (it != m_cache.end()) return it->second.get();
    std::shared_ptr<Texture> tex = ResourceManager::Instance().GetTexture(path);
    m_cache[path] = tex; // в том числе пустой указатель: не искать снова каждый кадр
    return tex.get();
}

glm::ivec2 UIEngineTextures::Size(const std::string& path) {
    const Texture* t = Get(path);
    return t ? glm::ivec2(t->Width(), t->Height()) : glm::ivec2(0);
}

} // namespace sage::ui
