#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sage/ui/core/UIContext.h"

class Font;
class UIRenderer;

// ---------------------------------------------------------------------------
// МОСТ К РЕСУРСАМ ДВИЖКА (§138–140 ТЗ).
//
// Интерфейс не заводит своего менеджера шрифтов и текстур — он пользуется тем,
// что уже есть в движке. Но и знать про него напрямую он не должен: иначе
// раскладку текста нельзя ни протестировать без GPU, ни переиспользовать в
// инструменте.
//
// Здесь — переходники: реализации IUIFontSource и IUITextureSource поверх
// шрифтов и ResourceManager движка. Их создаёт ТОТ, КТО РИСУЕТ (игра, редактор,
// плеер), и кладёт в контекст.
// ---------------------------------------------------------------------------
namespace sage::ui {

// Шрифты поверх Font движка. Своя таблица открытых шрифтов: интерфейсу нужны
// несколько кеглей и начертаний одновременно, а Font — это один запечённый
// атлас.
class UIEngineFonts : public IUIFontSource {
public:
    // fallbackPath — шрифт по умолчанию; пусто — берётся тот же путь, что у
    // штатного UIRenderer.
    explicit UIEngineFonts(std::string fallbackPath = {});
    ~UIEngineFonts() override;

    int Resolve(const std::string& family, int weight, bool italic) override;
    int Fallback() const override { return m_fallback; }
    UIFontMetrics Metrics(int font) const override;
    float Advance(int font, uint32_t codepoint) const override;
    bool HasGlyph(int font, uint32_t codepoint) const override;

    // Шрифт по ручке — нужен бэкенду рисования, чтобы взять атлас.
    const Font* Get(int handle) const;

private:
    int Open(const std::string& path);

    struct Entry {
        std::string Path;
        std::unique_ptr<Font> Data;
    };
    std::vector<Entry> m_fonts;
    std::unordered_map<std::string, int> m_byPath;
    int m_fallback = -1;
};

// Текстуры поверх ResourceManager. Кэш «путь → указатель» здесь свой и
// намеренно тонкий: сама текстура живёт в менеджере ресурсов, здесь только
// разрешение имени, потому что интерфейс спрашивает его на каждый кадр.
class UIEngineTextures : public IUITextureSource {
public:
    const Texture* Get(const std::string& path) override;
    glm::ivec2 Size(const std::string& path) override;
    void Clear() { m_cache.clear(); }

private:
    std::unordered_map<std::string, std::shared_ptr<Texture>> m_cache;
};

// Готовый контекст под текущий кадр движка. Собирает вместе шрифты, текстуры и
// перевод строк, чтобы каждый вызывающий не собирал это по кускам.
struct UIEngineResources {
    UIEngineFonts Fonts;
    UIEngineTextures Textures;

    void Install(UIContext& ctx) {
        ctx.Fonts = &Fonts;
        ctx.Textures = &Textures;
    }
};

} // namespace sage::ui
