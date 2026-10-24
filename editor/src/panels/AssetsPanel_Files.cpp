#include "../AssetOpen.h"
#include "../CreateAssetDialog.h"
#include "../DragSet.h"
#include "../PanelWindowId.h"
#include "../PanelWindows.h"
#include "AssetsPanel.h"
#include "FsCache.h"
#include "ModelImportDialog.h"
#include "../Progress.h"

#include <chrono>
#include <future>
#include "../FolderColors.h"
#include "ui/UI.h"
#include "EditorTheme.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "imgui.h"
#include "imgui_internal.h" // BeginDragDropTargetCustom: объект на всё окно сетки

#include "EditorHost.h"
#include "../AssetSlot.h"
#include "../EditorIcons.h"
#include "../HotkeyScope.h"
#include "../SceneCover.h"
#include "../Thumbnails.h"
#include "../AssetCovers.h"

#include "sage/assets/import/Convert.h"
#include "sage/render/ResourceManager.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/assets/Zip.h"
#include "Project.h"
#include "sage/core/Log.h"
#include "sage/ecs/LightSystem.h"
#include "sage/scene/Components.h"
#include "sage/scene/SceneSerializer.h"
#include "../Localization.h"

#include "AssetsPanelDetail.h"

namespace fs = std::filesystem;



namespace covers = sage::editor::covers;
using namespace sage::editor::assetspanel_detail;

// Файловые операции панели: перенос, переименование, удаление, внесение
// в проект, создание ассетов. Без интерфейса — их зовут и панель, и самопроверка.
namespace sage::editor::assetspanel_detail {
// Файлы, на которые ссылается .mtl (map_Kd, norm, bump, …). Разбираем сами, а
// не через tinyobj: нужен СПИСОК ИМЁН, а не разобранный материал, и делать ради
// него полный разбор геометрии — платить кратно больше, чем стоит задача.
std::vector<std::string> MtlTextureNames(const fs::path& mtl) {
    std::vector<std::string> names;
    std::ifstream in(mtl);
    if (!in) return names;
    std::string line;
    while (std::getline(in, line)) {
        // Ключевые слова карт: всё, что начинается на map_, плюс bump/norm/disp/refl.
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        const bool isMap = key.rfind("map_", 0) == 0 || key == "bump" || key == "norm" ||
                           key == "disp" || key == "refl";
        if (!isMap) continue;
        // У карты бывают опции («map_Kd -bm 0.2 stone.png») — имя файла это
        // ПОСЛЕДНИЙ токен строки, а не первый после ключевого слова.
        std::string token, last;
        while (ls >> token) last = token;
        if (!last.empty() && last[0] != '-') names.push_back(last);
    }
    return names;
}

// Внешние файлы, на которые ссылается .gltf: uri у buffers[] и images[].
// Разбираем как JSON — .gltf это он и есть. Разбор может не удаться (битый
// файл): тогда спутников просто не будет, а сам файл всё равно переедет.
std::vector<std::string> GltfExternalUris(const fs::path& gltf) {
    std::vector<std::string> uris;
    std::ifstream in(gltf);
    if (!in) return uris;
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        return uris;
    }
    for (const char* section : {"buffers", "images"}) {
        if (!j.contains(section) || !j[section].is_array()) continue;
        for (const auto& entry : j[section]) {
            const std::string uri = entry.value("uri", std::string{});
            // data:… — содержимое лежит внутри самого файла, копировать нечего.
            if (uri.empty() || uri.rfind("data:", 0) == 0) continue;
            uris.push_back(uri);
        }
    }
    return uris;
}

// Регистрация в базе ассетов ждёт путь ОТНОСИТЕЛЬНО корня проекта: по нему она
// кладёт сайдкар .meta и по нему же потом отвечает на «где этот файл». Отдать
// ей абсолютный путь значит записать ассет под ключом, которого в проекте нет.
void RegisterInDatabase(const fs::path& file) {
    sage::AssetDatabase& db = sage::AssetDatabase::Instance();
    const std::string root = db.ProjectDir();
    if (root.empty()) return;   // проект не открыт — регистрировать некуда
    std::error_code ec;
    const fs::path rel = fs::relative(file, fs::path(root), ec);
    if (ec || rel.empty()) return;
    db.Register(rel.generic_string());
}
} // namespace sage::editor::assetspanel_detail

