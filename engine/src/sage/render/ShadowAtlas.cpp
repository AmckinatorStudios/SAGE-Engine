#include "sage/render/ShadowAtlas.h"

#include <cmath>

namespace sage::render {

sage::rhi::TextureHandle DummyShadowTexture() {
    // Живёт до конца процесса: текстура в один пиксель, зато сэмплеры никогда
    // не смотрят в пустоту.
    // Указатель, а НЕ unique_ptr: статический объект уничтожается при выходе из
    // процесса, когда графического контекста уже нет, и удаление текстуры в
    // этот момент роняет программу (тот же капкан, из-за которого у панелей
    // редактора появился Shutdown). Один пиксель, живущий до конца процесса, —
    // честная плата за то, чтобы не падать на выходе.
    static sage::rhi::Texture2D* white = [] {
        sage::rhi::Texture2DDesc desc;
        desc.Width = 1;
        desc.Height = 1;
        desc.Channels = 4;
        desc.FilterMode = sage::rhi::Filter::Nearest;
        desc.WrapMode = sage::rhi::Wrap::ClampEdge;
        desc.GenerateMipmaps = false;
        const unsigned char pixel[4] = {255, 255, 255, 255};
        return sage::rhi::GraphicsDevice::Get().CreateTexture2D(desc, pixel).release();
    }();
    return white ? white->Handle() : sage::rhi::TextureHandle{};
}


int PointFaceUV(const glm::vec3& v, glm::vec2& outUV) {
    const glm::vec3 a = glm::abs(v);
    float axis;
    int face;
    glm::vec2 sc; // координаты в плоскости грани (камерные x/y до деления)
    if (a.x >= a.y && a.x >= a.z) {
        axis = a.x;
        if (v.x > 0.0f) { face = 0; sc = glm::vec2(-v.z, -v.y); }
        else             { face = 1; sc = glm::vec2( v.z, -v.y); }
    } else if (a.y >= a.z) {
        axis = a.y;
        if (v.y > 0.0f) { face = 2; sc = glm::vec2(v.x,  v.z); }
        else             { face = 3; sc = glm::vec2(v.x, -v.z); }
    } else {
        axis = a.z;
        if (v.z > 0.0f) { face = 4; sc = glm::vec2( v.x, -v.y); }
        else             { face = 5; sc = glm::vec2(-v.x, -v.y); }
    }
    // Угол грани — ровно 90°, поэтому деление на «ось» и есть проекция:
    // tan(45°) == 1, и никакой матрицы для координат не нужно.
    outUV = sc / std::max(axis, 1e-6f) * 0.5f + 0.5f;
    return face;
}

float PerspectiveDepth01(float distance, float nearZ, float farZ) {
    const float d = std::max(distance, 1e-6f);
    const float denom = std::max(farZ - nearZ, 1e-6f);
    const float ndc = ((farZ + nearZ) - 2.0f * farZ * nearZ / d) / denom;
    return ndc * 0.5f + 0.5f;
}

namespace {

// Ближняя плоскость карты локального света. Не «поменьше, чтобы всё влезло»:
// у перспективной проекции точность глубины почти целиком определяется ею, и
// сантиметровая ближняя плоскость съедает разрешение буфера так, что тень
// начинает шуметь на всей дальности. Пять сантиметров — предмет вплотную к
// лампе в карту не попадёт, но он и так внутри неё.
constexpr float kNear = 0.05f;

// Источники сортируются по «весу»: место в атласе получают те, чью тень
// заметнее. Вес — яркость на дальности: лампа, которая едва светит или светит
// на метр, тени не заслуживает, даже если стоит первой в списке сцены.
float Weight(float intensity, float range) {
    return std::max(intensity, 0.0f) * std::max(range, 0.0f);
}

} // namespace

LocalShadowAtlas::LocalShadowAtlas(int resolution, int tile) {
    m_tile = std::max(tile, 64);
    m_grid = std::max(resolution / m_tile, 1);
    m_resolution = m_grid * m_tile;

    sage::rhi::RenderTargetDesc desc;
    desc.Width = m_resolution;
    desc.Height = m_resolution;
    desc.Kind = sage::rhi::RenderTargetKind::DepthOnly;
    m_target = sage::rhi::GraphicsDevice::Get().CreateRenderTarget(desc);
    if (m_target) {
        m_binding.Texture = m_target->DepthTextureHandle();
        m_binding.AtlasTexel = 1.0f / (float)m_resolution;
    }
}

void LocalShadowAtlas::TileRect(int index, int& x, int& y) const {
    x = (index % m_grid) * m_tile;
    y = (index / m_grid) * m_tile;
}

glm::vec4 LocalShadowAtlas::TileUV(int index) const {
    int x, y;
    TileRect(index, x, y);
    const float inv = 1.0f / (float)m_resolution;
    return glm::vec4((float)x * inv, (float)y * inv, (float)m_tile * inv, 0.0f);
}

void LocalShadowAtlas::Prepare(LightingEnvironment& env) {
    m_passes.clear();
    m_cleared = false;
    m_binding.SpotCount = 0;
    m_binding.PointCount = 0;
    m_binding.Enabled = false;
    for (PointLight& p : env.PointLights) p.ShadowSlot = -1;
    for (SpotLight& s : env.SpotLights) s.ShadowSlot = -1;
    if (!m_target) return;

    const int tiles = m_grid * m_grid;
    int nextTile = 0;

    // --- Прожекторы: одна плитка на источник ------------------------------
    std::vector<int> spotOrder;
    for (int i = 0; i < (int)env.SpotLights.size(); ++i) {
        if (env.SpotLights[(size_t)i].CastShadows) spotOrder.push_back(i);
    }
    std::sort(spotOrder.begin(), spotOrder.end(), [&env](int a, int b) {
        const SpotLight& la = env.SpotLights[(size_t)a];
        const SpotLight& lb = env.SpotLights[(size_t)b];
        return Weight(la.Intensity, la.Range) > Weight(lb.Intensity, lb.Range);
    });

    for (int index : spotOrder) {
        if (m_binding.SpotCount >= LocalShadowBinding::kMaxSpot) break;
        if (nextTile >= tiles) break;
        SpotLight& light = env.SpotLights[(size_t)index];
        const int slot = m_binding.SpotCount++;
        const int tile = nextTile++;

        // Угол проекции — ПОЛНЫЙ внешний угол конуса: круг света вписан в
        // квадрат карты. Брать больше значит тратить текселя на темноту за
        // краем конуса, брать меньше — обрезать тень по краям светового пятна.
        const float outer = std::clamp(light.OuterAngleDeg, 1.0f, 87.0f);
        const float far = std::max(light.Range, kNear * 2.0f);
        const glm::mat4 proj = glm::perspective(glm::radians(outer * 2.0f), 1.0f, kNear, far);
        // Безопасный «верх»: прожектор, направленный строго вниз, вырождает
        // обычный (0,1,0) — тогда матрица вида получилась бы нулевой, а тень
        // исчезла бы ровно в самом частом случае (лампа под потолком).
        const glm::vec3 dir = glm::normalize(light.Direction);
        const glm::vec3 up = (std::abs(dir.y) > 0.99f) ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                       : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::mat4 view = glm::lookAt(light.Position, light.Position + dir, up);

        m_binding.SpotMatrix[slot] = proj * view;
        m_binding.SpotRect[slot] = TileUV(tile);
        // Мировой размер текселя на единицу расстояния: ширина карты на
        // расстоянии d равна 2*d*tan(outer), делённая на число текселей.
        const float texelPerUnit =
            2.0f * std::tan(glm::radians(outer)) / (float)m_tile;
        m_binding.SpotParams[slot] = glm::vec3(kNear, far, texelPerUnit);
        light.ShadowSlot = slot;

        Pass pass;
        pass.Matrix = m_binding.SpotMatrix[slot];
        TileRect(tile, pass.TileX, pass.TileY);
        m_passes.push_back(pass);
    }

    // --- Точечные: шесть плиток на источник --------------------------------
    std::vector<int> pointOrder;
    for (int i = 0; i < (int)env.PointLights.size(); ++i) {
        if (env.PointLights[(size_t)i].CastShadows) pointOrder.push_back(i);
    }
    std::sort(pointOrder.begin(), pointOrder.end(), [&env](int a, int b) {
        const PointLight& la = env.PointLights[(size_t)a];
        const PointLight& lb = env.PointLights[(size_t)b];
        return Weight(la.Intensity, la.Range) > Weight(lb.Intensity, lb.Range);
    });

    for (int index : pointOrder) {
        if (m_binding.PointCount >= LocalShadowBinding::kMaxPoint) break;
        if (nextTile + LocalShadowBinding::kFaces > tiles) break;
        PointLight& light = env.PointLights[(size_t)index];
        const int slot = m_binding.PointCount++;
        const float far = std::max(light.Range, kNear * 2.0f);
        const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, kNear, far);

        for (int face = 0; face < LocalShadowBinding::kFaces; ++face) {
            const int tile = nextTile++;
            const CubeFaceBasis& basis = CubeFaces()[face];
            const glm::mat4 view =
                glm::lookAt(light.Position, light.Position + basis.Forward, basis.Up);
            m_binding.PointRect[slot * LocalShadowBinding::kFaces + face] = TileUV(tile);

            Pass pass;
            pass.Matrix = proj * view;
            TileRect(tile, pass.TileX, pass.TileY);
            m_passes.push_back(pass);
        }
        // Угол грани 90°, значит tan(45°) == 1 и ширина грани на расстоянии d
        // равна 2*d — отсюда тексель.
        m_binding.PointParams[slot] = glm::vec3(kNear, far, 2.0f / (float)m_tile);
        light.ShadowSlot = slot;
    }

