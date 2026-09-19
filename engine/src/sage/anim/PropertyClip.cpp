#include "sage/anim/PropertyClip.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "sage/core/Paths.h"

namespace sage::anim {

namespace {

using nlohmann::json;

const char* InterpName(Interp i) {
    switch (i) {
        case Interp::Constant: return "constant";
        case Interp::Bezier: return "bezier";
        default: return "linear";
    }
}

Interp InterpFromName(const std::string& s) {
    if (s == "constant") return Interp::Constant;
    if (s == "bezier") return Interp::Bezier;
    return Interp::Linear;
}

// Кубическая Безье по ОДНОЙ координате. Классическая формула, развёрнутая без
// степеней: pow на каждый кадр каждой дорожки — это тысячи вызовов там, где
// хватает трёх умножений.
float Bezier1(float p0, float p1, float p2, float p3, float u) {
    const float v = 1.0f - u;
    return v * v * v * p0 + 3.0f * v * v * u * p1 + 3.0f * v * u * u * p2 + u * u * u * p3;
}

// ПАРАМЕТР БЕЗЬЕ ПО ВРЕМЕНИ. Кривая задана в двух измерениях (время и
// значение), и «значение в момент t» — это НЕ Bezier(t): сначала надо найти
// параметр u, при котором ВРЕМЕННАЯ координата кривой равна t. Забыть этот шаг
// — самая частая ошибка в анимации: кривая рисуется правильно, а движение по
// ней идёт не с той скоростью, и объяснить это глядя на экран невозможно.
//
// Ищется делением пополам, а не аналитически: кубическое уравнение решается
// формулой Кардано, но она даёт три корня, требует отбора нужного и на
// вырожденных касательных теряет точность ровно там, где кривая почти прямая, —
// то есть в самом частом случае. Двадцать делений дают точность около 1e-6 от
// длины отрезка, и это дешевле одного корня кубического уравнения.
float SolveU(float t0, float c1, float c2, float t1, float t) {
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 20; ++i) {
        const float mid = (lo + hi) * 0.5f;
        if (Bezier1(t0, c1, c2, t1, mid) < t) lo = mid;
        else hi = mid;
    }
    return (lo + hi) * 0.5f;
}

} // namespace

float WrapTime(float time, float duration, bool loop) {
    if (duration <= 0.0f) return 0.0f;
    if (!loop) return std::clamp(time, 0.0f, duration);
    // std::fmod на отрицательном времени даёт отрицательный остаток: клип,
    // отмотанный назад, прыгал бы за начало. Доворачиваем вперёд.
    float t = std::fmod(time, duration);
    if (t < 0.0f) t += duration;
    return t;
}

glm::vec4 Sample(const Track& track, float time) {
    if (track.Keys.empty()) return glm::vec4(0.0f);
    if (track.Keys.size() == 1 || time <= track.Keys.front().Time) return track.Keys.front().Value;
    if (time >= track.Keys.back().Time) return track.Keys.back().Value;

    // Отрезок, в который попало время. Ключей в дорожке единицы-десятки, и
    // двоичный поиск здесь выиграл бы наносекунды ценой ветки, которую труднее
    // прочитать; но upper_bound уже написан и читается ровно как задумано.
    size_t i = 0;
    while (i + 1 < track.Keys.size() && track.Keys[i + 1].Time <= time) ++i;
    const Key& a = track.Keys[i];
    const Key& b = track.Keys[i + 1];

    if (a.Out == Interp::Constant) return a.Value;

    const float span = b.Time - a.Time;
    if (span <= 1e-6f) return b.Value;
    const float t = (time - a.Time) / span;

    if (a.Out == Interp::Linear) return glm::mix(a.Value, b.Value, t);

    // --- Безье --------------------------------------------------------------
    //
    // Отрезок считается в ДОЛЯХ САМОГО СЕБЯ: время 0..1, значение 0..1.
    // Управляющие точки — концы плюс касательные, и это ровно «cubic-bezier»
    // из веба: P0=(0,0), P1=конец+исходящая, P2=(1,1)+входящая, P3=(1,1).
    //
    // Касательная соседа берётся ВХОДЯЩАЯ: у ключа их две, и форма отрезка
    // складывается из исходящей левого и входящей правого — иначе стык двух
    // отрезков ломался бы даже на гладко настроенной кривой.
    const float x1 = a.OutTangent.x, y1 = a.OutTangent.y;
    const float x2 = 1.0f + b.InTangent.x, y2 = 1.0f + b.InTangent.y;
    const float u = SolveU(0.0f, x1, x2, 1.0f, t);

    // ЗНАЧЕНИЕ СЧИТАЕТСЯ ПО ВСЕМ ЧЕТЫРЁМ КОМПОНЕНТАМ ОДНОЙ ФОРМОЙ: кривая
    // задаёт характер движения, а не отдельную кривую на каждую ось.
    const float shape = Bezier1(0.0f, y1, y2, 1.0f, u);
    glm::vec4 out;
    for (int c = 0; c < 4; ++c) {
        const float d = b.Value[c] - a.Value[c];
        out[c] = a.Value[c] + d * shape;
    }
    return out;
}

