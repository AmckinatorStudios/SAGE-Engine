#include "ProjectThumbnail.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <system_error>

#include "stb_image.h"
#include "stb_image_write.h"

#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "ProjectDatabase.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/render/Texture.h"

namespace fs = std::filesystem;

namespace Sage::Launcher {

namespace {

using EditorTheme::Role;

// Хэш строки: по нему у каждого проекта своя, но ПОСТОЯННАЯ обложка. Постоянство
// важнее красоты: карточка, меняющая вид при каждом запуске, перестаёт быть
// приметой, по которой проект узнают в списке.
uint32_t Hash(const std::string& text) {
    uint32_t h = 2166136261u;
    for (unsigned char c : text) { h ^= c; h *= 16777619u; }
    return h;
}

// Уменьшение усреднением (box filter). Не «взять каждый N-й пиксель»: на
// снимке экрана прореживание превращает тонкие линии интерфейса в рваную кашу,
// и обложка выглядит грязной именно там, где на ней что-то есть.
std::vector<unsigned char> Downscale(const unsigned char* src, int sw, int sh, int dw, int dh) {
    std::vector<unsigned char> out((size_t)dw * dh * 4);
    for (int y = 0; y < dh; ++y) {
        const int y0 = y * sh / dh;
        const int y1 = std::max(y0 + 1, (y + 1) * sh / dh);
        for (int x = 0; x < dw; ++x) {
            const int x0 = x * sw / dw;
            const int x1 = std::max(x0 + 1, (x + 1) * sw / dw);
            unsigned int acc[4] = {0, 0, 0, 0};
            unsigned int count = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const unsigned char* row = src + ((size_t)sy * sw + x0) * 4;
                for (int sx = x0; sx < x1; ++sx, row += 4) {
                    acc[0] += row[0]; acc[1] += row[1]; acc[2] += row[2]; acc[3] += row[3];
                    ++count;
                }
            }
            unsigned char* dst = out.data() + ((size_t)y * dw + x) * 4;
            for (int c = 0; c < 4; ++c) dst[c] = (unsigned char)(acc[c] / std::max(1u, count));
        }
    }
    return out;
}

// Время файла в секундах эпохи файловой системы — только для сравнения «кэш
// свежее источника?», поэтому единицы не важны, важна монотонность.
long long FileStamp(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return (long long)t.time_since_epoch().count();
}

} // namespace

// ---------------------------------------------------------------------------
//  Кэш уменьшенных копий
// ---------------------------------------------------------------------------

fs::path ProjectThumbnail::CacheDir() {
    // Рядом с базой проектов: одна папка на все данные стартового окна.
    return fs::path(ProjectDatabase::StoragePath()).parent_path() / "thumbnails";
}

fs::path ProjectThumbnail::CachePathFor(const std::string& source) {
    // Имя файла — хэш пути плюс кусок имени проекта: по хэшу кэш однозначен, по
    // имени папку кэша можно читать глазами, когда что-то не так.
    char name[64];
    std::snprintf(name, sizeof(name), "%08x.png", Hash(source));
    return CacheDir() / name;
}

// ---------------------------------------------------------------------------
//  Фоновая загрузка
// ---------------------------------------------------------------------------

ProjectThumbnail::~ProjectThumbnail() { Shutdown(); }

void ProjectThumbnail::Shutdown() {
    if (m_worker.joinable()) {
        m_stop = true;
        m_wake.notify_all();
        m_worker.join();
    }
    m_items.clear();   // текстуры отпускаются, пока контекст ещё жив
    m_ready.clear();
    m_queue.clear();
    m_pending = 0;
}

void ProjectThumbnail::EnsureWorker() {
    if (m_worker.joinable()) return;
    m_stop = false;
    m_worker = std::thread([this] {
        // Свой порядок строк: движок ставит глобальный флаг переворота на
        // время своих загрузок (см. render/Texture.cpp), и без этой строки
        // обложка приезжала бы то так, то вверх ногами — в зависимости от
        // того, что грузилось в главном потоке в ту же секунду.
        stbi_set_flip_vertically_on_load_thread(0);
        for (;;) {
            std::string job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [this] { return m_stop.load() || !m_queue.empty(); });
                if (m_stop) return;
                job = m_queue.front();
                m_queue.pop_front();
            }

            Decoded result;
            result.Source = job;

            const fs::path cache = CachePathFor(job);
            const bool cacheFresh = FileStamp(cache) != 0 && FileStamp(cache) >= FileStamp(job);

            int w = 0, h = 0, comp = 0;
            unsigned char* pixels = nullptr;
            if (cacheFresh) pixels = stbi_load(sage::PathToUtf8(cache).c_str(), &w, &h, &comp, 4);
            if (!pixels) pixels = stbi_load(job.c_str(), &w, &h, &comp, 4);
            if (!pixels || w <= 0 || h <= 0) {
                result.Failed = true;
            } else if (cacheFresh) {
                result.W = w;
                result.H = h;
                result.Pixels.assign(pixels, pixels + (size_t)w * h * 4);
            } else {
                // Вписываем в предельный размер, сохраняя пропорции: обложка
                // отвечает на вопрос «что это за проект», и растянутая картинка
                // отвечает на него неправильно.
                const float k = std::min((float)kThumbWidth / (float)w,
                                         (float)kThumbHeight / (float)h);
                const int dw = std::max(1, (int)std::lround(w * std::min(k, 1.0f)));
                const int dh = std::max(1, (int)std::lround(h * std::min(k, 1.0f)));
                result.W = dw;
                result.H = dh;
                result.Pixels = (dw == w && dh == h)
                                    ? std::vector<unsigned char>(pixels, pixels + (size_t)w * h * 4)
                                    : Downscale(pixels, w, h, dw, dh);
                std::error_code ec;
                fs::create_directories(cache.parent_path(), ec);
                stbi_write_png(sage::PathToUtf8(cache).c_str(), dw, dh, 4, result.Pixels.data(),
                               dw * 4);
            }
            if (pixels) stbi_image_free(pixels);

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_ready.push_back(std::move(result));
            }
            --m_pending;
        }
    });
}

