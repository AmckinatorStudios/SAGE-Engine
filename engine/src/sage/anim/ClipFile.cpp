#include "sage/anim/ClipFile.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "sage/assets/Pack.h"
#include "sage/core/Log.h"

using json = nlohmann::json;

namespace sage::anim {
namespace {

// Версия формата. Меняется, когда меняется СМЫСЛ полей, а не когда добавляется
// новое: новое поле старый файл просто не содержит, и значение по умолчанию его
// закрывает.
constexpr int kClipVersion = 1;

const char* PathName(AnimPath p) {
    switch (p) {
        case AnimPath::Rotation: return "rotation";
        case AnimPath::Scale:    return "scale";
        default:                 return "translation";
    }
}

AnimPath PathFromName(const std::string& s) {
    if (s == "rotation") return AnimPath::Rotation;
    if (s == "scale") return AnimPath::Scale;
    return AnimPath::Translation;
}

} // namespace

void SaveClip(const ClipAsset& clip, const std::string& path) {
    json root;
    root["sage_anim_version"] = kClipVersion;
    root["name"] = clip.Name;
    root["duration"] = clip.Duration;

    json channels = json::array();
    for (const ClipAsset::Channel& ch : clip.Channels) {
        json c;
        c["joint"] = ch.Joint;
        c["path"] = PathName(ch.Target);
        // Ступенчатая интерполяция пишется только когда она есть: у подавляющего
        // большинства каналов она линейная, и строка "interp":"linear" в каждом
        // из сотни каналов — это шум в файле, который человек открывает глазами.
        if (ch.Interp == AnimInterp::Step) c["interp"] = "step";

        c["times"] = ch.Times;
        json values = json::array();
        for (const glm::vec4& v : ch.Values) {
            // Поворот — кватернион (4 числа), перенос и масштаб — вектор (3).
            // Писать четвёртое число там, где его нет, значит предлагать
            // читателю файла гадать, что оно означает.
            if (ch.Target == AnimPath::Rotation)
                values.push_back(json::array({v.x, v.y, v.z, v.w}));
            else
                values.push_back(json::array({v.x, v.y, v.z}));
        }
        c["values"] = std::move(values);
        channels.push_back(std::move(c));
    }
    root["channels"] = std::move(channels);

    std::ofstream out(path);
    if (!out) throw std::runtime_error("не удалось открыть для записи: " + path);
    out << root.dump(1, '\t');
    if (!out) throw std::runtime_error("не удалось записать: " + path);
}

ClipAsset LoadClip(const std::string& path) {
    // Через Pack: клипы уезжают в .sagepak вместе с остальными ассетами, и
    // собранная игра читает их оттуда же, откуда читает сцены и материалы.
    std::string text;
    if (!sage::assets::vfs::ReadText(path, text))
        throw std::runtime_error("не удалось прочитать: " + path);

    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error("не разобран как JSON: " + std::string(e.what()));
    }

    ClipAsset clip;
    clip.Name = root.value("name", std::string());
    clip.Duration = root.value("duration", 0.0f);
    if (!root.contains("channels") || !root["channels"].is_array()) return clip;

    for (const json& c : root["channels"]) {
        if (!c.is_object()) continue;
        ClipAsset::Channel ch;
        ch.Joint = c.value("joint", std::string());
        ch.Target = PathFromName(c.value("path", std::string("translation")));
        ch.Interp = c.value("interp", std::string("linear")) == "step" ? AnimInterp::Step
                                                                       : AnimInterp::Linear;
        if (c.contains("times") && c["times"].is_array())
            for (const json& t : c["times"]) ch.Times.push_back(t.get<float>());
        if (c.contains("values") && c["values"].is_array()) {
            for (const json& v : c["values"]) {
                if (!v.is_array() || v.size() < 3) continue;
                glm::vec4 out(0.0f);
                out.x = v[0].get<float>();
                out.y = v[1].get<float>();
                out.z = v[2].get<float>();
                out.w = v.size() > 3 ? v[3].get<float>() : 0.0f;
                ch.Values.push_back(out);
            }
        }
        // Канал, у которого ключей и значений разное число, — битый: сэмплировать
        // его нечем. Обрезаем по короткому, вместо того чтобы читать за границу.
        const size_t n = std::min(ch.Times.size(), ch.Values.size());
        ch.Times.resize(n);
        ch.Values.resize(n);
        if (n == 0 || ch.Joint.empty()) continue;
        clip.Channels.push_back(std::move(ch));
    }
    return clip;
}

ClipAsset ToAsset(const AnimationClip& clip, const Skeleton& skeleton) {
    ClipAsset out;
    out.Name = clip.Name;
    out.Duration = clip.Duration;
    for (const AnimChannel& ch : clip.Channels) {
        if (ch.Joint < 0 || ch.Joint >= skeleton.Count()) continue;
        ClipAsset::Channel c;
        c.Joint = skeleton.Joints[(size_t)ch.Joint].Name;
        // Кость без имени адресовать нечем: такой канал в файле бесполезен, а
        // при обратной привязке молча пропал бы. Лучше не писать его вовсе.
        if (c.Joint.empty()) continue;
        c.Target = ch.Target;
        c.Interp = ch.Interp;
        c.Times = ch.Times;
        c.Values = ch.Values;
        out.Channels.push_back(std::move(c));
    }
    return out;
}

AnimationClip Bind(const ClipAsset& asset, const Skeleton& skeleton, int* outMissing) {
    std::unordered_map<std::string, int> byName;
    byName.reserve((size_t)skeleton.Count());
    for (int i = 0; i < skeleton.Count(); ++i) {
        const std::string& n = skeleton.Joints[(size_t)i].Name;
        if (!n.empty()) byName.emplace(n, i);
    }

    AnimationClip clip;
    clip.Name = asset.Name;
    clip.Duration = asset.Duration;
    int missing = 0;
    for (const ClipAsset::Channel& c : asset.Channels) {
        auto it = byName.find(c.Joint);
        if (it == byName.end()) { ++missing; continue; }
        AnimChannel ch;
        ch.Joint = it->second;
        ch.Target = c.Target;
        ch.Interp = c.Interp;
        ch.Times = c.Times;
        ch.Values = c.Values;
        clip.Channels.push_back(std::move(ch));
    }
    if (outMissing) *outMissing = missing;
    return clip;
}

std::string ClipFileName(const std::string& modelFileStem, const std::string& clipName) {
    std::string name;
    for (unsigned char ch : clipName) {
        // Разрешаем буквы, цифры, дефис и подчёркивание; кириллицу (>= 0x80)
        // тоже — имена клипов в русских проектах бывают русскими. Всё прочее
        // (пробелы, «|» из Blender, двоеточия из Mixamo) заменяем.
        const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '_' || ch >= 0x80;
        name.push_back(ok ? (char)ch : '_');
    }
    // Пустое или состоящее из одних заменённых символов имя — не имя.
    if (name.find_first_not_of('_') == std::string::npos) name = "clip";
    return modelFileStem + "." + name + ".sageanim";
}

} // namespace sage::anim
