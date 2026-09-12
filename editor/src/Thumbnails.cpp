#include "Thumbnails.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "imgui.h"

#include "Localization.h"
#include "sage/assets/format/TextureFormat.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Texture.h"

namespace fs = std::filesystem;

namespace thumbs {

namespace {

// Сторона обложки. 256 для плитки — это вчетверо больше самой крупной площадки,
// в которой её рисуют (80), и запас здесь не роскошь: обложку показывают и в
// увеличенном превью при наведении, и на экране с двукратным масштабом, а
// картинка, подготовленная ровно в размер площадки, на них снова становится
// мылом. 1024 для превью — предел, за которым разглядывать уже нечего: превью
// всё равно не больше половины окна.
constexpr int kTileSide = 256;
constexpr int kLargeSide = 1024;

// Потолок видеопамяти под обложки. Считается по факту (GpuBytes), а не по числу
// записей: обложка панорамы 1024x256 весит впятеро меньше квадратной, и
// ограничение штуками отмеряло бы не то, что кончается.
constexpr size_t kBudgetBytes = 96ull * 1024 * 1024;

// Сколько обложек заливать в видеопамять за кадр. Заливка — это создание
// текстуры и построение мипмапов, то есть работа на главном потоке; десяток
// подряд снова даёт ту самую просадку, ради которой всё и затевалось.
constexpr int kUploadsPerFrame = 4;

// Через сколько кадров без единого взгляда обложка отпускается (~15 секунд при
// 60 кадрах). Диалог закрыли — видеопамять вернулась сама.
constexpr int kIdleFrames = 900;

// Потолок очереди задач. Пролистав папку на десять тысяч файлов, человек ставит
// в очередь десять тысяч декодирований, из которых видит полсотни.
constexpr size_t kMaxJobs = 512;

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// --- Уменьшение --------------------------------------------------------------

// Точная подгонка усреднением площадей. Зовётся ПОСЛЕ цепочки делений пополам,
// поэтому уменьшает меньше чем вдвое: на каждый пиксель результата приходится
// от одного до четырёх исходных, и усреднить их достаточно простым циклом.
void ResampleArea(const std::vector<unsigned char>& src, int w, int h,
                  std::vector<unsigned char>& dst, int dw, int dh) {
    dst.assign((size_t)dw * dh * 4u, 0);
    if (w <= 0 || h <= 0 || dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; ++y) {
        const int y0 = (int)((int64_t)y * h / dh);
        const int y1 = std::max(y0 + 1, (int)((int64_t)(y + 1) * h / dh));
        for (int x = 0; x < dw; ++x) {
            const int x0 = (int)((int64_t)x * w / dw);
            const int x1 = std::max(x0 + 1, (int)((int64_t)(x + 1) * w / dw));
            int acc[4] = {0, 0, 0, 0};
            int n = 0;
            for (int sy = y0; sy < y1 && sy < h; ++sy) {
                for (int sx = x0; sx < x1 && sx < w; ++sx) {
                    const unsigned char* p = &src[((size_t)sy * w + sx) * 4];
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
                    ++n;
                }
            }
            if (n == 0) n = 1;
            unsigned char* o = &dst[((size_t)y * dw + x) * 4];
            for (int c = 0; c < 4; ++c) o[c] = (unsigned char)((acc[c] + n / 2) / n);
        }
    }
}

// Ужимает картинку так, чтобы бОльшая сторона стала не длиннее target.
//
// Сначала делениями пополам (усреднение 2x2 — та же функция, которой движок
// понижает текстуры при нехватке видеопамяти), и только последний шаг — точной
// подгонкой. Так и быстрее (каждое деление вчетверо сокращает работу
// следующего), и ровнее: усреднение по всем пикселям, а не выборка каждого
// N-ного, — это и есть разница между обложкой и рябью.
void FitDown(std::vector<unsigned char>& px, int& w, int& h, int target) {
    if (target < 1) target = 1;
    std::vector<unsigned char> tmp;
    while (std::max(w, h) >= target * 2 && std::max(w, h) > 1) {
        int nw = 0, nh = 0;
        ResourceManager::DownscaleRGBA(px, w, h, tmp, nw, nh);
        px.swap(tmp);
        w = nw;
        h = nh;
    }
    if (std::max(w, h) <= target) return;
    const double k = (double)target / (double)std::max(w, h);
    const int nw = std::max(1, (int)std::lround(w * k));
    const int nh = std::max(1, (int)std::lround(h * k));
    ResampleArea(px, w, h, tmp, nw, nh);
    px.swap(tmp);
    w = nw;
    h = nh;
}

// --- Что это на самом деле за файл -------------------------------------------

// Формат по первым байтам, а не по расширению.
//
// ЗАЧЕМ. Сообщение «unknown image type» ничего не объясняет: человек видит
// .png, редактор говорит «неизвестный тип», и вывод из этого один — «редактор
// сломался». А на деле файл с расширением .png сплошь и рядом оказывается WEBP:
// так их сохраняет половина сетевых редакторов и так их отдают браузеры. Ответ
// «на самом деле это WEBP» превращает загадку в понятное действие.
const char* SniffFormat(const std::string& file) {
    std::FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) return nullptr;
    unsigned char b[16] = {0};
    const size_t n = std::fread(b, 1, sizeof(b), f);
    std::fclose(f);
    if (n < 4) return nullptr;
    if (!std::memcmp(b, "RIFF", 4) && n >= 12 && !std::memcmp(b + 8, "WEBP", 4)) return "WEBP";
    if (n >= 12 && !std::memcmp(b + 4, "ftyp", 4)) {
        if (!std::memcmp(b + 8, "avif", 4) || !std::memcmp(b + 8, "avis", 4)) return "AVIF";
        if (!std::memcmp(b + 8, "heic", 4) || !std::memcmp(b + 8, "heix", 4) ||
            !std::memcmp(b + 8, "mif1", 4))
            return "HEIC";
    }
    if (!std::memcmp(b, "II*\0", 4) || !std::memcmp(b, "MM\0*", 4)) return "TIFF";
    if (!std::memcmp(b, "DDS ", 4)) return "DDS";
    if (!std::memcmp(b, "\x76\x2F\x31\x01", 4)) return "OpenEXR";
    if (b[0] == 0xFF && b[1] == 0x0A) return "JPEG XL";
    if (!std::memcmp(b, "\0\0\1\0", 4)) return "ICO";
    if (!std::memcmp(b, "%PDF", 4)) return "PDF";
    if (!std::memcmp(b, "<svg", 4) || !std::memcmp(b, "<?xm", 4)) return "SVG";
    return nullptr;
}

// --- Фоновое декодирование ---------------------------------------------------

struct Job {
    std::string Key;
    fs::path Path;     // чем проверять существование
    std::string File;  // чем открывать (stb принимает узкую строку)
    int Target = kTileSide;
};

struct Done {
    std::string Key;
    std::vector<unsigned char> Pixels;
    int W = 0, H = 0;        // размер обложки
    int SrcW = 0, SrcH = 0;  // размер ИСХОДНОЙ картинки
    bool Ok = false;
    bool Missing = false;    // файла нет вовсе
    const char* Format = nullptr; // чем он оказался на самом деле
};

struct Entry {
    std::shared_ptr<Texture> Tex;
    int SrcW = 0, SrcH = 0;
    bool Loading = false;
    bool Failed = false;
    std::string Error;
    long long Stamp = 0;   // время правки файла: правка картинки обновляет обложку
    int LastFrame = 0;     // когда на неё смотрели (для отпускания)
    int CheckFrame = 0;    // когда последний раз сверяли время правки
    size_t Bytes = 0;
};

struct State {
    std::unordered_map<std::string, Entry> Cache;
    size_t Bytes = 0;