void ProjectThumbnail::Request(const std::string& source) {
    EnsureWorker();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(source);
    }
    ++m_pending;
    m_wake.notify_one();
}

void ProjectThumbnail::Pump() {
    ++m_frame;

    std::vector<Decoded> ready;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // НЕ БОЛЬШЕ ДВУХ ТЕКСТУР ЗА КАДР. Заливка в видеопамять — работа
        // главного потока, и двенадцать картинок разом дают заметный рывок
        // ровно в тот момент, когда человек листает список.
        const size_t take = std::min<size_t>(m_ready.size(), 2);
        ready.assign(std::make_move_iterator(m_ready.begin()),
                     std::make_move_iterator(m_ready.begin() + (long)take));
        m_ready.erase(m_ready.begin(), m_ready.begin() + (long)take);
    }

    for (Decoded& d : ready) {
        Item& item = m_items[d.Source];
        item.Requested = false;
        if (d.Failed || d.Pixels.empty()) {
            item.Failed = true;
            continue;
        }
        item.Tex = std::make_shared<Texture>(d.Pixels.data(), d.W, d.H, TextureFilter::Bilinear,
                                             false);
        item.UsedFrame = m_frame;
    }
    EvictIfNeeded();
}

void ProjectThumbnail::EvictIfNeeded() {
    int alive = 0;
    for (const auto& [key, item] : m_items) if (item.Tex) ++alive;
    while (alive > kMaxTextures) {
        auto oldest = m_items.end();
        for (auto it = m_items.begin(); it != m_items.end(); ++it) {
            if (!it->second.Tex) continue;
            if (it->second.UsedFrame == m_frame) continue;   // на экране прямо сейчас
            if (oldest == m_items.end() || it->second.UsedFrame < oldest->second.UsedFrame)
                oldest = it;
        }
        if (oldest == m_items.end()) break;
        oldest->second.Tex.reset();
        --alive;
    }
}

int ProjectThumbnail::LoadedCount() const {
    int n = 0;
    for (const auto& [key, item] : m_items) if (item.Tex) ++n;
    return n;
}

int ProjectThumbnail::PendingCount() const { return m_pending.load(); }

// ---------------------------------------------------------------------------
//  Рисование
// ---------------------------------------------------------------------------