// Перенос файла в папку броском. Вместе с ним едет сайдкар .meta (личность
// ассета — по нему ссылки в сценах остаются целыми) и .sageimport (настройки
// импорта модели): оставить их на старом месте значило бы, что переехавшая
// модель потеряла и свой GUID, и свой масштаб.
const char* AssetsPanel::FolderIcon(const fs::path& dir) {
    // Через кэш листингов (FsCache.h): значок папки рисуется в каждой строке
    // дерева и на каждой плитке В КАЖДОМ КАДРЕ, и листать для него диск каждый
    // раз значило платить сотнями системных вызовов за кадр. Сайдкары .meta
    // кэш уже отбросил: без своего файла они не остаются.
    const sage::editor::fscache::Listing& l = sage::editor::fscache::List(dir, 0.5);
    return l.Dirs.empty() && l.Files.empty() ? "folder" : "folder-full";
}


bool AssetsPanel::MoveIntoFolder(EditorHost& host, const fs::path& source, const fs::path& folder,
                                 bool rescan) {
    std::error_code ec;
    if (!fs::exists(source, ec)) return false;
    // ПАПКА ПЕРЕЕЗЖАЕТ ТОЖЕ — кроме как в саму себя или в свою же вложенную:
    // такой перенос ОС либо запретит, либо (на части систем) унесёт папку в
    // никуда. Раньше папки не переносились вовсе, и разложить проект по
    // местам можно было только в проводнике, мимо редактора.
    if (fs::is_directory(source, ec)) {
        const fs::path src = fs::weakly_canonical(source, ec);
        for (fs::path up = fs::weakly_canonical(folder, ec); !up.empty(); up = up.parent_path()) {
            if (up == src) {
                host.SetStatusMessage(T("A folder cannot be moved into itself"));
                return false;
            }
            if (up == up.parent_path()) break;
        }
    }
    const fs::path target = folder / source.filename();
    if (fs::weakly_canonical(source, ec) == fs::weakly_canonical(target, ec)) return false;
    if (fs::exists(target, ec)) {
        host.SetStatusMessage(T("The folder already contains ") + source.filename().string());
        return false;
    }

    fs::rename(source, target, ec);
    if (ec) {
        host.SetStatusMessage(T("Moving failed: ") + ec.message());
        LOG_ERROR("Editor") << "Перенос ассета не удался: " << ec.message();
        return false;
    }
    for (const char* sidecar : {".meta", ".sageimport"}) {
        const fs::path from = source.string() + sidecar;
        std::error_code sec;
        if (fs::exists(from, sec)) fs::rename(from, target.string() + sidecar, sec);
    }

    if (m_selected == source) m_selected = target;
    for (fs::path& p : m_multi) if (p == source) p = target;
    // Метка едет с папкой: иначе раскладка рассыпалась бы от первого переезда.
    sage::editor::foldercolors::Rename(source, target);
    // Пересканировать проект: база ассетов помнит пути, и после переезда её
    // ответ на «где этот файл» обязан измениться.
    if (!rescan) return true;
    sage::AssetDatabase::Instance().ScanProject(host.CurrentProject().Dir().string());
    host.SetStatusMessage(T("Moved into ") + folder.filename().string() + ": " +
                          source.filename().string());
    return true;
}


void AssetsPanel::MoveGroupIntoFolder(EditorHost& host, const std::vector<std::string>& sources,
                                      const fs::path& folder) {
    if (sources.size() == 1) {
        MoveIntoFolder(host, sources.front(), folder);
        return;
    }
    // Один пересчёт базы на всю группу, а не на каждый файл: пересканировать
    // проект сорок раз подряд значит заморозить редактор на сорок сканов.
    int moved = 0;
    for (const std::string& src : sources)
        if (fs::path(src) != folder && MoveIntoFolder(host, src, folder, false)) ++moved;
    if (moved == 0) return;
    sage::AssetDatabase::Instance().ScanProject(host.CurrentProject().Dir().string());
    char text[256];
    std::snprintf(text, sizeof(text), T("Moved %d items into %s"), moved, folder.filename().string().c_str());
    host.SetStatusMessage(text);
}