    std::mutex JobMx;
    std::condition_variable JobCv;
    // СТЕК, а не очередь. Пролистав папку, человек ставит в очередь сотни
    // задач, а смотрит на те полсотни плиток, что под курсором сейчас. Очередь
    // отдала бы ему сначала то, что уже уехало за край экрана, — то есть
    // обложки появлялись бы в порядке, обратном нужному.
    std::vector<Job> Jobs;
    std::mutex DoneMx;
    std::deque<Done> Ready;
    std::vector<std::thread> Workers;
    std::atomic<bool> Stop{false};
    bool Started = false;

    // Потоки останавливаются ДО того, как начнут разрушаться очереди и мьютексы,
    // которых они касаются. Без этого выход из редактора — гонка: главный поток
    // разбирает статические объекты, а воркер в этот момент ждёт на условной
    // переменной, которой уже нет.
    ~State() {
        Stop.store(true);
        JobCv.notify_all();
        for (std::thread& t : Workers) {
            if (t.joinable()) t.join();
        }
    }
};

State& S() {
    static State state;
    return state;
}

bool DecodeNative(const std::string& file, int target, Done& out) {
    sage::assets::TextureData tex;
    std::string err;
    if (!sage::assets::ReadTextureFile(file, tex, err)) return false;
    out.SrcW = tex.Width;
    out.SrcH = tex.Height;
    // Мип-уровни у своего формата ПОСЧИТАНЫ ЗАРАНЕЕ — берём сразу тот, что
    // близок к нужному размеру. Разжимать уровень 4096 ради обложки 256, когда
    // в том же файле лежит готовый 512-й, незачем.
    int level = 0;
    int side = std::max(tex.Width, tex.Height);
    while (level + 1 < (int)tex.Levels.size() && side >= target * 2) {
        ++level;
        side = std::max(1, side / 2);
    }
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!sage::assets::DecodeToRGBA(tex, level, rgba, w, h)) return false;
    // .sagetex хранит строки сверху вниз, а рисуем мы, как и весь движок, с
    // (0,0) внизу — переворачиваем здесь, чтобы обложка не встала на голову.
    const size_t rowBytes = (size_t)w * 4;
    for (int y = 0; y < h / 2; ++y) {
        uint8_t* a = rgba.data() + (size_t)y * rowBytes;
        uint8_t* b = rgba.data() + (size_t)(h - 1 - y) * rowBytes;
        std::swap_ranges(a, a + rowBytes, b);
    }
    out.Pixels.assign(rgba.begin(), rgba.end());
    out.W = w;
    out.H = h;
    FitDown(out.Pixels, out.W, out.H, target);
    out.Ok = true;
    return true;
}