    m_binding.Enabled = !m_passes.empty();
}

void LocalShadowAtlas::BeginPass(int pass) {
    if (!m_target || pass < 0 || pass >= (int)m_passes.size()) return;
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    m_target->Bind();
    if (!m_cleared) {
        // Чистим ВЕСЬ атлас разом, до первой плитки. Чистить каждую отдельно
        // ножницами было бы столько же работы, но оставило бы незанятые плитки
        // с прошлым кадром — а «незанятая» плитка обязана означать «здесь
        // ничего не загораживает свет».
        device.SetScissor(false);
        device.Clear(/*color=*/false, /*depth=*/true);
        m_cleared = true;
    }
    const Pass& p = m_passes[(size_t)pass];
    device.SetViewport(p.TileX, p.TileY, m_tile, m_tile);
    // Ножницы поверх вьюпорта — страховка. Треугольники и так отсекаются
    // объёмом отсечения ровно по границе вьюпорта, но широкие линии и точки
    // (отладочная геометрия) этому правилу не подчиняются, и одна такая линия
    // испортила бы соседнюю плитку, то есть тень чужой лампы.
    device.SetScissor(true, p.TileX, p.TileY, m_tile, m_tile);
    device.SetCullMode(sage::rhi::CullMode::Back);
}

void LocalShadowAtlas::End(int screenW, int screenH) {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetScissor(false);
    device.SetCullMode(sage::rhi::CullMode::Back);
    device.BindDefaultFramebuffer();
    device.SetViewport(0, 0, screenW, screenH);
}

} // namespace sage::render