// Переименование ассета вместе с сайдкарами. Сайдкар .meta — это личность
// файла: в нём лежит GUID, по которому сцены его и находят. Оставить .meta под
// старым именем значит превратить переименование в «удалили один ассет,
// создали другой»: все ссылки на него в сценах становятся битыми, а рядом
// навсегда поселяется запись про файл, которого нет. В панели такая запись
// теперь даже не видна — сайдкары в сетке скрыты.
bool AssetsPanel::RenameAsset(const fs::path& path, const std::string& newName,
                              fs::path& outRenamed, std::string& err) {
    if (newName.empty()) {
        err = T("The name cannot be empty");
        return false;
    }
    const fs::path target = path.parent_path() / newName;
    std::error_code ec;
    if (fs::exists(target, ec)) {
        err = T("A file with this name already exists");
        return false;
    }
    fs::rename(path, target, ec);
    if (ec) {
        err = "Rename failed: " + ec.message();
        return false;
    }
    for (const char* sidecar : {".meta", ".sageimport"}) {
        const fs::path from = path.string() + sidecar;
        std::error_code sec;
        if (fs::exists(from, sec)) fs::rename(from, target.string() + sidecar, sec);
    }
    outRenamed = target;
    return true;
}


// Удаление ассета вместе с сайдкарами: иначе от него остаётся невидимая запись
// .meta, которую база ассетов честно считает существующим ассетом.
void AssetsPanel::DeleteAsset(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    if (ec) LOG_ERROR("Editor") << "Asset delete failed: " << ec.message();
    for (const char* sidecar : {".meta", ".sageimport"}) {
        std::error_code sec;
        fs::remove(path.string() + sidecar, sec);
    }
}