void RunJob(const Job& job, Done& out) {
    out.Key = job.Key;
    std::error_code ec;
    if (!fs::exists(job.Path, ec)) {
        out.Missing = true;
        return;
    }
    if (Lower(job.Path.extension().string()) == ".sagetex") {
        DecodeNative(job.File, job.Target, out);
        return;
    }
    std::vector<unsigned char> px;
    int w = 0, h = 0;
    if (!ResourceManager::DecodeImageFile(job.File, px, w, h)) {
        out.Format = SniffFormat(job.File);
        return;
    }
    out.SrcW = w;
    out.SrcH = h;
    FitDown(px, w, h, job.Target);
    out.Pixels = std::move(px);
    out.W = w;
    out.H = h;
    out.Ok = true;
}

void WorkerLoop() {
    State& s = S();
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(s.JobMx);
            s.JobCv.wait(lk, [&] { return s.Stop.load() || !s.Jobs.empty(); });
            if (s.Stop.load()) return;
            job = std::move(s.Jobs.back());
            s.Jobs.pop_back();
        }
        Done done;
        RunJob(job, done);
        {
            std::lock_guard<std::mutex> lk(s.DoneMx);
            s.Ready.push_back(std::move(done));
        }
    }
}

void StartWorkers() {
    State& s = S();
    if (s.Started) return;
    s.Started = true;
    // Два потока, а не один: декодирование фотографии — это десятки
    // миллисекунд, и папка из сотни снимков на одном потоке раскрывается вдвое
    // дольше. Больше двух не берём: файлы читаются с одного диска, и очередь к
    // нему от лишних потоков только растёт.
    const unsigned n = std::min(2u, std::max(1u, std::thread::hardware_concurrency() / 2));
    for (unsigned i = 0; i < n; ++i) s.Workers.emplace_back(WorkerLoop);
}

void Release(State& s, Entry& e) {
    s.Bytes -= std::min(s.Bytes, e.Bytes);
    e.Tex.reset();
    e.Bytes = 0;
}

long long StampOf(const fs::path& path) {
    std::error_code ec;
    const auto w = fs::last_write_time(path, ec);
    return ec ? 0 : (long long)w.time_since_epoch().count();
}

} // namespace

bool IsImage(const fs::path& path) {
    const std::string ext = Lower(path.extension().string());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" ||
           ext == ".gif" || ext == ".psd" || ext == ".hdr" || ext == ".pic" || ext == ".ppm" ||
           ext == ".pgm" || ext == ".sagetex";
}

Thumb Get(const fs::path& path, Size size) {
    Thumb out;
    if (path.empty() || !IsImage(path)) return out;

    State& s = S();
    const int target = size == Size::Large ? kLargeSide : kTileSide;
    const std::string file = path.string();
    const std::string key = file + (size == Size::Large ? "|L" : "|T");
    const int frame = ImGui::GetFrameCount();

    Entry& e = s.Cache[key];
    e.LastFrame = frame;

    // Правку картинки снаружи (Photoshop, GIMP) обложка обязана заметить, но
    // спрашивать у диска время правки каждой плитки КАЖДЫЙ кадр — это полсотни
    // обращений к файловой системе на кадр ни за чем. Раз в две секунды.
    if (!e.Loading && (e.Tex || e.Failed) && frame - e.CheckFrame > 120) {
        e.CheckFrame = frame;
        if (StampOf(path) != e.Stamp) {
            Release(s, e);
            e = Entry{};
            e.LastFrame = frame;
        }
    }

    if (!e.Tex && !e.Loading && !e.Failed) {
        e.Loading = true;
        e.Stamp = StampOf(path);
        e.CheckFrame = frame;
        StartWorkers();
        {
            std::lock_guard<std::mutex> lk(s.JobMx);
            // Превью под курсором — одно и прямо сейчас; плиток полсотни и они
            // подождут. Поэтому крупная задача кладётся на самый верх стека.
            if (s.Jobs.size() >= kMaxJobs) s.Jobs.erase(s.Jobs.begin());
            s.Jobs.push_back(Job{key, path, file, target});
        }
        s.JobCv.notify_one();
    }

    out.Id = e.Tex ? e.Tex->NativeHandle() : 0;
    out.W = e.SrcW;
    out.H = e.SrcH;
    out.Loading = e.Loading;
    out.Failed = e.Failed;
    out.Error = e.Error;
    return out;
}

