#pragma once
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "sage/rhi/GraphicsDevice.h"

// ---------------------------------------------------------------------
// ShadowMap — тени от направленного света («солнца»). Сцена рисуется по
// глубине из точки зрения солнца в depth-текстуру (проход теней), затем
// основные шейдеры сэмплируют её и решают, в тени ли фрагмент. Мягкие края
// даёт PCF-фильтрация в шейдере-приёмнике.
//
// ЗАЧЕМ КАСКАДЫ. Одна карта на всю видимую даль — это компромисс, который
// нельзя выиграть. Разрешение карты фиксировано, поэтому размер текселя в
// метрах = 2·radius / resolution. Для комнаты (radius 10 м, карта 2048)
// тексель ~1 см, и тень выглядит отлично. Для улицы (radius 200 м) тот же
// тексель — 20 см: тень от столба под ногами превращается в лесенку из
// квадратов, и никакой фильтрацией это не лечится — информации просто нет.
//
// Каскады решают это тем, что карт несколько, и каждая накрывает свой
// диапазон дальности. Ближний каскад — маленький и подробный, дальний —
// большой и грубый, но он и виден издалека, где грубость незаметна. Тексель
// в метрах перестаёт быть одним числом на весь кадр и следует за тем, как
// быстро объект уменьшается на экране.
//
// ЦЕНА. Проход глубины выполняется по одному разу на каскад, то есть вся
// отбрасывающая тень геометрия рисуется N раз. Это честная плата, и поэтому
// каскадов по умолчанию три, а не восемь: четвёртый добавляет проход ради
// дальности, которую в кадре обычно занимает небо.
//
// ПОЧЕМУ ГРАНИЦЫ КАСКАДОВ СЧИТАЮТСЯ ПО СФЕРЕ, А НЕ ПО КОРОБКЕ. Ортобокс,
// натянутый на углы под-фрустума, меняет размер при повороте камеры: одна и
// та же геометрия то влезает, то нет, размер текселя скачет, и тени «кипят»
// просто от вращения мышью. Радиус описанной сферы зависит только от
// дальностей и угла обзора — от поворота нет. Поэтому сфера.
//
// Использование за кадр (каскадный путь):
//   ShadowMap::CameraView v{pos, forward, up, fovY, aspect, 0.1f, 120.0f};
//   shadows.SetCascades(sun.Direction, v);
//   for (int c = 0; c < shadows.CascadeCount(); ++c) {
//       shadows.BeginRender(c);
//       depthShader.Use(); depthShader.SetMat4("uLightSpace", shadows.LightMatrix(c));
//       ...рисуем отбрасывающую тень геометрию...
//   }
//   shadows.EndRender(window.Width(), window.Height());
//   ...в основном проходе: UploadShadowUniforms(shader, shadows, enabled)...
//
// Одна карта (комната, редактор, превью) — по-прежнему одной строкой:
//   shadows.SetLightMatrix(sun.Direction, center, radius);
// ---------------------------------------------------------------------
class ShadowMap {
public:
    // Больше четырёх каскадов не бывает по делу: каждый — это ещё один полный
    // проход геометрии, а выигрыш в детализации падает вдвое с каждым шагом.
    // Число зафиксировано и в шейдере (см. PbrShader.h): GLSL 330 не умеет
    // индексировать массив сэмплеров переменной, и выбор каскада там развёрнут
    // вручную.
    static constexpr int kMaxCascades = 4;

    // Параметры камеры, для которой строятся каскады. Перспективная: каскады
    // существуют именно потому, что у перспективы плотность пикселей на метр
    // падает с дальностью. Для ортографической камеры это не так, и там
    // правильный ответ — одна карта (см. SetLightMatrix).
    struct CameraView {
        glm::vec3 Position{0.0f};
        glm::vec3 Forward{0.0f, 0.0f, -1.0f};
        glm::vec3 Up{0.0f, 1.0f, 0.0f};
        float FovY = glm::radians(60.0f);
        float Aspect = 16.0f / 9.0f;
        float Near = 0.1f;
        // Дальность теней, а НЕ far камеры. Тени за горизонтом событий никому
        // не нужны, а растягивать на них каскады — значит терять разрешение
        // там, где на тень смотрят.
        float ShadowDistance = 120.0f;
    };