// Копирование БЕЗ базы ассетов (см. AssetsPanel.h): всё, что здесь делается, —
// работа файловой системы, и её ведёт фоновый поток.
AssetsPanel::ImportReport AssetsPanel::CopyIntoProject(
    const fs::path& source, const fs::path& destDir,
    const std::function<void(float, const std::string&)>& tell) {
    ImportReport report;
    std::error_code ec;
    auto say = [&](float fraction, const std::string& what) {
        if (tell) tell(fraction, what);
    };

    if (!fs::exists(source, ec)) {
        report.Error = T("File not found: ") + source.string();
        return report;
    }
    fs::create_directories(destDir, ec);
    if (ec) {
        report.Error = T("The folder is not accessible: ") + ec.message();
        return report;
    }

    // --- ПАПКА ЦЕЛИКОМ -------------------------------------------------------
    //
    // Скачанный ассет — это чаще всего папка: модель, набор карт, лицензия,
    // превью. Вносить её по файлу значит десяток раз открыть диалог и один раз
    // ошибиться, а разбор спутников (.mtl, .bin, текстуры) тут и не нужен —
    // внутри и так уже всё, что нужно модели.
    if (fs::is_directory(source, ec)) {
        const fs::path target = destDir / source.filename();
        // ПО ФАЙЛУ, А НЕ ОДНИМ fs::copy: рекурсивное копирование не умеет
        // рассказывать, где оно сейчас, а папка ассетов — это сотни файлов, и
        // «идёт копирование, 214 из 680, wood_normal.png» это и есть разница
        // между ожиданием и зависанием.
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(source, ec)) {
            if (entry.is_directory(ec)) continue;
            files.push_back(entry.path());
        }
        fs::create_directories(target, ec);
        size_t done = 0;
        for (const fs::path& from : files) {
            const fs::path rel = fs::relative(from, source, ec);
            const fs::path to = target / rel;
            std::error_code cec;
            fs::create_directories(to.parent_path(), cec);
            // Существующий файл не трогаем — тем же правилом, что и ниже:
            // повторное внесение не должно затирать правку.
            if (!fs::exists(to, cec)) fs::copy_file(from, to, cec);
            if (cec) {
                report.Missing.push_back(rel.generic_string());
            } else {
                report.Extra.push_back(to);
            }
            ++done;
            say(files.empty() ? -1.0f : (float)done / (float)files.size(),
                rel.filename().string());
        }
        report.Ok = true;
        report.Created = target;
        return report;
    }

    // --- АРХИВ ---------------------------------------------------------------
    //
    // Определяется ПО СОДЕРЖИМОМУ, а не по расширению: тот же zip приезжает и
    // как .zip, и без расширения вовсе. Распаковывается в свою папку, а не
    // вперемешку с тем, что уже лежит рядом: в архиве часто десятки файлов, и
    // высыпать их в открытую папку проекта значит превратить её в свалку,
    // которую потом разбирать руками.
    if (sage::assets::IsZip(source)) {
        const fs::path target = destDir / source.stem();
        std::string zipErr;
        std::vector<std::string> skipped;
        // Доля неизвестна: распаковка идёт одним вызовом и о ходе дела не
        // сообщает. Полоса при этом бежит, а не стоит на нуле (см. Progress.h).
        say(-1.0f, source.filename().string());
        const int written = sage::assets::ExtractZip(source, target, zipErr, &skipped);
        if (written < 0) {
            report.Error = T("The archive could not be unpacked: ") + zipErr;
            return report;
        }
        for (const auto& entry : fs::recursive_directory_iterator(target, ec)) {
            if (entry.is_directory(ec)) continue;
            report.Extra.push_back(entry.path());
        }
        report.Missing = skipped;
        report.Ok = true;
        report.Created = target;
        return report;
    }

    // Уже в проекте — вносить нечего. Это не ошибка: человек мог выбрать файл в
    // папке проекта просто потому, что диалог там и открылся.
    const fs::path canonicalSrc = fs::weakly_canonical(source, ec);
    const fs::path canonicalDst = fs::weakly_canonical(destDir / source.filename(), ec);
    if (!ec && canonicalSrc == canonicalDst) {
        report.Ok = true;
        report.Created = canonicalDst;
        return report;
    }

    // Спутники ищем ДО копирования: если модель ссылается на файлы, которых нет,
    // сказать об этом надо про исходную папку, а не про проект.
    const fs::path srcDir = source.parent_path();
    std::string ext = ToLower(source.extension().string());
    std::vector<std::string> companions;
    if (ext == ".obj") {
        // .mtl обычно называется как модель, но в файле может стоять и другое
        // имя — читаем mtllib, а одноимённый добавляем на всякий случай.
        std::ifstream obj(source);
        std::string line;
        while (std::getline(obj, line)) {
            std::istringstream ls(line);
            std::string key, name;
            ls >> key;
            if (key != "mtllib") continue;
            while (ls >> name) companions.push_back(name);
        }
        companions.push_back(source.stem().string() + ".mtl");
    } else if (ext == ".gltf") {
        companions = GltfExternalUris(source);
    }

    // Картинки, на которые ссылаются найденные .mtl — второй уровень.
    std::vector<std::string> nested;
    for (const std::string& c : companions) {
        if (ToLower(fs::path(c).extension().string()) != ".mtl") continue;
        for (const std::string& tex : MtlTextureNames(srcDir / c)) nested.push_back(tex);
    }
    companions.insert(companions.end(), nested.begin(), nested.end());

    auto copyOne = [&](const fs::path& from, const fs::path& to) -> bool {
        std::error_code cec;
        fs::create_directories(to.parent_path(), cec);
        // Существующий файл НЕ перезаписываем: повторный импорт не должен
        // затирать текстуру, которую после первого раза поправили.
        if (fs::exists(to, cec)) return true;
        fs::copy_file(from, to, cec);
        return !cec;
    };

    const fs::path mainDst = destDir / source.filename();
    if (!copyOne(source, mainDst)) {
        report.Error = T("Could not copy: ") + source.filename().string();
        return report;
    }
    report.Created = mainDst;
    say(companions.empty() ? 1.0f : 0.0f, source.filename().string());

    size_t doneCompanions = 0;
    for (const std::string& rel : companions) {
        // Путь спутника относительный — сохраняем его форму, иначе .gltf,
        // ссылающийся на «textures/wood.png», после импорта не нашёл бы файл.
        const fs::path from = srcDir / rel;
        if (!fs::exists(from, ec) || fs::is_directory(from, ec)) {
            report.Missing.push_back(rel);
            continue;
        }
        const fs::path to = destDir / rel;
        if (copyOne(from, to)) {
            report.Extra.push_back(to);
        } else {
            report.Missing.push_back(rel);
        }
        ++doneCompanions;
        say((float)doneCompanions / (float)companions.size(), fs::path(rel).filename().string());
    }

    report.Ok = true;
    return report;
}


// Регистрация внесённого в базе ассетов — ТОЛЬКО главный поток (см. заголовок).
void AssetsPanel::RegisterImported(const ImportReport& report) {
    if (!report.Ok) return;
    if (!report.Created.empty() && !fs::is_directory(report.Created)) RegisterInDatabase(report.Created);
    for (const fs::path& file : report.Extra) RegisterInDatabase(file);
}


// Прежний однокадровый путь: копирование плюс регистрация. Им пользуются те,
// у кого кадра нет вовсе, — self-test и сценарии без интерфейса.
AssetsPanel::ImportReport AssetsPanel::ImportAsset(const fs::path& source,
                                                   const fs::path& destDir) {
    ImportReport report = CopyIntoProject(source, destDir);
    RegisterImported(report);
    return report;
}


