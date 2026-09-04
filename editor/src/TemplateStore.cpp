#include "TemplateStore.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include <nlohmann/json.hpp>

#include "EditorPrefs.h"
#include "sage/assets/Pack.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace sage::editor::templates {

namespace {

// Файл описания внутри шаблона. Лежит и в установленной папке, и в пакете:
// без него шаблон — просто папка с проектом, и в списке он назывался бы
// именем каталога.
constexpr const char* kManifest = "template.json";

Manifest ReadManifest(const fs::path& file, const std::string& fallbackId) {
    Manifest m;
    m.Id = fallbackId;
    m.Name = fallbackId;
    std::ifstream in(file);
    if (!in.is_open()) return m;
    try {
        json root;
        in >> root;
        if (!root.is_object()) return m;
        m.Id = root.value("id", m.Id);
        m.Name = root.value("name", m.Id);
        m.Summary = root.value("summary", std::string());
        m.Note = root.value("note", std::string());
        m.Version = root.value("version", std::string());
        m.Url = root.value("url", std::string());
        m.Bytes = root.value("bytes", (uint64_t)0);
    } catch (const std::exception& e) {
        // Испорченное описание — это шаблон без подписи, а не отсутствие
        // шаблона: проект в папке от этого никуда не делся.
        LOG_WARN("Editor") << "Описание шаблона " << sage::PathToUtf8(file)
                           << " не читается: " << e.what();
    }
    return m;
}

void WriteManifest(const fs::path& file, const Manifest& m) {
    json root;
    root["id"] = m.Id;
    root["name"] = m.Name;
    root["summary"] = m.Summary;
    root["note"] = m.Note;
    root["version"] = m.Version;
    std::ofstream out(file);
    if (out.is_open()) out << root.dump(2) << "\n";
}

// Имя папки шаблона — из идентификатора, и только безопасные символы. Пакет
// приходит из сети и с чужого диска: «id» вида "../../assets" распаковал бы
// шаблон поверх редактора.
std::string SafeId(const std::string& id) {
    std::string out;
    for (char c : id) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
        out.push_back(ok ? c : '-');
    }
    if (out.empty()) out = "template";
    return out;
}

// Путь внутри пакета безопасен, если он относительный и никуда не поднимается.
// Проверяется КАЖДЫЙ, а не только подозрительный: пакет — чужой файл, и
// «..\\..\\Windows\\System32» в нём написать не сложнее, чем "scenes/main.sage".
bool SafeRelative(const std::string& p) {
    if (p.empty() || p.front() == '/' || p.front() == '\\') return false;
    if (p.size() > 1 && p[1] == ':') return false;              // C:\...
    if (p.find("..") != std::string::npos) return false;
    return true;
}

bool CopyTree(const fs::path& from, const fs::path& to, std::string& err) {
    std::error_code ec;
    fs::create_directories(to, ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        err = ec.message();
        return false;
    }
    return true;
}

// Запуск внешней программы с чтением её вывода. Через popen: редактору нужен
// один ответ целиком, а не поток.
bool RunCapture(const std::string& command, std::string& out) {
    out.clear();
#if defined(_WIN32)
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) return false;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
#if defined(_WIN32)
    const int rc = _pclose(pipe);
#else
    const int rc = pclose(pipe);
#endif
    return rc == 0;
}

// Адрес в командную строку без права что-то в ней исполнить. Кавычки и всё,
// что оболочка трактует как команду, отбрасываются: ссылка приходит из файла
// каталога и из поля ввода, а строка уходит в оболочку.
bool SafeUrl(const std::string& url) {
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) return false;
    for (unsigned char c : url) {
        if (c <= 0x20 || c >= 0x7f) return false;
        if (std::string("\"'`$;&|<>\\^(){}[]*?!").find((char)c) != std::string::npos) return false;
    }
    return true;
}

} // namespace

fs::path Root() {
    const fs::path exe = sage::ExecutableDir();
    return exe.empty() ? fs::path("templates") : exe / "templates";
}