    explicit ShadowMap(int resolution = 2048, int cascades = 1)
        : m_resolution(resolution), m_cascades(std::clamp(cascades, 1, kMaxCascades)) {
        sage::rhi::RenderTargetDesc desc;
        desc.Width = resolution;
        desc.Height = resolution;
        desc.Kind = sage::rhi::RenderTargetKind::DepthOnly;
        for (int i = 0; i < m_cascades; ++i) {
            m_targets.push_back(sage::rhi::GraphicsDevice::Get().CreateRenderTarget(desc));
        }
        m_lightMatrix.assign((size_t)m_cascades, glm::mat4(1.0f));
        m_radius.assign((size_t)m_cascades, 1.0f);
    }

    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;
    ShadowMap(ShadowMap&&) noexcept = default;
    ShadowMap& operator=(ShadowMap&&) noexcept = default;

    int CascadeCount() const { return m_cascades; }
    int Resolution() const { return m_resolution; }

    // Одна карта на всё: ортобокс с полустороной radius, отцентрованный на
    // center. sunDir — направление, КУДА летит свет. Остаётся основным путём
    // для сцен, где вся геометрия рядом (комната, превью, редактор): каскады
    // там дали бы три прохода ради одинаковой картинки.
    //
    // Если карт заведено несколько, лишние каскады получают ту же матрицу —
    // шейдер найдёт фрагмент в нулевом и до остальных не дойдёт.
    void SetLightMatrix(glm::vec3 sunDir, glm::vec3 center, float radius) {
        const glm::mat4 m = FitOrtho(sunDir, center, radius);
        for (int i = 0; i < m_cascades; ++i) {
            m_lightMatrix[(size_t)i] = m;
            m_radius[(size_t)i] = radius;
        }
        m_activeCascades = 1; // остальные — копии, шейдеру они не нужны
    }

    // Каскадный путь: разбивает дальность камеры на m_cascades диапазонов и
    // подгоняет под каждый свою карту.
    //
    // lambda — как делить дальность. 0 — равными кусками (дальним каскадам
    // хорошо, ближним плохо), 1 — логарифмически (наоборот). Практика давно
    // сошлась на середине: около 0.6 ближний каскад получает подробность, а
    // дальний не вырождается в полоску у горизонта.
    void SetCascades(glm::vec3 sunDir, const CameraView& view, float lambda = 0.6f) {
        const float nearZ = std::max(view.Near, 0.01f);
        const float farZ = std::max(view.ShadowDistance, nearZ * 2.0f);

        const glm::vec3 forward = glm::normalize(view.Forward);
        glm::vec3 right = glm::cross(forward, view.Up);
        // Вырожденный случай: смотрим точно вдоль up. Берём любую ось,
        // перпендикулярную forward, — на размер сферы это не влияет.
        if (glm::dot(right, right) < 1e-8f) right = glm::vec3(1.0f, 0.0f, 0.0f);
        right = glm::normalize(right);
        const glm::vec3 up = glm::cross(right, forward);

        const float tanH = std::tan(view.FovY * 0.5f);
        const float tanW = tanH * view.Aspect;

        float splitNear = nearZ;
        for (int i = 0; i < m_cascades; ++i) {
            const float splitFar = SplitDistance(i, m_cascades, nearZ, farZ, lambda);

            // Центр описанной сферы под-фрустума лежит на оси взгляда. Ищем
            // его аналитически: точка, равноудалённая от ближнего и дальнего
            // колец. Если формула выводит центр за пределы отрезка (широкий
            // угол, короткий кусок), сфера строится вокруг дальнего кольца —
            // она его и накрывает.
            const float n = splitNear, f = splitFar;
            const float k2 = tanH * tanH + tanW * tanW;
            float center = 0.5f * (n + f) * (1.0f + k2);
            float radius;
            if (center >= f) {
                center = f;
                radius = f * std::sqrt(k2);
            } else {
                const float dx = f - center;
                radius = std::sqrt(dx * dx + f * f * k2);
            }

            const glm::vec3 worldCenter = view.Position + forward * center;
            m_lightMatrix[(size_t)i] = FitOrtho(sunDir, worldCenter, radius);
            m_radius[(size_t)i] = radius;
            splitNear = splitFar;
        }
        m_activeCascades = m_cascades;
    }

