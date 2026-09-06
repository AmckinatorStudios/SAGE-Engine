#include "sage/ui/icons/SageIcons.h"

#include <algorithm>
#include <cmath>

#include "sage/core/Log.h"
#include "sage/rhi/GraphicsDevice.h"

namespace sage::ui::icons {

IconRegistry& IconRegistry::Instance() {
    // Живёт до конца процесса и НЕ разрушается: в нём лежат текстуры, а
    // разрушать их после закрытия графического контекста — падение. Тот же
    // приём, что у реестра документов интерфейса и рендер-текстур движка.
    static IconRegistry* instance = new IconRegistry();
    return *instance;
}

IconRegistry::IconRegistry() { m_textures.assign(kAtlasCount, nullptr); }

// Ближайший атлас по размеру. Не «всегда самый большой»: значок 24 px,
// уменьшенный до 16, теряет тонкие штрихи ровно там, где они и несут смысл, —
// поэтому в наборе три размера, а не один (§11).
int IconRegistry::AtlasIndexFor(float pixelSize) const {
    int best = 0;
    float bestDiff = 1e9f;
    for (int i = 0; i < kAtlasCount; ++i) {
        const float diff = std::abs((float)kAtlasSizes[i] - pixelSize);
        if (diff < bestDiff) {
            bestDiff = diff;
            best = i;
        }
    }
    return best;
}

IconHandle IconRegistry::Handle(Icon icon, float pixelSize) const {
    IconHandle h;
    const int index = (int)icon;
    if (index < 0 || index >= kIconCount) return h;

    const int a = AtlasIndexFor(pixelSize);
    const IconAtlasData& atlas = generated::kAtlases[a];
    const IconCell::XY cell = generated::kCells[index].Cells[a];
    h.Atlas = (uint32_t)a;
    h.U0 = (float)cell.X / (float)atlas.Width;
    h.V0 = (float)cell.Y / (float)atlas.Height;
    h.U1 = (float)(cell.X + atlas.Size) / (float)atlas.Width;
    h.V1 = (float)(cell.Y + atlas.Size) / (float)atlas.Height;
    return h;
}

IconHandle IconRegistry::Handle(const std::string& name, float pixelSize) const {
    for (int i = 0; i < kIconCount; ++i)
        if (name == generated::kNames[i]) return Handle((Icon)i, pixelSize);

    // Значки плагинов: их имена приходят с префиксом («my_plugin.radar»).
    for (size_t e = 0; e < m_extra.size(); ++e) {
        const Extra& x = m_extra[e];
        if (name.rfind(x.Prefix, 0) != 0) continue;
        const std::string local = name.substr(x.Prefix.size());
        for (size_t i = 0; i < x.Names.size(); ++i) {
            if (x.Names[i] != local) continue;
            IconHandle h;
            const IconCell::XY cell = x.Cells[i].Cells[0];
            h.Atlas = (uint32_t)(kAtlasCount + e);
            h.U0 = (float)cell.X / (float)x.Data.Width;
            h.V0 = (float)cell.Y / (float)x.Data.Height;
            h.U1 = (float)(cell.X + x.Data.Size) / (float)x.Data.Width;
            h.V1 = (float)(cell.Y + x.Data.Size) / (float)x.Data.Height;
            return h;
        }
    }
    (void)pixelSize;
    return IconHandle{};
}

bool IconRegistry::Has(const std::string& name) const { return Handle(name).Valid(); }

const char* IconRegistry::Name(Icon icon) const {
    const int index = (int)icon;
    return (index >= 0 && index < kIconCount) ? generated::kNames[index] : "";
}

std::vector<std::string> IconRegistry::Names() const {
    std::vector<std::string> out;
    out.reserve((size_t)kIconCount);
    for (int i = 0; i < kIconCount; ++i) out.push_back(generated::kNames[i]);
    for (const Extra& x : m_extra)
        for (const std::string& n : x.Names) out.push_back(x.Prefix + n);
    return out;
}

sage::rhi::Texture2D* IconRegistry::Texture(uint32_t atlas) const {
    if (atlas >= m_textures.size()) return nullptr;
    if (m_textures[atlas]) return m_textures[atlas];

    const IconAtlasData* data = nullptr;
    if (atlas < (uint32_t)kAtlasCount) data = &generated::kAtlases[atlas];
    else if (atlas - kAtlasCount < m_extra.size()) data = &m_extra[atlas - kAtlasCount].Data;
    if (!data) return nullptr;

    sage::rhi::Texture2DDesc desc;
    desc.Width = data->Width;
    desc.Height = data->Height;
    desc.Channels = 1;   // покрытие, как у атласа шрифта
    // Билинейная без мипов: значок рисуется в свой размер или очень близко к
    // нему (для того и три атласа), а мипы съели бы тонкие штрихи.
    desc.FilterMode = sage::rhi::Filter::Bilinear;
    desc.WrapMode = sage::rhi::Wrap::ClampEdge;
    desc.GenerateMipmaps = false;
    // Текстура одна на процесс и живёт до конца — release() намеренный.
    m_textures[atlas] =
        sage::rhi::GraphicsDevice::Get().CreateTexture2D(desc, data->Pixels).release();
    LOG_DEBUG("Icons") << "атлас " << data->Size << "px загружен (" << data->Width << "x"
                       << data->Height << ", " << data->PixelCount / 1024 << " КиБ)";
    return m_textures[atlas];
}

uint32_t IconRegistry::AddAtlas(const std::string& prefix, const IconAtlasData& atlas,
                                const std::vector<std::string>& names,
                                const std::vector<IconCell>& cells) {
    Extra x;
    x.Prefix = prefix;
    x.Data = atlas;
    x.Names = names;
    x.Cells = cells;
    m_extra.push_back(std::move(x));
    m_textures.push_back(nullptr);
    // Плагин НЕ трогает основной атлас: у него свой ресурс, свой номер и своя
    // текстура. Выгрузка плагина не должна уносить значки редактора.
    return (uint32_t)(kAtlasCount + m_extra.size() - 1);
}

} // namespace sage::ui::icons