std::vector<Manifest> Installed() {
    std::vector<Manifest> out;
    std::error_code ec;
    const fs::path root = Root();
    if (!fs::is_directory(root, ec)) return out;
    for (const fs::directory_entry& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory()) continue;
        // Признак шаблона — файл проекта, а не имя папки: недокопированная или
        // пустая папка не должна показываться как готовый к работе шаблон.
        if (!fs::exists(e.path() / "project.sageproj", ec)) continue;
        Manifest m = ReadManifest(e.path() / kManifest, sage::PathToUtf8(e.path().filename()));
        m.Id = sage::PathToUtf8(e.path().filename()); // папка — источник правды
        m.Installed = true;
        out.push_back(m);
    }
    std::sort(out.begin(), out.end(),
              [](const Manifest& a, const Manifest& b) { return a.Id < b.Id; });
    return out;
}

bool InstallFromFile(const fs::path& file, std::string& err) {
    sage::assets::PackReader pack;
    if (!pack.Open(file)) {
        err = "Файл не читается как шаблон SAGE: " + sage::PathToUtf8(file);
        return false;
    }
    const std::vector<std::string> paths = pack.Paths();
    if (paths.empty()) {
        err = "В шаблоне нет ни одного файла";
        return false;
    }
    // Сначала — описание: под каким именем ставить, знает оно.
    Manifest info;
    if (std::vector<uint8_t> raw; pack.Read(kManifest, raw)) {
        try {
            json root = json::parse(std::string(raw.begin(), raw.end()));
            info.Id = root.value("id", std::string());
            info.Name = root.value("name", info.Id);
            info.Summary = root.value("summary", std::string());
            info.Note = root.value("note", std::string());
            info.Version = root.value("version", std::string());
        } catch (const std::exception&) {
            // Описание испорчено — имя возьмём от файла, содержимое уцелело.
        }
    }
    if (info.Id.empty()) info.Id = sage::PathToUtf8(file.stem());
    info.Id = SafeId(info.Id);
    if (info.Name.empty()) info.Name = info.Id;

    if (!pack.Contains("project.sageproj")) {
        err = "Это не шаблон проекта: внутри нет project.sageproj";
        return false;
    }

    // Распаковываем во ВРЕМЕННУЮ папку рядом и переносим целиком. Так
    // прерванная на середине установка не оставляет полушаблон, который
    // выглядит рабочим: у него уже есть project.sageproj, а половины сцен нет.
    std::error_code ec;
    const fs::path root = Root();
    fs::create_directories(root, ec);
    const fs::path target = root / sage::PathFromUtf8(info.Id);
    const fs::path staging = root / sage::PathFromUtf8("." + info.Id + ".part");
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);

    for (const std::string& p : paths) {
        if (!SafeRelative(p)) {
            fs::remove_all(staging, ec);
            err = "Шаблон пытается записать файл за пределы своей папки: " + p;
            return false;
        }
        std::vector<uint8_t> data;
        if (!pack.Read(p, data)) {
            fs::remove_all(staging, ec);
            err = "Шаблон повреждён, не читается: " + p;
            return false;
        }
        const fs::path dst = staging / sage::PathFromUtf8(p);
        fs::create_directories(dst.parent_path(), ec);
        std::ofstream out(dst, std::ios::binary);
        if (!out.is_open()) {
            fs::remove_all(staging, ec);
            err = "Не записать файл шаблона: " + sage::PathToUtf8(dst);
            return false;
        }
        if (!data.empty()) out.write((const char*)data.data(), (std::streamsize)data.size());
    }
    WriteManifest(staging / kManifest, info);

    fs::remove_all(target, ec);
    fs::rename(staging, target, ec);
    if (ec) {
        // rename через границу файловых систем не работает — копируем.
        if (!CopyTree(staging, target, err)) {
            fs::remove_all(staging, ec);
            return false;
        }
        fs::remove_all(staging, ec);
    }
    LOG_INFO("Editor") << "Шаблон установлен: " << info.Name << " -> " << sage::PathToUtf8(target);
    return true;
}