int Pump() {
    State& s = S();
    if (!s.Started) return 0;

    // Забираем ровно столько, сколько успеем залить за кадр; остальное дождётся
    // следующего — оно уже декодировано и никуда не денется.
    std::vector<Done> take;
    {
        std::lock_guard<std::mutex> lk(s.DoneMx);
        while (!s.Ready.empty() && (int)take.size() < kUploadsPerFrame) {
            take.push_back(std::move(s.Ready.front()));
            s.Ready.pop_front();
        }
    }

    int uploaded = 0;
    for (Done& d : take) {
        auto it = s.Cache.find(d.Key);
        if (it == s.Cache.end()) continue;   // запись успели отпустить
        Entry& e = it->second;
        e.Loading = false;
        if (!d.Ok) {
            e.Failed = true;
            if (d.Missing) {
                e.Error = T("The file is gone");
            } else if (d.Format) {
                char buf[256];
                std::snprintf(buf, sizeof(buf), T("This is actually %s — the format is not supported"),
                              d.Format);
                e.Error = buf;
            } else {
                e.Error = T("The file could not be read as an image");
            }
            continue;
        }
        if (d.W <= 0 || d.H <= 0 || d.Pixels.size() < (size_t)d.W * d.H * 4u) {
            e.Failed = true;
            e.Error = T("The file could not be read as an image");
            continue;
        }
        e.SrcW = d.SrcW;
        e.SrcH = d.SrcH;
        // Мелкой картинке мипмапы не нужны, а сглаживание вредно: спрайт 32x32,
        // растянутый до плитки, обязан остаться пиксель-артом, а не мылом.
        const bool tiny = std::max(d.SrcW, d.SrcH) <= 64;
        try {
            e.Tex = std::make_shared<Texture>(d.Pixels.data(), d.W, d.H,
                                              tiny ? TextureFilter::Nearest : TextureFilter::Trilinear,
                                              !tiny);
            e.Bytes = e.Tex->GpuBytes();
            s.Bytes += e.Bytes;
            ++uploaded;
        } catch (const std::exception&) {
            e.Failed = true;
            e.Error = T("The cover did not fit into video memory");
        }
    }

    // --- Отпускание ---------------------------------------------------------
    const int frame = ImGui::GetFrameCount();
    std::vector<std::pair<int, std::string>> old;   // (кадр последнего взгляда, ключ)
    for (auto& kv : s.Cache) {
        Entry& e = kv.second;
        if (e.Loading) continue;
        if (frame - e.LastFrame > kIdleFrames) {
            Release(s, e);
            e.Failed = false;
            e.Error.clear();
            e.SrcW = e.SrcH = 0;
            continue;
        }
        if (e.Tex && e.LastFrame != frame) old.emplace_back(e.LastFrame, kv.first);
    }
    if (s.Bytes > kBudgetBytes) {
        std::sort(old.begin(), old.end());
        for (auto& p : old) {
            if (s.Bytes <= kBudgetBytes) break;
            Release(s, s.Cache[p.second]);
        }
    }
    // Пустые записи (отпущенные и не просимые) не копим: иначе таблица растёт
    // на каждый просмотренный файл и не уменьшается никогда.
    for (auto it = s.Cache.begin(); it != s.Cache.end();) {
        if (!it->second.Tex && !it->second.Loading && !it->second.Failed &&
            frame - it->second.LastFrame > kIdleFrames) {
            it = s.Cache.erase(it);
        } else {
            ++it;
        }
    }
    return uploaded;
}

void Clear() {
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.JobMx);
        s.Jobs.clear();
    }
    {
        std::lock_guard<std::mutex> lk(s.DoneMx);
        s.Ready.clear();
    }
    for (auto& kv : s.Cache) Release(s, kv.second);
    s.Cache.clear();
    s.Bytes = 0;
}

} // namespace thumbs
