#include "sage/paint/PaintCanvas.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sage/core/Log.h"
#include "sage/render/Shader.h"
#include "sage/rhi/GraphicsDevice.h"

// Реализации stb уже собраны в движке (Screenshot.cpp пишет PNG,
// импортёры читают): здесь только объявления, второй копии не заводим.
#include "stb_image.h"
#include "stb_image_write.h"

namespace sage::paint {
namespace {

// --- Кисть: круг с растушёванным краем ------------------------------------
//
// Вершины считаются из gl_VertexID: четыре угла квадрата, покрывающего след
// кисти. Отдельного буфера под них нет намеренно — удар кисти это две
// треугольника, и заводить ради них геометрию значит платить за неё на каждом
// ударе мазка.
const char* kDabVert = R"(#version 330 core
uniform vec2 uCenterUV;   // центр следа в координатах холста [0..1]
uniform vec2 uRadiusUV;   // радиус по каждой оси (холст бывает неквадратным в метрах)
out vec2 vLocal;          // -1..1 внутри следа

// Два треугольника из шести вершин: DrawArrays в RHI рисует только их,
// полос (TriangleStrip) в интерфейсе нет — и заводить их ради кисти незачем.
vec2 Corner(int id) {
    int i = (id < 3) ? id : (id == 3 ? 0 : (id == 4 ? 2 : 3));
    return vec2((i == 1 || i == 2) ? 1.0 : -1.0, (i >= 2) ? 1.0 : -1.0);
}

void main() {
    vec2 corner = Corner(gl_VertexID);
    vLocal = corner;
    vec2 uv = uCenterUV + corner * uRadiusUV;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kDabFrag = R"(#version 330 core
in vec2 vLocal;
out vec4 FragColor;

uniform vec4  uColor;
uniform float uHardness;   // 0 — мягкий край, 1 — почти круг
uniform float uFlow;       // сколько краски за удар
uniform int   uErase;

void main() {
    float d = length(vLocal);
    if (d > 1.0) discard;
    // Край: от uHardness до единицы краска гаснет. При uHardness = 1 переход
    // всё равно шириной в сотую радиуса — иначе круг получается с лесенкой,
    // а «резкая кисть» не значит «зубчатая».
    float inner = clamp(uHardness, 0.0, 0.99);
    float a = 1.0 - smoothstep(inner, 1.0, d);
    a *= uFlow;
    if (uErase == 1) {
        // Ластик: снимаем краску. Цвет не важен, важна доля снятого.
        FragColor = vec4(0.0, 0.0, 0.0, a);
        return;
    }
    FragColor = vec4(uColor.rgb, uColor.a * a);
}
)";

// --- Заливка: сплошные треугольники в координатах холста -------------------
const char* kFillVert = R"(#version 330 core
layout (location = 0) in vec2 aUV;
void main() { gl_Position = vec4(aUV * 2.0 - 1.0, 0.0, 1.0); }
)";

const char* kFillFrag = R"(#version 330 core
out vec4 FragColor;
uniform vec4 uColor;
void main() { FragColor = uColor; }
)";

Shader& DabShader() {
    static Shader* s = new Shader(Shader::FromSource(kDabVert, kDabFrag, "Paint.Dab"));
    return *s;
}
Shader& FillShader() {
    static Shader* s = new Shader(Shader::FromSource(kFillVert, kFillFrag, "Paint.Fill"));
    return *s;
}

// Детерминированное «дрожание» кисти по краю.
//
// Именно детерминированное, а не случайное: одна и та же область обязана
// краситься одинаково при каждой перерисовке. Со случайным генератором край
// «кипел» бы при любом обновлении, а прогон перестал бы быть воспроизводимым —
// то есть сравнивать кадры было бы нечем.
float Jitter(const Vec2& p, float salt) {
    const float v = std::sin(p.x * 12.9898f + p.y * 78.233f + salt * 37.719f) * 43758.5453f;
    return (v - std::floor(v)) * 2.0f - 1.0f;   // -1..1
}

} // namespace

struct Canvas::Impl {
    std::unique_ptr<rhi::RenderTarget> Target;
    std::unique_ptr<rhi::Geometry> DabGeom;    // без атрибутов: углы из gl_VertexID
    std::unique_ptr<rhi::Geometry> FillGeom;   // динамический буфер vec2
    std::vector<Vec2> Scratch;                 // треугольники заливки, переиспользуем
    std::shared_ptr<::Texture> View;           // обёртка для материалов и интерфейса
};

Canvas::Canvas(int size, const PlanarMapping& mapping)
    : m_size(std::max(4, size)), m_mapping(mapping), m_impl(std::make_unique<Impl>()) {}

Canvas::~Canvas() = default;