    // Размер текселя карты в метрах — по нему видно, откуда берётся качество:
    // вдвое меньший радиус даёт вдвое более мелкий тексель при том же
    // разрешении. Открыто наружу ради диагностики и проверок.
    float TexelWorldSize(float radius) const { return (radius * 2.0f) / (float)m_resolution; }
    // Размер текселя КОНКРЕТНОГО каскада — то, что реально определяет
    // качество тени на его дальности.
    float CascadeTexelSize(int cascade) const {
        return TexelWorldSize(m_radius[(size_t)Clamp(cascade)]);
    }
    float CascadeRadius(int cascade) const { return m_radius[(size_t)Clamp(cascade)]; }

    void BeginRender(int cascade = 0) {
        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        m_targets[(size_t)Clamp(cascade)]->Bind(); // FBO + viewport под разрешение карты
        device.Clear(/*color=*/false, /*depth=*/true);
        // Отсечение ЛИЦЕВЫХ граней в проходе глубины (вместо задних):
        // классический приём против shadow acne — в карту пишется задняя
        // стенка объекта, а лицевую (освещённую) сравнение уже не самозатеняет.
        // Работает для замкнутых тел (воксельные кубы, меши-кубы, .obj).
        device.SetCullMode(sage::rhi::CullMode::Front);
    }

    void EndRender(int screenW, int screenH) {
        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        device.SetCullMode(sage::rhi::CullMode::Back);
        device.BindDefaultFramebuffer();
        device.SetViewport(0, 0, screenW, screenH);
    }

    // Нативный хендл depth-текстуры — для привязки шейдерам-приёмникам
    // через GraphicsDevice::BindTexture2D.
    unsigned int DepthTexture(int cascade = 0) const {
        return m_targets[(size_t)Clamp(cascade)]->DepthTextureHandle();
    }
    const glm::mat4& LightMatrix(int cascade = 0) const {
        return m_lightMatrix[(size_t)Clamp(cascade)];
    }
    // Сколько каскадов реально несут разную информацию: после SetLightMatrix
    // это 1, даже если карт заведено больше. Шейдеру важно именно это число.
    int ActiveCascades() const { return m_activeCascades; }

private:
    int Clamp(int cascade) const { return std::clamp(cascade, 0, m_cascades - 1); }

    // Граница i-го каскада: смесь логарифмического и равномерного деления.
    static float SplitDistance(int i, int count, float nearZ, float farZ, float lambda) {
        const float p = (float)(i + 1) / (float)count;
        const float logSplit = nearZ * std::pow(farZ / nearZ, p);
        const float uniSplit = nearZ + (farZ - nearZ) * p;
        return lambda * logSplit + (1.0f - lambda) * uniSplit;
    }

    // Ортобокс света вокруг сферы (center, radius) с привязкой к сетке текселей.
    glm::mat4 FitOrtho(glm::vec3 sunDir, glm::vec3 center, float radius) const {
        glm::vec3 dir = glm::normalize(sunDir);
        // Безопасный up: когда солнце почти в зените (dir ~ вертикаль),
        // обычный up (0,1,0) вырождается — берём горизонтальный.
        glm::vec3 up = (glm::abs(dir.y) > 0.99f) ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                 : glm::vec3(0.0f, 1.0f, 0.0f);

        // ПРИВЯЗКА К СЕТКЕ ТЕКСЕЛЕЙ.
        //
        // Ортобокс двигается вслед за сценой, и без привязки его край каждый
        // кадр попадает между текселями. Тень при этом «кипит»: одни и те же
        // кромки перескакивают на тексель туда-обратно, и в движении это
        // заметно сильнее, чем сама пикселизация. Лечится тем, что центр бокса
        // округляется до целого числа текселей — тогда содержимое карты
        // сдвигается ровно на тексель, а не на его долю.
        const float texelWorld = (radius * 2.0f) / (float)m_resolution;
        {
            const glm::mat4 view = glm::lookAt(center - dir, center, up);
            glm::vec3 inLight = glm::vec3(view * glm::vec4(center, 1.0f));
            inLight.x = std::floor(inLight.x / texelWorld) * texelWorld;
            inLight.y = std::floor(inLight.y / texelWorld) * texelWorld;
            center = glm::vec3(glm::inverse(view) * glm::vec4(inLight, 1.0f));
        }

        // «Камеру света» отодвигаем назад по лучу, чтобы видеть весь бокс
        // снаружи; far берём с запасом на высоту геометрии над центром.
        const glm::vec3 eye = center - dir * (radius * 2.0f);
        const glm::mat4 lightView = glm::lookAt(eye, center, up);
        const glm::mat4 lightProj =
            glm::ortho(-radius, radius, -radius, radius, 0.1f, radius * 4.0f);
        return lightProj * lightView;
    }

