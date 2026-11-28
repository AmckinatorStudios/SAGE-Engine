#include "sage/render/BillboardSystem.h"
#include "sage/render/ResourceManager.h"

#include <cmath>

// Встроенный шейдер билбордов. Тот же способ разворота к камере, что и у
// частиц (uCameraRight/uCameraUp), — поэтому спрайт и частица в одном кадре
// ведут себя одинаково и их можно ставить рядом.
//
// Отдельный от частиц шейдер, а не общий: у частицы параметры приходят
// ПОТОКОМ ИНСТАНСОВ (их тысячи и они меняются каждый кадр), у билборда —
// юниформами (их десятки и они стоят на месте). Свести их в один шейдер
// значило бы гонять инстанс-буфер ради трёх маркеров.
namespace {

const char* kBillboardVert = R"(#version 330 core
layout (location = 0) in vec2 aCorner;   // -0.5..0.5 угол квада
layout (location = 1) in vec2 aUV;

out vec2 vUV;

uniform mat4 uView;
uniform mat4 uProjection;
uniform vec3 uCameraRight;
uniform vec3 uCameraUp;
uniform vec3 uWorldPos;
uniform vec2 uSize;
uniform vec2 uPivot;      // сдвиг квада в его плоскости (низ спрайта на точке)
uniform float uRotation;
uniform vec4 uSpriteUV;

void main() {
    vec2 corner = aCorner + uPivot;
    float c = cos(uRotation), s = sin(uRotation);
    vec2 r = vec2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);
    vec3 world = uWorldPos + uCameraRight * (r.x * uSize.x) + uCameraUp * (r.y * uSize.y);
    gl_Position = uProjection * uView * vec4(world, 1.0);
    vUV = mix(uSpriteUV.xy, uSpriteUV.zw, aUV);
}
)";

const char* kBillboardFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform bool uUseTexture;
uniform vec4 uTint;

void main() {
    vec4 c = uTint;
    if (uUseTexture) c *= texture(uTexture, vUV);
    // Полностью прозрачные пиксели выбрасываем: маркер с текстурой-иконкой
    // иначе кладёт на сцену прямоугольник пустоты там, где у иконки фон.
    if (c.a < 0.004) discard;
    FragColor = c;
}
)";

} // namespace

Shader& BillboardSystem::BuiltinShader() {
    // Намеренно не уничтожается (как встроенный шейдер частиц): function-local
    // static Shader снёс бы GL-программу уже после разрушения контекста.
    static Shader* shader =
        new Shader(Shader::FromSource(kBillboardVert, kBillboardFrag, "BillboardSystem"));
    return *shader;
}

void BillboardSystem::Update(float dt) {
    if (!std::isfinite(dt) || dt <= 0.0f) return;
    for (auto& [id, sprite] : m_sprites)
        if (sprite.SpriteRef.Valid() && sprite.SpriteRef.Type == sage::assets::SpriteRef::Kind::Sequence)
            sprite.SpriteTime += dt;
}

void BillboardSystem::SetSprite(int id, sage::assets::SpriteRef ref) {
    if (auto* sprite = Get(id)) {
        sprite->SpriteRef = ref;
        sprite->SpriteTime = 0.0;
    }
}

void BillboardSystem::DrawOne(Shader& shader, const BillboardSprite& sprite) {
    auto& resources = ResourceManager::Instance();
    std::shared_ptr<const Texture> image;
    const Texture* texture = sprite.SpriteTexture;
    ResourceManager::SpriteFrame frame;
    frame.Pivot.y = sprite.Pivot == BillboardPivot::Bottom ? 0.5f : 0.0f;
    if (sprite.SpriteRef.Valid()) {
        auto resolved = resources.ResolveSpriteTexture(sprite.SpriteRef, TextureFilter::Bilinear,
                                                       false, false, false, false);
        if (resolved) {
            double time = sprite.SpriteTime;
            if (const auto* sequence = resolved.Data.Sequence(); sequence && sequence->Loop && !sequence->FrameIds().empty())
                time = std::fmod(time, double(sequence->FrameIds().size()) / sequence->Fps);
            if (const auto* region = resolved.Data.Region(static_cast<float>(time))) {
                frame = resolved.Frame(*region);
                image = std::move(resolved.Image);
                texture = image.get();
            }
        }
    }
    if (!image) {
        if (!texture && !sprite.TexturePath.empty()) {
            image = resources.GetTexture(sprite.TexturePath, TextureFilter::Bilinear);
            texture = image.get();
        }
        if (texture && !texture->FlippedRows()) frame.UV = {0, 1, 1, 0};
    }

    shader.SetVec3("uWorldPos", sprite.WorldPos);
    shader.SetVec2("uSize", sprite.Size);
    shader.SetVec2("uPivot", frame.Pivot);
    shader.SetVec4("uSpriteUV", frame.UV);
    shader.SetFloat("uRotation", sprite.Rotation);
    shader.SetVec4("uTint", sprite.Tint);
    shader.SetInt("uUseTexture", texture ? 1 : 0);
    if (texture) {
        texture->Bind(0);
        shader.SetInt("uTexture", 0);
    }
    m_geometry->DrawArrays(6);
}