void Canvas::EnsureResources() {
    if (m_impl->Target) return;
    rhi::RenderTargetDesc desc;
    desc.Width = desc.Height = m_size;
    // ColorHDR — потому что другого варианта «цвет без глубины» в RHI нет.
    // Для краски это не расточительство, а точность: мазки накладываются
    // десятками, и в восьми битах на канал накопление заметно ступенчатое.
    desc.Kind = rhi::RenderTargetKind::ColorHDR;
    m_impl->Target = rhi::GraphicsDevice::Get().CreateRenderTarget(desc);

    rhi::VertexLayout dab;   // атрибутов нет
    m_impl->DabGeom = rhi::GraphicsDevice::Get().CreateGeometry(dab);

    rhi::VertexLayout fill;
    fill.Stride = sizeof(Vec2);
    fill.Attributes = {{0, 2, rhi::AttribType::Float, 0}};
    m_impl->FillGeom = rhi::GraphicsDevice::Get().CreateGeometry(fill);

    Clear();
}

void Canvas::BeginPaint() {
    EnsureResources();
    rhi::GraphicsDevice& device = rhi::GraphicsDevice::Get();
    m_impl->Target->Bind();
    device.SetDepthTest(false);
    device.SetDepthWrite(false);
    device.SetCullMode(rhi::CullMode::Off);
    device.SetBlend(true);
    device.SetBlendMode(rhi::GraphicsDevice::BlendMode::Alpha);
}

void Canvas::EndPaint() {
    rhi::GraphicsDevice& device = rhi::GraphicsDevice::Get();
    device.SetBlend(false);
    device.SetDepthTest(true);
    device.SetDepthWrite(true);
    device.BindDefaultFramebuffer();
}

void Canvas::Clear(const Vec4& color) {
    EnsureResources();
    rhi::GraphicsDevice& device = rhi::GraphicsDevice::Get();
    m_impl->Target->Bind();
    device.SetClearColor(color.r, color.g, color.b, color.a);
    device.Clear(true, false);
    device.BindDefaultFramebuffer();
}

void Canvas::Stamp(const Vec2& worldPos, const Brush& brush) {
    BeginPaint();
    Shader& sh = DabShader();
    sh.Use();
    const Vec2 uv = m_mapping.ToUV(worldPos);
    const Vec2 span = m_mapping.Size();
    sh.SetVec2("uCenterUV", uv);
    sh.SetVec2("uRadiusUV", Vec2(brush.Radius / (span.x != 0.0f ? span.x : 1.0f),
                                 brush.Radius / (span.y != 0.0f ? span.y : 1.0f)));
    sh.SetVec4("uColor", brush.Color);
    sh.SetFloat("uHardness", std::clamp(brush.Hardness, 0.0f, 1.0f));
    sh.SetFloat("uFlow", std::clamp(brush.Flow, 0.0f, 1.0f));
    sh.SetInt("uErase", brush.Mode == BlendMode::Erase ? 1 : 0);
    if (brush.Mode == BlendMode::Erase)
        rhi::GraphicsDevice::Get().SetBlendMode(rhi::GraphicsDevice::BlendMode::Erase);
    m_impl->DabGeom->DrawArrays(6);
    EndPaint();
}

void Canvas::Stroke(const Vec2& a, const Vec2& b, const Brush& brush, float spacing) {
    const float step = std::max(0.02f, brush.Radius * std::max(0.05f, spacing));
    const Vec2 delta = b - a;
    const float len = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    const int count = std::max(1, (int)std::ceil(len / step));
    for (int i = 0; i <= count; ++i) {
        const float t = (float)i / (float)count;
        Stamp(a + delta * t, brush);
    }
}

void Canvas::FillPolygon(const std::vector<Vec2>& poly, const Brush& brush, float edgeJitter) {
    if (poly.size() < 3) return;

    // 1. Середина — сплошь. Веер треугольников от первой вершины: области,
    // которые красят этой системой (территория, разметка, пятно), выпуклые, а
    // для невыпуклых веер даст лишнее — о чём и сказано в заголовке.
    BeginPaint();
    std::vector<Vec2>& tri = m_impl->Scratch;
    tri.clear();
    for (size_t i = 1; i + 1 < poly.size(); ++i) {
        tri.push_back(m_mapping.ToUV(poly[0]));
        tri.push_back(m_mapping.ToUV(poly[i]));
        tri.push_back(m_mapping.ToUV(poly[i + 1]));
    }
    Shader& fill = FillShader();
    fill.Use();
    fill.SetVec4("uColor", brush.Color);
    m_impl->FillGeom->SetVertexData(tri.data(), tri.size() * sizeof(Vec2), true);
    m_impl->FillGeom->DrawArrays((int)tri.size());
    EndPaint();

    // 2. Край — кистью. Это и есть вся разница между «залито» и «закрашено»:
    // прямой отрезок многоугольника превращается в цепочку круглых отпечатков,
    // и граница перестаёт читаться как геометрия.
    Brush edge = brush;
    edge.Hardness = std::min(brush.Hardness, 0.35f);
    const float step = std::max(0.05f, edge.Radius * 0.35f);
    for (size_t i = 0; i < poly.size(); ++i) {
        const Vec2 a = poly[i];
        const Vec2 b = poly[(i + 1) % poly.size()];
        const Vec2 d = b - a;
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        const int count = std::max(1, (int)std::ceil(len / step));
        for (int k = 0; k <= count; ++k) {
            const float t = (float)k / (float)count;
            Vec2 p = a + d * t;
            if (edgeJitter > 0.0f) {
                p.x += Jitter(p, 1.0f) * edge.Radius * edgeJitter;
                p.y += Jitter(p, 2.0f) * edge.Radius * edgeJitter;
            }
            Stamp(p, edge);
        }
    }
}