    std::vector<std::unique_ptr<sage::rhi::RenderTarget>> m_targets;
    std::vector<glm::mat4> m_lightMatrix;
    std::vector<float> m_radius;
    int m_resolution;
    int m_cascades;
    int m_activeCascades = 1;
};

// Текстурные юниты каскадов. Нулевой сидит на юните 1 с тех пор, как тени
// вообще появились, и там и остаётся: сдвинуть его значило бы трогать все
// шейдеры и все игры. Остальные уезжают за лайтмапу (юнит 9), чтобы не
// столкнуться с картами материала (0..5) и GI-объёмом (6..8).
constexpr int kShadowUnit0 = 1;
constexpr int kShadowUnitRest = 10; // каскад 1 -> 10, 2 -> 11, 3 -> 12

inline int ShadowCascadeUnit(int cascade) {
    return cascade == 0 ? kShadowUnit0 : kShadowUnitRest + (cascade - 1);
}

// ---------------------------------------------------------------------
// ShadowBinding — всё, что нужно шейдеру-приёмнику, чтобы посчитать тень:
// матрицы каскадов, их текстуры и признак «тени включены».
//
// ЗАЧЕМ ОТДЕЛЬНЫЙ ТИП. Раньше это ездило по функциям отрисовки тремя
// параметрами подряд (матрица, хендл текстуры, флаг). С каскадами их стало бы
// девять, и каждый вызов превратился бы в место, где легко перепутать порядок
// или забыть каскад. Здесь они связаны, и рассинхронизировать матрицу с её
// текстурой больше нечем.
// ---------------------------------------------------------------------
struct ShadowBinding {
    glm::mat4 Matrices[ShadowMap::kMaxCascades];
    unsigned int Textures[ShadowMap::kMaxCascades] = {0, 0, 0, 0};
    int Count = 1;
    bool Enabled = false;

    ShadowBinding() {
        for (glm::mat4& m : Matrices) m = glm::mat4(1.0f);
    }

    // Одна карта — частный случай каскадов с Count == 1.
    ShadowBinding(const glm::mat4& matrix, unsigned int texture, bool enabled) : ShadowBinding() {
        Matrices[0] = matrix;
        Textures[0] = texture;
        Enabled = enabled;
    }

    ShadowBinding(const ShadowMap& shadows, bool enabled) : ShadowBinding() {
        Count = shadows.ActiveCascades();
        for (int i = 0; i < Count; ++i) {
            Matrices[i] = shadows.LightMatrix(i);
            Textures[i] = shadows.DepthTexture(i);
        }
        Enabled = enabled;
    }

    // Привязывает карты к их юнитам. Незаполненные каскады получают текстуру
    // последнего: сэмплер обязан указывать на живую текстуру, иначе драйвер
    // вправе вернуть из непривязанного юнита что угодно — и обычно возвращает
    // чёрное, то есть всю сцену в тени.
    void Bind() const {
        if (!Enabled) return;
        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        for (int i = 0; i < ShadowMap::kMaxCascades; ++i) {
            const unsigned int tex = Textures[std::min(i, std::max(Count - 1, 0))];
            if (tex) device.BindTexture2D(ShadowCascadeUnit(i), tex);
        }
    }
};