bool InstallFromFolder(const fs::path& dir, const Manifest& info, std::string& err) {
    std::error_code ec;
    if (!fs::exists(dir / "project.sageproj", ec)) {
        err = "В папке нет project.sageproj — это не проект SAGE";
        return false;
    }
    Manifest m = info;
    m.Id = SafeId(m.Id.empty() ? sage::PathToUtf8(dir.filename()) : m.Id);
    if (m.Name.empty()) m.Name = m.Id;

    const fs::path target = Root() / sage::PathFromUtf8(m.Id);
    fs::remove_all(target, ec);
    if (!CopyTree(dir, target, err)) return false;
    WriteManifest(target / kManifest, m);
    LOG_INFO("Editor") << "Шаблон установлен из папки: " << m.Name;
    return true;
}

bool Uninstall(const std::string& id, std::string& err) {
    const std::string safe = SafeId(id);
    const fs::path target = Root() / sage::PathFromUtf8(safe);
    std::error_code ec;
    if (!fs::is_directory(target, ec)) {
        err = "Шаблон не установлен: " + id;
        return false;
    }
    // Удаляем только то, что действительно шаблон. Папка без project.sageproj
    // в templates/ — чужая, и стирать её по кнопке «удалить шаблон» нельзя.
    if (!fs::exists(target / "project.sageproj", ec)) {
        err = "В папке нет project.sageproj — не трогаю: " + sage::PathToUtf8(target);
        return false;
    }
    fs::remove_all(target, ec);
    if (ec) {
        err = ec.message();
        return false;
    }
    LOG_INFO("Editor") << "Шаблон удалён: " << safe;
    return true;
}

bool Pack(const fs::path& projectDir, const Manifest& info, const fs::path& outFile,
          std::string& err) {
    std::error_code ec;
    if (!fs::exists(projectDir / "project.sageproj", ec)) {
        err = "В папке нет project.sageproj — упаковывать нечего";
        return false;
    }
    sage::assets::PackWriter writer;
    // .meta не кладём: сайдкары редактора описывают ЛОКАЛЬНЫЙ импорт, и у того,
    // кто поставит шаблон, они всё равно перегенерируются.
    const size_t files = writer.AddDirectory(projectDir, {".meta", ".sagepak", ".sagetemplate"});
    if (files == 0) {
        err = "В проекте нет файлов";
        return false;
    }
    Manifest m = info;
    m.Id = SafeId(m.Id.empty() ? sage::PathToUtf8(projectDir.filename()) : m.Id);
    if (m.Name.empty()) m.Name = m.Id;
    json root;
    root["id"] = m.Id;
    root["name"] = m.Name;
    root["summary"] = m.Summary;
    root["note"] = m.Note;
    root["version"] = m.Version;
    const std::string text = root.dump(2) + "\n";
    writer.Add(kManifest, std::vector<uint8_t>(text.begin(), text.end()));

    if (outFile.has_parent_path()) fs::create_directories(outFile.parent_path(), ec);
    if (!writer.Save(outFile)) {
        err = "Не записать файл шаблона: " + sage::PathToUtf8(outFile);
        return false;
    }
    LOG_INFO("Editor") << "Шаблон упакован: " << sage::PathToUtf8(outFile) << " (" << writer.Count()
                       << " файлов)";
    return true;
}

std::string DefaultCatalogUrl() {
    return "https://github.com/AmckinatorStudios/SAGE-Engine/releases/download/windows-latest/"
           "templates.json";
}

std::string CatalogUrl() {
    return sage::editor::prefs::GetString("templateCatalog", DefaultCatalogUrl());
}

void SetCatalogUrl(const std::string& url) {
    sage::editor::prefs::SetString("templateCatalog", url);
}