bool AssetsPanel::CreateAsset(CreateKind kind, const std::string& rawName, const fs::path& dir,
                              fs::path& outCreated, std::string& err) {
    std::string name = rawName;
    if (name.empty()) {
        err = "Name must not be empty";
        return false;
    }

    const char* wantExt = nullptr;
    switch (kind) {
        case CreateKind::Script:   wantExt = ".lua"; break;
        case CreateKind::SageScript: wantExt = ".slua"; break;
        case CreateKind::Scene:    wantExt = ".sage"; break;
        case CreateKind::TextFile: wantExt = ".txt"; break;
        case CreateKind::Material: wantExt = ".sagemat"; break;
        default: break;
    }
    if (wantExt && fs::path(name).extension() != wantExt) name += wantExt;

    fs::path target = dir / name;
    std::error_code ec;
    if (fs::exists(target, ec)) {
        err = "Already exists: " + name;
        return false;
    }

    if (kind == CreateKind::Folder) {
        if (!fs::create_directory(target, ec) || ec) {
            err = "Create folder failed: " + ec.message();
            return false;
        }
    } else {
        std::string content;
        if (kind == CreateKind::Script) {
            content =
                "-- " + name + " — скрипт сущности SAGE.\n"
                "-- OnStart(entity) вызывается один раз при привязке скрипта,\n"
                "-- OnUpdate(entity, dt) — каждый кадр Play-режима/игры.\n"
                "\n"
                "-- ПУБЛИЧНЫЕ ПЕРЕМЕННЫЕ — настройка объекта, видна и правится в инспекторе,\n"
                "-- своя у каждого объекта с этим скриптом (см. docs/scripting.md):\n"
                "-- Vars = {\n"
                "--     speed = 3.0,\n"
                "--     target = { kind = \"entity\", label = \"Цель\" },\n"
                "-- }\n"
                "-- Читаются так: entity:Vars().speed (или self:Vars().speed).\n"
                "\n"
                "function OnStart(entity)\n"
                "end\n"
                "\n"
                "function OnUpdate(entity, dt)\n"
                "    -- entity.Transform.Rotation.y = entity.Transform.Rotation.y + dt * 45.0\n"
                "end\n";
        } else if (kind == CreateKind::Scene) {
            // Новая сцена — сразу рабочая: солнце и камера. Без них открытая
            // сцена — чёрный кадр (свет мира сам собой не горит), а Play —
            // «нет камеры», и непонятно, что сделано не так.
            Scene scene(fs::path(name).stem().string());
            sage::ecs::CreateSunEntity(scene);
            GameObject cam = scene.CreateEmptyObject("Main Camera");
            cam.GetTransform().Position = glm::vec3(0.0f, 2.0f, 6.0f);
            cam.GetTransform().Rotation = glm::vec3(-12.0f, 0.0f, 0.0f);
            scene.Registry().emplace<CameraComponent>(cam.Entity());
            content = SceneSerializer::SaveToString(scene);
        } else if (kind == CreateKind::SageScript) {
            // Имя класса — из имени файла: главный компонент файла тот, чьё
            // имя совпадает с файлом (иначе — последний объявленный).
            std::string cls;
            bool upper = true;
            for (char c : fs::path(name).stem().string()) {
                if (!std::isalnum((unsigned char)c)) { upper = true; continue; }
                cls += upper ? (char)std::toupper((unsigned char)c) : c;
                upper = false;
            }
            if (cls.empty() || std::isdigit((unsigned char)cls[0])) cls = "Behaviour" + cls;
            content =
                "-- " + name + " — компонент на SAGE Lua (см. docs/sage_lua.md).\n"
                "\n"
                "class " + cls + " extends Component\n"
                "    @field(editable = true, name = \"Скорость\", range = { min = 0, max = 20, step = 0.1 })\n"
                "    public speed: number = 5\n"
                "\n"
                "    function onStart()\n"
                "    end\n"
                "\n"
                "    function onUpdate(dt: number)\n"
                "    end\n"
                "end\n";
        } else if (kind == CreateKind::Material) {
            content =
                "{\n"
                "    \"albedo\": [1.0, 1.0, 1.0],\n"
                "    \"emissive\": [0.0, 0.0, 0.0],\n"
                "    \"metallic\": 0.0,\n"
                "    \"roughness\": 0.5,\n"
                "    \"cull\": \"back\",\n"
                "    \"texture\": \"\"\n"
                "}\n";
        }
        std::ofstream out(target);
        if (!out) {
            err = "Create file failed: " + target.string();
            return false;
        }
        out << content;
    }

    LOG_INFO("Editor") << "Asset created: " << target.string();
    outCreated = target;
    return true;
}