std::shared_ptr<::Texture> Canvas::AsTexture() {
    EnsureResources();
    if (!m_impl->View) m_impl->View = ::Texture::Wrap(Texture(), m_size, m_size);
    return m_impl->View;
}

rhi::TextureHandle Canvas::Texture() const {
    return m_impl->Target ? m_impl->Target->ColorTextureHandle() : rhi::TextureHandle{};
}

Vec4 Canvas::SampleWorld(const Vec2& worldPos) const {
    if (!m_impl->Target) return {};
    const Vec2 uv = m_mapping.ToUV(worldPos);
    const int x = std::clamp((int)(uv.x * (float)m_size), 0, m_size - 1);
    // Строки читаются снизу вверх (см. GraphicsDevice.h), поэтому по вертикали
    // координата не переворачивается: uv.y = 0 это и есть нижняя строка.
    const int y = std::clamp((int)(uv.y * (float)m_size), 0, m_size - 1);

    unsigned char px[4] = {0, 0, 0, 0};
    m_impl->Target->Bind();
    const bool ok = rhi::GraphicsDevice::Get().ReadPixelsRGBA(x, y, 1, 1, px);
    rhi::GraphicsDevice::Get().BindDefaultFramebuffer();
    if (!ok) return {};
    return {px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f, px[3] / 255.0f};
}

bool Canvas::SavePng(const std::filesystem::path& path) const {
    if (!m_impl->Target) return false;
    std::vector<unsigned char> pixels((size_t)m_size * m_size * 4);
    m_impl->Target->Bind();
    const bool ok = rhi::GraphicsDevice::Get().ReadPixelsRGBA(0, 0, m_size, m_size, pixels.data());
    rhi::GraphicsDevice::Get().BindDefaultFramebuffer();
    if (!ok) {
        LOG_ERROR("Paint") << "Бэкенд не умеет читать RGBA — холст не сохранён: " << path.string();
        return false;
    }
    // Переворот строк: GL отдаёт снизу вверх, PNG хранит сверху вниз.
    std::vector<unsigned char> flipped(pixels.size());
    const size_t row = (size_t)m_size * 4;
    for (int y = 0; y < m_size; ++y)
        std::memcpy(&flipped[(size_t)y * row], &pixels[(size_t)(m_size - 1 - y) * row], row);
    return stbi_write_png(path.string().c_str(), m_size, m_size, 4, flipped.data(),
                          (int)row) != 0;
}

bool Canvas::LoadPng(const std::filesystem::path& path) {
    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load(path.string().c_str(), &w, &h, &comp, 4);
    if (!data) {
        LOG_ERROR("Paint") << "Не удалось прочитать холст: " << path.string();
        return false;
    }
    EnsureResources();
    // Кладём картинку на холст ОДНИМ прямоугольником через временную текстуру:
    // рисовать её попиксельно ударами кисти было бы и медленно, и неточно.
    rhi::Texture2DDesc desc;
    desc.Width = w;
    desc.Height = h;
    desc.Channels = 4;
    desc.FilterMode = rhi::Filter::Bilinear;
    desc.WrapMode = rhi::Wrap::ClampEdge;
    desc.GenerateMipmaps = false;
    auto tex = rhi::GraphicsDevice::Get().CreateTexture2D(desc, data);
    stbi_image_free(data);
    if (!tex) return false;

    // Рисуем полноэкранный прямоугольник с этой текстурой, полностью заменяя
    // содержимое холста (смешивание выключено — это загрузка, а не мазок).
    static Shader* blit = new Shader(Shader::FromSource(
        R"(#version 330 core
out vec2 vUV;
vec2 Corner(int id) {
    int i = (id < 3) ? id : (id == 3 ? 0 : (id == 4 ? 2 : 3));
    return vec2((i == 1 || i == 2) ? 1.0 : 0.0, (i >= 2) ? 1.0 : 0.0);
}
void main() {
    vec2 c = Corner(gl_VertexID);
    vUV = c;
    gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);
})",
        R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSrc;
void main() { FragColor = texture(uSrc, vUV); })",
        "Paint.Load"));

    rhi::GraphicsDevice& device = rhi::GraphicsDevice::Get();
    m_impl->Target->Bind();
    device.SetBlend(false);
    device.SetDepthTest(false);
    blit->Use();
    blit->SetInt("uSrc", 0);
    tex->Bind(0);
    m_impl->DabGeom->DrawArrays(6);
    device.SetDepthTest(true);
    device.BindDefaultFramebuffer();
    return true;
}

} // namespace sage::paint
