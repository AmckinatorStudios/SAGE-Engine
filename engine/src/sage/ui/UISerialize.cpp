#include "sage/ui/UISerialize.h"

#include <string>

#include "sage/events/Events.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Texture.h"
#include "sage/scene/SceneJson.h"
#include "sage/scene/SceneValueJson.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPart.h"
#include "sage/vars/ScriptVars.h"

using json = nlohmann::json;

// Переменные и связи в JSON — общий словарь (см. SceneValueJson.h).
using sage::scene::BindingsFromJson;
using sage::scene::BindingsToJson;
using sage::scene::ValueFromJson;
using sage::scene::ValueToJson;
using sage::scene::VarsFromJson;
using sage::scene::VarsToJson;
using sage::scene::Vec2FromJson;
using sage::scene::Vec2ToJson;
using sage::scene::Vec4FromJson;
using sage::scene::Vec4ToJson;

namespace sage::ui {
namespace {

void SaveField(json& out, const void* data, const sage::ui::PartField& f) {
    using K = sage::ui::PartField::Kind;
    switch (f.Type) {
        case K::Bool: out[f.Key] = sage::ui::FieldAs<bool>(data, f); break;
        case K::Int: out[f.Key] = sage::ui::FieldAs<int>(data, f); break;
        case K::Float: out[f.Key] = sage::ui::FieldAs<float>(data, f); break;
        case K::String: out[f.Key] = sage::ui::FieldAs<std::string>(data, f); break;
        case K::Color:
        case K::Vec4: out[f.Key] = Vec4ToJson(sage::ui::FieldAs<glm::vec4>(data, f)); break;
        case K::Vec2: out[f.Key] = Vec2ToJson(sage::ui::FieldAs<glm::vec2>(data, f)); break;
        // Перечисление пишется ЧИСЛОМ: имена значений живут в таблице полей и
        // нужны человеку, а файл должен пережить их переименование.
        case K::Enum: out[f.Key] = sage::ui::FieldAs<int>(data, f); break;
        case K::Bindings:
            out[f.Key] = BindingsToJson(sage::ui::FieldAs<sage::events::Bindings>(data, f));
            break;
    }
}

void LoadField(const json& in, void* data, const sage::ui::PartField& f) {
    if (!in.contains(f.Key)) return; // нет ключа — остаётся значение по умолчанию
    using K = sage::ui::PartField::Kind;
    const json& v = in[f.Key];
    switch (f.Type) {
        case K::Bool:
            if (v.is_boolean()) sage::ui::FieldAs<bool>(data, f) = v.get<bool>();
            break;
        case K::Int:
            if (v.is_number()) sage::ui::FieldAs<int>(data, f) = v.get<int>();
            break;
        case K::Float:
            if (v.is_number()) sage::ui::FieldAs<float>(data, f) = v.get<float>();
            break;
        case K::String:
            if (v.is_string()) sage::ui::FieldAs<std::string>(data, f) = v.get<std::string>();
            break;
        case K::Color:
        case K::Vec4:
            sage::ui::FieldAs<glm::vec4>(data, f) =
                Vec4FromJson(v, sage::ui::FieldAs<glm::vec4>(data, f));
            break;
        case K::Vec2:
            sage::ui::FieldAs<glm::vec2>(data, f) =
                Vec2FromJson(v, sage::ui::FieldAs<glm::vec2>(data, f));
            break;
        case K::Enum:
            // Значение из файла зажимается по списку имён: чужой номер (файл от
            // будущей версии) не должен превращаться в мусорное перечисление.
            if (v.is_number()) {
                const int n = v.get<int>();
                if (f.EnumCount <= 0 || (n >= 0 && n < f.EnumCount))
                    sage::ui::FieldAs<int>(data, f) = n;
            }
            break;
        case K::Bindings:
            BindingsFromJson(v, sage::ui::FieldAs<sage::events::Bindings>(data, f));
            break;
    }
}

void SaveUIComponents(json& j, const entt::registry& reg, entt::entity e) {
    const sage::ui::Element* t = reg.try_get<sage::ui::Element>(e);
    if (!t) return;
    json& uj = j["ui"];

    // Раскладка — не компонент, а сам элемент: без неё элемента нет, и в
    // реестре компонентов ей делать нечего.
    json& tj = uj["element"];
    tj["type"] = t->Type;
    tj["anchor"] = (int)t->Anchor;
    tj["stretch"] = (int)t->Mode;
    tj["position"] = Vec2ToJson(t->Position);
    tj["size"] = Vec2ToJson(t->Size);
    tj["margin"] = Vec4ToJson(t->Margin);
    tj["pivot"] = Vec2ToJson(t->Pivot);
    tj["rotation"] = t->Rotation;
    tj["order"] = t->Order;
    tj["visible"] = t->Visible;
    tj["active"] = t->Active;
    tj["locked"] = t->Locked;
    // Resolved не пишется: это след последнего кадра, а не настройка.

    for (const sage::ui::PartType& p : sage::ui::Parts()) {
        if (!p.Fields || !p.Has || !p.Has(reg, e)) continue;  // без полей писать нечего
        const void* data = p.Get(reg, e);
        if (!data) continue;
        json& pj = uj[p.Id];
        for (const sage::ui::PartField& f : *p.Fields) SaveField(pj, data, f);
    }
}

// Загружает текстуру картинки элемента (рантайм-поле, в файл не пишется).
void ResolveImageTextureImpl(sage::ui::Image& im) {
    // Путь стёрли — картинки нет. Оставить прежний указатель значит показывать
    // файл, который больше не назначен.
    if (im.Path.empty()) {
        im.Tex.reset();
        im.TexPath.clear();
        return;
    }
    // Пиксель-арт грузится ближайшим соседом и без мипмапов — иначе набор
    // спрайтов размывается, а мипмапы ЛИСТА подмешивают в края соседний спрайт.
    // Резкая фильтрация грузится ближайшим соседом и без мипмапов — иначе
    // набор спрайтов размывается, а мипмапы ЛИСТА подмешивают в края соседний
    // спрайт.
    im.Tex = im.Sharp() ? ResourceManager::Instance().GetTexture(im.Path, TextureFilter::Nearest,
                                                                 /*mipmaps=*/false)
                        : ResourceManager::Instance().GetTexture(im.Path);
    im.TexPath = im.Path;
    im.TexFiltering = im.Filtering;
}

void LoadUIComponents(const json& uj, entt::registry& reg, entt::entity e) {
    sage::ui::Element t;
    // Ключ "element", а прежде был "transform": раскладка стала частью самого
    // элемента, и старое имя врало бы про устройство. Прежний ключ читается —
    // сцены, сделанные до переименования, обязаны открываться.
    const json* tjp = uj.contains("element")   ? &uj["element"]
                      : uj.contains("transform") ? &uj["transform"]
                                                 : nullptr;
    if (tjp) {
        const json& tj = *tjp;
        t.Type = tj.value("type", t.Type);
        const int anchor = tj.value("anchor", (int)t.Anchor);
        if (anchor >= 0 && anchor <= 8) t.Anchor = (UIAnchor)anchor;
        const int stretch = tj.value("stretch", (int)t.Mode);
        if (stretch >= 0 && stretch <= 3) t.Mode = (sage::ui::Element::Stretch)stretch;
        // "offset" -> "position", "layer" -> "order": те же значения под именами,
        // которыми их зовут люди.
        t.Position = Vec2FromJson(tj.value("position", tj.value("offset", json::object())),
                                  t.Position);
        t.Size = Vec2FromJson(tj.value("size", json::object()), t.Size);
        if (tj.contains("margin")) t.Margin = Vec4FromJson(tj["margin"], t.Margin);
        t.Pivot = Vec2FromJson(tj.value("pivot", json::object()), t.Pivot);
        t.Rotation = tj.value("rotation", t.Rotation);
        t.Order = tj.value("order", tj.value("layer", t.Order));
        t.Visible = tj.value("visible", t.Visible);
        t.Active = tj.value("active", t.Active);
        t.Locked = tj.value("locked", t.Locked);
    }
    reg.emplace_or_replace<sage::ui::Element>(e, t);

    // Части — по реестру. Ключ, которого реестр не знает (часть из другой
    // сборки игры), ПРОПУСКАЕТСЯ молча и остаётся в файле нетронутым: терять
    // чужие данные при открытии сцены нельзя.
    for (const sage::ui::PartType& p : sage::ui::Parts()) {
        if (!p.Fields || !p.Id || !uj.contains(p.Id)) continue;
        p.Add(reg, e);
        void* data = p.GetMutable(reg, e);
        if (!data) continue;
        const json& pj = uj[p.Id];
        for (const sage::ui::PartField& f : *p.Fields) LoadField(pj, data, f);
    }

    // Картинке нужен рантайм-указатель на текстуру: путь в файле есть, а
    // загрузить его — дело загрузчика сцены.
    if (sage::ui::Image* im = reg.try_get<sage::ui::Image>(e)) ResolveImageTextureImpl(*im);
}

// Звуковой источник объекта (см. sage/audio/AudioComponents.h). Рантайм-поля
// (дескриптор, «звучит», команда) в файл НЕ идут: это состояние партии, а не
// свойство сцены, и сохранённое «сейчас звучит» означало бы, что сцена
// открывается с уже играющим звуком, которого никто не запускал.

} // namespace

bool SaveElement(json& out, const entt::registry& reg, entt::entity e) {
    if (!reg.try_get<sage::ui::Element>(e)) return false;
    json wrapper;
    SaveUIComponents(wrapper, reg, e);
    out = wrapper.contains("ui") ? wrapper["ui"] : json::object();
    return true;
}

void ResolveImageTexture(Image& image) { ResolveImageTextureImpl(image); }

bool ImageTextureStale(const Image& image) {
    // Путь стёрли, а указатель остался — старая картинка продолжала бы
    // рисоваться на элементе, которому её больше не назначали.
    if (image.Path.empty()) return image.Tex != nullptr;
    // Путь есть, а указателя нет — ещё не грузили (или не загрузилось).
    if (!image.Tex) return true;
    // Загружено НЕ ТО: путь сменили, или переключили пиксель-арт, а он меняет
    // фильтр и мипмапы — то есть саму текстуру, а не то, как её рисуют.
    return image.TexPath != image.Path || image.TexFiltering != image.Filtering;
}

void EnsureImageTexture(Image& image) {
    // Дешёвое сравнение на каждый кадр вместо загрузки. Сама загрузка
    // кэширована по пути (ResourceManager), так что даже при смене пути файл
    // читается с диска один раз.
    if (ImageTextureStale(image)) ResolveImageTextureImpl(image);
}

void LoadElement(const json& in, entt::registry& reg, entt::entity e) {
    LoadUIComponents(in, reg, e);
}

} // namespace sage::ui