int SetKey(Track& track, float time, const glm::vec4& value, Interp interp) {
    for (size_t i = 0; i < track.Keys.size(); ++i) {
        if (std::fabs(track.Keys[i].Time - time) < 1e-4f) {
            track.Keys[i].Value = value;
            track.Keys[i].Out = interp;
            return (int)i;
        }
    }
    Key k;
    k.Time = time;
    k.Value = value;
    k.Out = interp;
    track.Keys.push_back(k);
    std::sort(track.Keys.begin(), track.Keys.end(),
              [](const Key& a, const Key& b) { return a.Time < b.Time; });
    for (size_t i = 0; i < track.Keys.size(); ++i)
        if (std::fabs(track.Keys[i].Time - time) < 1e-4f) return (int)i;
    return 0;
}

bool RemoveKey(Track& track, int index) {
    if (index < 0 || index >= (int)track.Keys.size()) return false;
    track.Keys.erase(track.Keys.begin() + index);
    return true;
}

// --- Файл --------------------------------------------------------------------

std::string ToJsonString(const PropertyClip& clip) {
    json j;
    j["sage_clip_version"] = 1;
    j["name"] = clip.Name;
    j["duration"] = clip.Duration;
    j["loop"] = clip.Loop;
    j["imported"] = clip.Imported;
    j["tracks"] = json::array();
    for (const Track& t : clip.Tracks) {
        json tj;
        tj["target"] = t.Target;
        tj["property"] = t.Property;
        tj["keys"] = json::array();
        for (const Key& k : t.Keys) {
            json kj;
            kj["time"] = k.Time;
            kj["value"] = {k.Value.x, k.Value.y, k.Value.z, k.Value.w};
            kj["interp"] = InterpName(k.Out);
            // Касательные пишутся ТОЛЬКО у безье: у прямой и ступеньки они не
            // значат ничего, и файл, полный нулевых касательных, читается как
            // «здесь что-то настроено», хотя не настроено ничего.
            if (k.Out == Interp::Bezier) {
                kj["in"] = {k.InTangent.x, k.InTangent.y};
                kj["out"] = {k.OutTangent.x, k.OutTangent.y};
            }
            tj["keys"].push_back(kj);
        }
        j["tracks"].push_back(tj);
    }
    j["markers"] = json::array();
    for (const Marker& m : clip.Markers) {
        j["markers"].push_back({{"time", m.Time}, {"name", m.Name}});
    }
    return j.dump(2);
}

bool FromJsonString(const std::string& text, PropertyClip& out, std::string& err) {
    try {
        const json j = json::parse(text);
        PropertyClip c;
        c.Name = j.value("name", std::string());
        c.Duration = j.value("duration", 1.0f);
        c.Loop = j.value("loop", true);
        c.Imported = j.value("imported", false);
        if (j.contains("tracks") && j["tracks"].is_array()) {
            for (const json& tj : j["tracks"]) {
                Track t;
                t.Target = tj.value("target", std::string());
                t.Property = tj.value("property", std::string());
                if (tj.contains("keys") && tj["keys"].is_array()) {
                    for (const json& kj : tj["keys"]) {
                        Key k;
                        k.Time = kj.value("time", 0.0f);
                        if (kj.contains("value") && kj["value"].is_array()) {
                            for (size_t i = 0; i < 4 && i < kj["value"].size(); ++i)
                                k.Value[(int)i] = kj["value"][i].get<float>();
                        }
                        k.Out = InterpFromName(kj.value("interp", std::string("linear")));
                        if (kj.contains("in") && kj["in"].is_array() && kj["in"].size() == 2)
                            k.InTangent = {kj["in"][0].get<float>(), kj["in"][1].get<float>()};
                        if (kj.contains("out") && kj["out"].is_array() && kj["out"].size() == 2)
                            k.OutTangent = {kj["out"][0].get<float>(), kj["out"][1].get<float>()};
                        t.Keys.push_back(k);
                    }
                }
                // Порядок ключей — забота формата, а не того, кто его писал:
                // файл могли поправить руками, и несортированные ключи дали бы
                // движение рывками вместо ошибки.
                std::sort(t.Keys.begin(), t.Keys.end(),
                          [](const Key& a, const Key& b) { return a.Time < b.Time; });
                c.Tracks.push_back(std::move(t));
            }
        }
        if (j.contains("markers") && j["markers"].is_array()) {
            for (const json& mj : j["markers"]) {
                Marker m;
                m.Time = mj.value("time", 0.0f);
                m.Name = mj.value("name", std::string());
                c.Markers.push_back(m);
            }
        }
        out = std::move(c);
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

bool SaveClipFile(const PropertyClip& clip, const std::string& path, std::string& err) {
    std::ofstream f(sage::PathFromUtf8(path));
    if (!f) {
        err = "не удалось открыть файл для записи: " + path;
        return false;
    }
    f << ToJsonString(clip);
    return true;
}

bool LoadClipFile(const std::string& path, PropertyClip& out, std::string& err) {
    std::ifstream f(sage::PathFromUtf8(path));
    if (!f) {
        err = "не удалось открыть файл: " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return FromJsonString(ss.str(), out, err);
}

} // namespace sage::anim