bool ParseCatalog(const std::string& text, std::vector<Manifest>& out, std::string& err) {
    out.clear();
    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception& e) {
        err = std::string("Каталог не разобрать: ") + e.what();
        return false;
    }
    // Допускаем обе формы: массив и объект с полем templates. Каталог правят
    // руками, и требовать одну-единственную обёртку — лишний повод ошибиться.
    const json* list = nullptr;
    if (root.is_array()) list = &root;
    else if (root.is_object() && root.contains("templates") && root["templates"].is_array())
        list = &root["templates"];
    if (!list) {
        err = "В каталоге нет списка шаблонов";
        return false;
    }
    for (const json& e : *list) {
        if (!e.is_object()) continue;
        Manifest m;
        m.Id = e.value("id", std::string());
        if (m.Id.empty()) continue;   // без имени шаблон нельзя ни поставить, ни назвать
        m.Name = e.value("name", m.Id);
        m.Summary = e.value("summary", std::string());
        m.Note = e.value("note", std::string());
        m.Version = e.value("version", std::string());
        m.Url = e.value("url", std::string());
        m.Bytes = e.value("bytes", (uint64_t)0);
        out.push_back(m);
    }
    if (out.empty()) {
        err = "Каталог пуст";
        return false;
    }
    return true;
}

bool NetworkAvailable() {
    static const bool have = [] {
        std::string out;
#if defined(_WIN32)
        return RunCapture("curl --version", out);
#else
        return RunCapture("curl --version 2>/dev/null", out);
#endif
    }();
    return have;
}

bool HttpGet(const std::string& url, std::string& body, std::string& err) {
    if (!SafeUrl(url)) {
        err = "Недопустимый адрес: " + url;
        return false;
    }
    if (!NetworkAvailable()) {
        err = "Не найден curl — скачивание недоступно. Поставьте шаблон файлом .sagetemplate.";
        return false;
    }
    // -fsSL: молча, но с ненулевым кодом на ошибке HTTP и с переходами по
    // редиректам (у релизов GitHub ссылка всегда через редирект).
    const std::string cmd = "curl -fsSL --max-time 60 " + url;
    if (!RunCapture(cmd, body)) {
        err = "Не удалось получить " + url;
        return false;
    }
    return true;
}

bool HttpDownload(const std::string& url, const fs::path& to, std::string& err) {
    if (!SafeUrl(url)) {
        err = "Недопустимый адрес: " + url;
        return false;
    }
    if (!NetworkAvailable()) {
        err = "Не найден curl — скачивание недоступно. Поставьте шаблон файлом .sagetemplate.";
        return false;
    }
    std::error_code ec;
    if (to.has_parent_path()) fs::create_directories(to.parent_path(), ec);
    std::string out;
    const std::string cmd = "curl -fsSL --max-time 600 -o \"" + sage::PathToUtf8(to) + "\" " + url;
    if (!RunCapture(cmd, out)) {
        fs::remove(to, ec);
        err = "Не удалось скачать " + url;
        return false;
    }
    if (!fs::exists(to, ec) || fs::file_size(to, ec) == 0) {
        fs::remove(to, ec);
        err = "Скачанный файл пуст: " + url;
        return false;
    }
    return true;
}

bool FetchCatalog(std::vector<Manifest>& out, std::string& err) {
    std::string body;
    if (!HttpGet(CatalogUrl(), body, err)) return false;
    if (!ParseCatalog(body, out, err)) return false;
    const std::vector<Manifest> have = Installed();
    for (Manifest& m : out) {
        m.Installed = std::any_of(have.begin(), have.end(),
                                  [&m](const Manifest& h) { return h.Id == m.Id; });
    }
    return true;
}

bool Download(const Manifest& item, std::string& err) {
    if (item.Url.empty()) {
        err = "У шаблона «" + item.Name + "» не указана ссылка";
        return false;
    }
    std::error_code ec;
    const fs::path tmp = Root() / sage::PathFromUtf8("." + SafeId(item.Id) + ".download");
    fs::create_directories(Root(), ec);
    if (!HttpDownload(item.Url, tmp, err)) return false;
    const bool ok = InstallFromFile(tmp, err);
    fs::remove(tmp, ec);   // скачанный файл — не результат, результат в templates/
    return ok;
}

} // namespace sage::editor::templates