void ProjectThumbnail::Draw(const ProjectEntry& entry, const ImVec2& min, const ImVec2& max,
                            float rounding) {
    const std::string& source = entry.Thumbnail;
    if (source.empty() || entry.Missing) {
        DrawGenerated(entry, min, max, rounding);
        return;
    }

    Item& item = m_items[source];
    item.UsedFrame = m_frame;
    if (!item.Tex) {
        if (!item.Requested && !item.Failed) {
            item.Requested = true;
            Request(source);
        }
        // Пока грузится — своя обложка. Пустое место на её месте читалось бы
        // как «картинки нет», и разницы с «ещё не загрузилась» человек не
        // увидел бы.
        DrawGenerated(entry, min, max, rounding);
        return;
    }

    // ВПИСЫВАЕМ ПО ЗАПОЛНЕНИЮ, лишнее срезаем по краям: поля вокруг картинки в
    // сетке одинаковых карточек читаются как брак вёрстки, а не как честные
    // пропорции.
    const float boxW = max.x - min.x, boxH = max.y - min.y;
    const float texW = (float)item.Tex->Width(), texH = (float)item.Tex->Height();
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (texW > 0.0f && texH > 0.0f && boxW > 0.0f && boxH > 0.0f) {
        const float boxAspect = boxW / boxH;
        const float texAspect = texW / texH;
        if (texAspect > boxAspect) {
            const float keep = boxAspect / texAspect;      // срезаем по бокам
            uv0.x = (1.0f - keep) * 0.5f;
            uv1.x = 1.0f - uv0.x;
        } else {
            const float keep = texAspect / boxAspect;      // срезаем сверху и снизу
            uv0.y = (1.0f - keep) * 0.5f;
            uv1.y = 1.0f - uv0.y;
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddImageRounded((ImTextureID)(std::intptr_t)item.Tex->NativeHandle(), min, max, uv0, uv1,
                        IM_COL32_WHITE, rounding, ImDrawFlags_RoundCornersTop);
}

void ProjectThumbnail::DrawGenerated(const ProjectEntry& entry, const ImVec2& min,
                                     const ImVec2& max, float rounding) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = max.x - min.x, h = max.y - min.y;
    if (w <= 1.0f || h <= 1.0f) return;

    // Оттенок — из имени проекта: соседние карточки не сливаются, а один и тот
    // же проект всегда одного цвета.
    const uint32_t hash = Hash(entry.Name.empty() ? entry.Path : entry.Name);
    const float hue = (float)(hash % 360u) / 360.0f;
    float tr, tg, tb;
    ImGui::ColorConvertHSVtoRGB(hue, 0.30f, 0.34f, tr, tg, tb);

    const ImVec4 base = EditorTheme::Color(Role::SurfaceAlt);
    // Смешиваем с поверхностью темы, а не красим наотмашь: обложка обязана
    // остаться частью экрана, а не пятном поверх него. В светлой теме тот же
    // оттенок берётся светлее — иначе карточки станут тёмными дырами.
    const float k = EditorTheme::Current().Dark ? 0.55f : 0.22f;
    const ImVec4 top(base.x + (tr - base.x) * k, base.y + (tg - base.y) * k,
                     base.z + (tb - base.z) * k, 1.0f);
    const ImVec4 bottom(base.x * 0.85f, base.y * 0.85f, base.z * 0.9f, 1.0f);
    const ImU32 topCol = ImGui::GetColorU32(top);
    const ImU32 bottomCol = ImGui::GetColorU32(bottom);

    // Форма — скруглённым прямоугольником, градиент — поверх него внутри
    // обрезки: AddRectFilledMultiColor скруглять не умеет, а карточка обязана
    // быть одной формы со своей рамкой.
    dl->AddRectFilled(min, max, topCol, rounding, ImDrawFlags_RoundCornersTop);
    dl->PushClipRect(min, max, true);
    const ImU32 fade = ImGui::GetColorU32(ImVec4(bottom.x, bottom.y, bottom.z, 0.0f));
    dl->AddRectFilledMultiColor(ImVec2(min.x, min.y + rounding), max, fade, fade, bottomCol,
                                bottomCol);
    dl->PopClipRect();

    // «Горизонт» и две-три горы: узнаваемый силуэт сцены, а не абстрактное
    // пятно. Форма считается из того же хэша — обложка проекта постоянна.
    const float horizon = min.y + h * 0.66f;
    const ImU32 ridge = ImGui::GetColorU32(ImVec4(bottom.x * 0.7f, bottom.y * 0.7f,
                                                  bottom.z * 0.8f, 1.0f));
    dl->PushClipRect(min, max, true);
    for (int i = 0; i < 3; ++i) {
        const float cx = min.x + w * (0.18f + 0.32f * (float)i) +
                         (float)((hash >> (i * 5)) % 40u) / 40.0f * w * 0.12f;
        const float peak = horizon - h * (0.16f + (float)((hash >> (i * 3)) % 24u) / 24.0f * 0.26f);
        const float half = w * (0.16f + (float)((hash >> (i * 7)) % 16u) / 16.0f * 0.12f);
        dl->AddTriangleFilled(ImVec2(cx - half, horizon + h * 0.2f), ImVec2(cx, peak),
                              ImVec2(cx + half, horizon + h * 0.2f), ridge);
    }
    // Линия горизонта акцентом, приглушённо: тот самый жёлтый SAGE, но как
    // деталь, а не как заливка.
    dl->AddLine(ImVec2(min.x, horizon), ImVec2(max.x, horizon),
                EditorTheme::Color32Alpha(Role::Accent, 0.35f), 1.0f);
    dl->PopClipRect();

    // Значок типа проекта в углу — по нему видно, что это, даже без подписи.
    const char* icon = entry.Kind == ProjectKind::Scene ? "scene"
                       : entry.Kind == ProjectKind::Sample ? "material"
                       : "cube";
    const float glyph = std::floor(std::min(w, h) * 0.22f);
    const ImVec4 dim = EditorTheme::Color(Role::TextFaint);
    EditorIcons::Overlay(min.x + w * 0.5f - glyph * 0.5f, min.y + h * 0.30f - glyph * 0.5f, glyph,
                         icon, glm::vec3(dim.x, dim.y, dim.z));
}

} // namespace Sage::Launcher
