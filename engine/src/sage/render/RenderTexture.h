#pragma once
#include <memory>
#include <string>
#include <unordered_map>

#include <glm/glm.hpp>

#include "sage/render/Framebuffer.h"
#include "sage/render/Texture.h"

// ---------------------------------------------------------------------------
// Рендер-текстуры: движок рисует картинку САМ и отдаёт её как обычную текстуру.
//
// Нужны везде, где изображение не лежит файлом на диске: экран камеры
// наблюдения, зеркало, портал, карта местности, портрет персонажа в интерфейсе
// и объёмная иконка предмета вместо плоского значка.
//
// До сих пор такой возможности не было вовсе. Рендер-таргеты у движка есть
// давно, но они внутренние (тени, пост-обработка), безымянные и живут внутри
// своих систем; ни интерфейс, ни материал сослаться на них не мог. Здесь у
// картинки есть ИМЯ, а по имени её берут и UI (Image с путём "rt:<имя>"), и
// скрипт.
// ---------------------------------------------------------------------------
namespace sage::render {

class RenderTexture {
public:
    RenderTexture(const std::string& name, int width, int height);

    // Делает таргет активным и чистит его заданным цветом.
    // Прозрачный фон (alpha 0) — то, что нужно иконке: она ложится на любой
    // интерфейс, а не таскает с собой квадрат подложки.
    void Begin(const glm::vec4& clearColor);
    // Сводит MSAA (если был) и возвращает отрисовку в буфер по умолчанию.
    void End();

    const std::string& Name() const { return m_name; }
    int Width() const { return m_fbo.Width(); }
    int Height() const { return m_fbo.Height(); }
    Framebuffer& Target() { return m_fbo; }

    // Текстура для интерфейса и материалов. Одна и та же на всё время жизни:
    // держатели shared_ptr видят свежий кадр без повторного запроса.
    const std::shared_ptr<Texture>& AsTexture() const { return m_view; }

    void Resize(int width, int height);

private:
    std::string m_name;
    Framebuffer m_fbo;
    std::shared_ptr<Texture> m_view;
};

// Именованный набор. Один на процесс: имя картинки — общий язык между
// скриптом, интерфейсом и материалом, и двух разных «rt:portrait» быть не
// должно.
class RenderTextureRegistry {
public:
    static RenderTextureRegistry& Instance();

    RenderTexture& GetOrCreate(const std::string& name, int width, int height);
    RenderTexture* Find(const std::string& name);
    void Remove(const std::string& name);
    void Clear();

    // Разбирает путь вида "rt:<имя>" и отдаёт текстуру, если такая есть.
    // Возвращает пустой указатель для обычных путей — вызывающий продолжит
    // грузить файл как раньше.
    static std::shared_ptr<Texture> Resolve(const std::string& path);

private:
    std::unordered_map<std::string, std::unique_ptr<RenderTexture>> m_items;
};

} // namespace sage::render
