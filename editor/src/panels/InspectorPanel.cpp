#include "InspectorPanel.h"

#include <cmath>
#include <cstdio>
#include <system_error>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "sage/core/Log.h"
#include <algorithm>

#include "EditorIcons.h"
#include "Project.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ModelLoader.h"
#include "sage/render/ModelMaterial.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/render/ParticlePresets.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "sage/ui/UIIcons.h"

namespace fs = std::filesystem;

// Редактор материала: правит поля РАЗДЕЛЯЕМОГО экземпляра из кэша
// ResourceManager — все сущности с этим материалом обновляются в вьюпорте
// сразу; Save фиксирует значения на диск, Revert перечитывает файл.
// Слот текстуры: превью + путь + «Обзор…» + «Из Assets» + «Очистить».
void InspectorPanel::DrawTextureSlot(EditorHost& host, const char* label, std::string& path,
                                     const std::shared_ptr<Texture>& tex, const char* tooltip) {
    ImGui::PushID(label);

    // Превью — квадрат 48x48. Пустой слот рисуется рамкой, а не пустотой: иначе
    // «текстура не назначена» и «текстура назначена, но не загрузилась»
    // выглядят одинаково, а это разные беды с разным лечением.
    // Превью — КНОПКА, а не картинка: клик по нему открывает выбор файла. Это
    // первое, во что человек тычет, когда хочет сменить текстуру, и раньше он
    // не делал ничего.
    const ImVec2 size(64, 64);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##preview", size);
    const bool previewClicked = ImGui::IsItemClicked();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);

    // Шахматка: без неё прозрачные места картинки сливаются с фоном панели, и
    // текстура с альфой выглядит просто дырявой.
    const float checker = 8.0f;
    dl->PushClipRect(p0, p1, true);
    for (float y = p0.y; y < p1.y; y += checker)
        for (float x = p0.x; x < p1.x; x += checker) {
            const bool odd = ((int)((x - p0.x) / checker) + (int)((y - p0.y) / checker)) % 2;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + checker, y + checker),
                              odd ? IM_COL32(68, 68, 74, 255) : IM_COL32(50, 50, 56, 255));
        }
    dl->PopClipRect();

    if (tex) {
        dl->AddImage((ImTextureID)(std::intptr_t)tex->NativeHandle(), p0, p1, ImVec2(0, 1),
                     ImVec2(1, 0));
        dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 40), 4.0f);
    } else {
        // Пустой слот и НЕЗАГРУЗИВШИЙСЯ — разные беды с разным лечением, поэтому
        // и выглядят по-разному: тусклая рамка против красной с крестом.
        const ImU32 col = path.empty() ? IM_COL32(110, 110, 120, 160) : IM_COL32(210, 90, 90, 220);
        dl->AddRect(p0, p1, col, 4.0f, 0, 1.5f);
        if (path.empty()) {
            const char* plus = "+";
            const ImVec2 ts = ImGui::CalcTextSize(plus);
            dl->AddText(ImVec2(p0.x + (size.x - ts.x) * 0.5f, p0.y + (size.y - ts.y) * 0.5f), col,
                        plus);
        } else {
            dl->AddLine(ImVec2(p0.x + 18, p0.y + 18), ImVec2(p1.x - 18, p1.y - 18), col, 2.0f);
            dl->AddLine(ImVec2(p1.x - 18, p0.y + 18), ImVec2(p0.x + 18, p1.y - 18), col, 2.0f);
        }
    }
    // Приём перетаскивания из панели Assets — самый короткий путь назначить
    // текстуру, когда она уже найдена в дереве проекта.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ASSET_PATH")) {
            std::string dropped((const char*)p->Data, (size_t)p->DataSize);
            if (!dropped.empty() && dropped.back() == '\0') dropped.pop_back();
            // Относительно проекта: абсолютный путь текстуры живёт ровно до
            // сборки игры (см. Project::AssetRef).
            path = host.CurrentProject().AssetRef(dropped);
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(label);
        if (tooltip) ImGui::TextDisabled("%s", tooltip);
        if (path.empty()) ImGui::TextDisabled("Не назначена");
        else if (!tex) ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "Не загрузилась: %s",
                                          path.c_str());
        else ImGui::TextDisabled("%s", path.c_str());
        ImGui::EndTooltip();
    }

    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted(label);

    // Полный путь в узком поле нечитаем: у «C:/Users/.../Pictures/frame1.png»
    // видно ровно начало, то есть самую бесполезную часть. Показываем имя
    // файла, а весь путь — в подсказке и в поле, когда его правят.
    if (!path.empty()) {
        const std::string name = std::filesystem::path(path).filename().string();
        ImGui::TextDisabled("%s", name.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
    } else {
        ImGui::TextDisabled("не назначена");
    }

    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", path.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##path", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        path = buf;
        ResourceManager::Instance().ResolveMaterialTextures(
            *ResourceManager::Instance().GetMaterial(host.SelectedAssetPath().string()));
    }

    if (EditorIcons::Button("folder", "Обзор…") || previewClicked) {
        FileBrowser::Config c;
        c.Title = std::string("Текстура: ") + label;
        c.Filters = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr"};
        c.FilterLabel = "Изображения";
        if (host.CurrentProject().Loaded()) c.StartDir = host.CurrentProject().AssetsDir();
        m_browser.Open(c);
        m_browseTarget = &path;
        m_browseIsShader = false;
    }
    // «Из Assets» — то, что выбрано в панели ассетов: самый быстрый путь, когда
    // текстура уже найдена там.
    const std::string selExt = host.SelectedAssetPath().extension().string();
    const bool selIsImage = selExt == ".png" || selExt == ".jpg" || selExt == ".jpeg" ||
                            selExt == ".tga" || selExt == ".bmp" || selExt == ".hdr";
    ImGui::SameLine();
    ImGui::BeginDisabled(!selIsImage);
    if (EditorIcons::Button("texture", "Из Assets")) {
        path = host.CurrentProject().AssetRef(host.SelectedAssetPath());
    }
    ImGui::EndDisabled();
    if (!selIsImage && ImGui::IsItemHovered())
        ImGui::SetTooltip("Выберите изображение в панели Assets");

    ImGui::SameLine();
    ImGui::BeginDisabled(path.empty());
    if (EditorIcons::Button("trash", "Очистить")) path.clear();
    ImGui::EndDisabled();

    ImGui::EndGroup();
    ImGui::PopID();
    ImGui::Spacing();
}

void InspectorPanel::DrawMaterialEditor(EditorHost& host) {
    std::string pathStr = host.SelectedAssetPath().string();
    std::shared_ptr<Material> material = ResourceManager::Instance().GetMaterial(pathStr);

    // Файл мог не прочитаться (удалён, битый JSON) — тогда весь редактор ниже
    // разыменовывал бы nullptr. Выбор испорченного .sagemat в Assets ронял
    // редактор: превью-то от null защищалось, а первое же поле — нет.
    if (!material) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "Материал не читается");
        ImGui::TextDisabled("Файл удалён или повреждён — подробности в консоли.");
        return;
    }

    // --- 3D-превью ------------------------------------------------------------
    //
    // Раньше материал показывался КВАДРАТИКОМ ЦВЕТА albedo. По нему нельзя
    // понять ничего из того, ради чего материал и настраивают: металл или
    // диэлектрик, гладкий или матовый, как легла карта нормалей. Всё это видно
    // только на изогнутой поверхности при свете.
    if (material) {
        const float side = std::min(ImGui::GetContentRegionAvail().x, 220.0f);
        const uint64_t tex = m_preview.RenderMaterial(material, (int)side);
        if (tex) {
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)(std::intptr_t)tex, ImVec2(side, side), ImVec2(0, 1),
                         ImVec2(1, 0));
            // Вращение мышью: блик и шероховатость читаются только в движении —
            // на неподвижной картинке гладкое и почти гладкое неотличимы.
            if (ImGui::IsItemHovered()) {
                ImGuiIO& io = ImGui::GetIO();
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    m_preview.Orbit(io.MouseDelta.x * 0.5f, -io.MouseDelta.y * 0.5f);
                }
                if (io.MouseWheel != 0.0f) m_preview.Zoom(io.MouseWheel);
                ImGui::SetTooltip("ЛКМ — вращать, колесо — приблизить");
            }
            (void)p0;
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextDisabled("Превью");
        if (ImGui::SmallButton("Сбросить вид")) m_preview.ResetView();
        ImGui::EndGroup();
        ImGui::Spacing();
    }

    ImGui::ColorEdit3("Albedo", &material->Albedo.x);
    ImGui::ColorEdit3("Emissive", &material->Emissive.x);
    // Сила свечения отдельным ползунком, и его предел заметно больше единицы:
    // bloom срабатывает от яркости ВЫШЕ 1 (EngineConfig::BloomThreshold), а цвет
    // в редакторе зажат в 0..1. Без множителя «свечение» оставалось бы просто
    // светлым цветом без ореола.
    ImGui::DragFloat("Emissive Strength", &material->EmissiveStrength, 0.05f, 0.0f, 20.0f, "%.2f");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Множитель свечения. Значения больше 1 дают ореол (bloom).");
    }

    // PBR (metallic-roughness) — основной путь освещения (Cook-Torrance).
    ImGui::SeparatorText("PBR");
    ImGui::SliderFloat("Metallic", &material->Metallic, 0.0f, 1.0f);
    ImGui::SliderFloat("Roughness", &material->Roughness, 0.0f, 1.0f);

    // Карты: путь правится вручную; по Enter/потере фокуса перезагружаем текстуры
    // материала (albedo/normal), чтобы вьюпорт сразу показал результат.
        // Слоты текстур: превью, путь, «Обзор…», «Из Assets», «Очистить».
    //
    // Раньше здесь стояли пять голых полей ввода, применявшихся по Enter: путь
    // к текстуре надо было ЗНАТЬ и напечатать без опечатки, а единственной
    // обратной связью была строчка в консоли. Слот показывает саму картинку —
    // назначено ли что-то и то ли это, что хотели, видно сразу.
    DrawTextureSlot(host, "Albedo", material->TexturePath, material->AlbedoTex,
                    "Базовый цвет. Умножается на Albedo выше.");
    DrawTextureSlot(host, "Normal", material->NormalMapPath, material->NormalTex,
                    "Карта нормалей, tangent-space (OpenGL: зелёный вверх).");
    DrawTextureSlot(host, "Metallic", material->MetallicMapPath, material->MetallicTex,
                    "Канал R. Умножается на фактор Metallic выше.");
    DrawTextureSlot(host, "Roughness", material->RoughnessMapPath, material->RoughnessTex,
                    "Канал R. Умножается на фактор Roughness выше.");
    DrawTextureSlot(host, "Emissive", material->EmissiveMap, material->EmissiveTex,
                    "Где светится, а где нет. Умножается на цвет и силу свечения.");
    DrawTextureSlot(host, "AO", material->AOMapPath, material->AOTex,
                    "Ambient occlusion, канал R. Пусто — AO = 1.");

    ImGui::TextDisabled("Normal: tangent-space (OpenGL). Metallic/Rough/AO use R channel.");
    ImGui::TextDisabled("Map value multiplies the factor above; Enter applies the path.");

    ImGui::SeparatorText("Transparency");
    ImGui::SliderFloat("Opacity", &material->Opacity, 0.0f, 1.0f);

    // Свой шейдер материала: пара .vert/.frag (см. docs/custom_shaders.md).
    // Правка файлов подхватывается на лету — ReloadChangedShaders в EditorLayer.
    ImGui::SeparatorText("Custom Shader");
    char vsBuf[512];
    std::snprintf(vsBuf, sizeof(vsBuf), "%s", material->VertexShaderPath.c_str());
    if (ImGui::InputText("Vertex", vsBuf, sizeof(vsBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        material->VertexShaderPath = vsBuf;
        material->ShaderPtr.reset();
    }
    char fsBuf[512];
    std::snprintf(fsBuf, sizeof(fsBuf), "%s", material->FragmentShaderPath.c_str());
    if (ImGui::InputText("Fragment", fsBuf, sizeof(fsBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        material->FragmentShaderPath = fsBuf;
        material->ShaderPtr.reset();
    }
    if (material->HasCustomShader() && !material->Params.empty()) {
        ImGui::TextDisabled("Params: %d (edit in the .sagemat file)", (int)material->Params.size());
    }

    // Свойства рендера рисуются ПО ТАБЛИЦЕ (MaterialRenderFields). Новая
    // возможность рендера появляется в инспекторе сама — от неё требуется поле
    // и строка таблицы, а не ещё одна правка здесь. Раньше каждое такое
    // свойство надо было дописать в пяти местах, и панель редактора отставала
    // от формата файла чаще всего: её забывали.
    ImGui::SeparatorText("Рендер");
    for (const MaterialRenderField& f : MaterialRenderFields()) {
        if (f.Type == MaterialRenderField::Kind::Bool && f.AsBool) {
            ImGui::Checkbox(f.Label, &(material->Render.*f.AsBool));
        } else if (f.AsFloat) {
            ImGui::SliderFloat(f.Label, &(material->Render.*f.AsFloat), f.Min, f.Max);
        }
        if (f.Tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.Tooltip);
    }

    ImGui::SeparatorText("Legacy");
    ImGui::DragFloat("Shininess", &material->Shininess, 0.5f, 1.0f, 256.0f);
    ImGui::TextDisabled("Edits apply live to every entity using this material.");

    if (ImGui::Button("Save Material")) {
        try {
            material->SaveToFile(pathStr);
            LOG_INFO("Editor") << "Material saved: " << pathStr;
        } catch (const std::exception& e) {
            LOG_ERROR("Editor") << "Material save failed: " << e.what();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) ResourceManager::Instance().ReloadMaterial(pathStr);
}

// Настройки импорта выбранной модели (.obj/.gltf/.glb): масштаб/центрирование/
// нормализация в сайдкар .sageimport. Reimport перечитывает меш и обновляет все
// сущности сцены, использующие эту модель.
void InspectorPanel::DrawModelImportEditor(EditorHost& host) {
    std::string path = host.SelectedAssetPath().string();
    ModelLoader::ImportSettings s = ModelLoader::LoadImportSettings(path);
    ImGui::DragFloat("Import Scale", &s.Scale, 0.01f, 0.001f, 1000.0f);
    ImGui::Checkbox("Recenter (AABB -> origin)", &s.Recenter);
    ImGui::Checkbox("Normalize size (max side = 1)", &s.NormalizeSize);

    if (ImGui::Button("Reimport")) {
        if (!ModelLoader::SaveImportSettings(path, s)) {
            host.SetStatusMessage("Import settings save failed");
        } else {
            // Перечитываем меш и переназначаем всем сущностям с этой моделью.
            std::shared_ptr<Mesh> mesh = ResourceManager::Instance().ReloadModel(path);
            Scene& scene = host.CurrentScene();
            auto view = scene.Registry().view<MeshRendererComponent>();
            for (auto e : view) {
                MeshRendererComponent& mr = view.get<MeshRendererComponent>(e);
                if (mr.Ref.type == MeshRef::Type::Model && mr.Ref.path == path) mr.MeshPtr = mesh;
            }
            host.SetStatusMessage("Reimported: " + host.SelectedAssetPath().filename().string());
        }
    }
    ImGui::TextDisabled("Baked into the mesh on load — affects editor and built game");
}

// Материал модели — вместе с моделью.
//
// ЗАЧЕМ. Файл .gltf/.glb/.obj несёт не только треугольники: там лежит материал
// с albedo, металличностью, шероховатостью и картами — всё то, чем движок и так
// умеет рисовать. До сих пор всё это выбрасывалось: в сцену приезжала белая
// болванка, а привести её в тот вид, в котором её экспортировали, значило
// вручную завести .sagemat и вручную прописать шесть путей к картинкам, ещё и
// зная, что glTF пакует металличность и шероховатость в разные каналы одной
// текстуры. Это и есть «модели не так развиты, как PBR-текстуры»: возможности
// рендера были на месте, а дороги от файла модели до них не было.
//
// ЧТО ИМЕННО ДЕЛАЕТСЯ. Рядом с моделью появляется <имя>.sagemat, если его там
// ещё нет, и назначается сущности. Уже существующий файл НЕ перезаписывается:
// материал после импорта правят, и повторное «Загрузить» не повод стирать эту
// правку — оно просто назначает готовое. Материал, назначенный человеком
// вручную, тоже не трогается.
void InspectorPanel::AutoAssignModelMaterial(EditorHost& host, MeshRendererComponent& mr) {
    if (mr.Ref.path.empty()) return;
    if (!mr.MaterialPath.empty()) return;   // выбор человека главнее импорта

    // Путь к самому файлу модели: в Ref он относительный (см. Project::AssetRef),
    // а читать надо настоящий файл на диске.
    const std::string modelPath = sage::AssetDatabase::Instance().LocatePath(mr.Ref.path);
    const fs::path matPath = fs::path(modelPath).replace_extension(".sagemat");

    std::error_code ec;
    if (!fs::exists(matPath, ec)) {
        const ModelLoader::ExtractedMaterial ex = ModelLoader::ExtractMaterial(modelPath);
        if (!ex.Found) {
            // Нет материала в файле — и не надо: белая болванка это честный
            // результат «в модели материала нет», а не поломка.
            if (!ex.Warnings.empty())
                host.SetStatusMessage("Материал модели: " + ex.Warnings.front());
            return;
        }

        Material mat;
        mat.Albedo = ex.Albedo;
        mat.Emissive = ex.Emissive;
        mat.EmissiveStrength = ex.EmissiveStrength;
        mat.Metallic = ex.Metallic;
        mat.Roughness = ex.Roughness;
        mat.Opacity = ex.Opacity;
        // Пути карт — относительно проекта: материал переживёт сборку игры и
        // переезд проекта (см. Project::AssetRef).
        const Project& project = host.CurrentProject();
        mat.TexturePath = project.AssetRef(ex.AlbedoMap);
        mat.NormalMapPath = project.AssetRef(ex.NormalMap);
        mat.MetallicMapPath = project.AssetRef(ex.MetallicMap);
        mat.RoughnessMapPath = project.AssetRef(ex.RoughnessMap);
        mat.AOMapPath = project.AssetRef(ex.AOMap);
        mat.EmissiveMap = project.AssetRef(ex.EmissiveMap);

        try {
            mat.SaveToFile(matPath.string());
        } catch (const std::exception& e) {
            LOG_ERROR("Editor") << "Материал модели не сохранён: " << e.what();
            host.SetStatusMessage("Материал модели не сохранён — подробности в Console");
            return;
        }
        sage::AssetDatabase::Instance().Register(matPath.string(), "material");
        host.SetStatusMessage("Материал модели импортирован: " +
                              matPath.filename().string() +
                              (ex.HasAnyMap() ? " (с картами)" : ""));
        LOG_INFO("Editor") << "Материал модели импортирован: " << matPath.string();
    }

    mr.MaterialPath = host.CurrentProject().AssetRef(matPath);
    mr.MaterialPtr = ResourceManager::Instance().GetMaterial(mr.MaterialPath);
}

// --- Mesh Renderer, часть 1: ЧТО рисуем --------------------------------------
void InspectorPanel::DrawMeshSlot(EditorHost& host, MeshRendererComponent& mr) {
    ImGui::SeparatorText("Меш");

    // Порядок строго совпадает с MeshRef::Type (индекс комбо = значение enum).
    const char* kinds[] = {"Нет", "Куб", "Сфера", "Плоскость", "Цилиндр", "Конус", "Модель"};
    int kind = (int)mr.Ref.type;
    if (ImGui::Combo("Источник", &kind, kinds, IM_ARRAYSIZE(kinds))) {
        host.PushUndoSnapshot(); // дискретное изменение — прямая запись undo
        mr.Ref.type = (MeshRef::Type)kind;
        if (mr.Ref.type != MeshRef::Type::Model) {
            mr.Ref.path.clear();
            mr.MeshPtr = ResourceManager::Instance().GetPrimitive(mr.Ref.type); // Нет -> nullptr
        }
        // Model — путь задаётся ниже и грузится кнопкой.
    }

    if (mr.Ref.type == MeshRef::Type::Model) {
        char pathBuf[512];
        std::snprintf(pathBuf, sizeof(pathBuf), "%s", mr.Ref.path.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##modelpath", pathBuf, sizeof(pathBuf))) mr.Ref.path = pathBuf;
        host.TrackLastImGuiItem();
        // Перетаскивание из панели Assets — тот же способ назначить файл, что и
        // у слотов текстур материала. Раньше модель этого не умела, хотя тайл в
        // Assets уже был источником перетаскивания.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ASSET_PATH")) {
                std::string dropped((const char*)p->Data, (size_t)p->DataSize);
                if (!dropped.empty() && dropped.back() == '\0') dropped.pop_back();
                if (ModelLoader::IsSupportedModel(dropped)) {
                    host.PushUndoSnapshot();
                    mr.Ref.path = host.CurrentProject().AssetRef(dropped);
                    m_pendingMeshLoad = true;
                }
            }
            ImGui::EndDragDropTarget();
        }

        // «Обзор…» вместо «напечатай путь наизусть». Именно на этом шаге
        // всё и заканчивалось: человек не помнит абсолютный путь к своей
        // модели, а ошибка в нём давала только строчку в консоли.
        if (EditorIcons::Button("folder", "Обзор…")) {
            FileBrowser::Config c;
            c.Title = "Выбрать модель";
            c.Filters = {".obj", ".gltf", ".glb", ".sagemesh"};
            c.FilterLabel = "Модели (*.obj, *.gltf, *.glb, *.sagemesh)";
            if (host.CurrentProject().Loaded()) c.StartDir = host.CurrentProject().AssetsDir();
            m_browser.Open(c);
            m_browseTarget = &mr.Ref.path;
            m_browseIsShader = false;
            m_browseIsMesh = true;
        }
        ImGui::SameLine();
        const bool selIsModel = ModelLoader::IsSupportedModel(
            host.SelectedAssetPath().extension().string());
        ImGui::BeginDisabled(!selIsModel);
        if (EditorIcons::Button("model", "Из Assets")) {
            host.PushUndoSnapshot();
            mr.Ref.path = host.CurrentProject().AssetRef(host.SelectedAssetPath());
            m_pendingMeshLoad = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (EditorIcons::Button("refresh", "Загрузить")) m_pendingMeshLoad = true;

        if (m_pendingMeshLoad) {
            m_pendingMeshLoad = false;
            mr.MeshPtr = ResourceManager::Instance().GetModel(mr.Ref.path);
            // GetModel сам логирует причину и отдаёт nullptr — сообщаем об
            // этом ЗДЕСЬ, в панели: строчку в консоли легко не заметить, а
            // «модель не появилась» без объяснения выглядит как поломка
            // редактора.
            if (!mr.MeshPtr) {
                host.SetStatusMessage("Модель не загрузилась: " + mr.Ref.path +
                                      " — подробности в Console");
            } else {
                // Материал модели — вместе с моделью, а не «потом руками».
                // Подробности см. в AutoAssignModelMaterial.
                AutoAssignModelMaterial(host, mr);
            }
        }
        if (!mr.Ref.path.empty() && !mr.MeshPtr) {
            ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "Меш не загружен");
            if (!ModelLoader::IsSupportedModel(mr.Ref.path)) {
                ImGui::TextDisabled("Поддерживаются .obj, .gltf, .glb, .sagemesh");
            }
        }
        // Абсолютный путь работает в редакторе и НЕ работает нигде больше —
        // говорим об этом сразу, а не после сборки игры.
        if (!mr.Ref.path.empty() && host.CurrentProject().Loaded() &&
            std::filesystem::path(mr.Ref.path).is_absolute()) {
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), "Файл вне проекта");
            ImGui::TextDisabled("В собранной игре не найдётся. Внесите его в проект:");
            ImGui::TextDisabled("Assets -> Импорт…");
        }
    }

    if (mr.MeshPtr) {
        const glm::vec3 size = mr.MeshPtr->BoundsMax() - mr.MeshPtr->BoundsMin();
        ImGui::TextDisabled("Треугольников: %d, габарит %.2f x %.2f x %.2f",
                            (int)(mr.MeshPtr->IndexCount() / 3), size.x, size.y, size.z);
    }
}

// --- Mesh Renderer, часть 2: ЧЕМ красим --------------------------------------
void InspectorPanel::DrawMaterialSlot(EditorHost& host, MeshRendererComponent& mr) {
    ImGui::SeparatorText("Материал");

    // Превью материала — шариком, а не квадратиком цвета: по квадратику не
    // отличить металл от диэлектрика и гладкое от матового, то есть ровно то,
    // ради чего материал и назначают. Тот же рендер, что в редакторе материала.
    const float side = 64.0f;
    if (mr.MaterialPtr) {
        const uint64_t thumb = m_preview.RenderMaterial(mr.MaterialPtr, (int)side);
        if (thumb) {
            ImGui::Image((ImTextureID)(std::intptr_t)thumb, ImVec2(side, side), ImVec2(0, 1),
                         ImVec2(1, 0));
        } else {
            ImGui::ColorButton("##mat_preview",
                               ImVec4(mr.MaterialPtr->Albedo.r, mr.MaterialPtr->Albedo.g,
                                      mr.MaterialPtr->Albedo.b, 1.0f),
                               0, ImVec2(side, side));
        }
        ImGui::SameLine();
    }

    ImGui::BeginGroup();
    if (mr.MaterialPath.empty()) {
        ImGui::TextDisabled("Не назначен");
        ImGui::TextDisabled("Объект рисуется поправками ниже как есть.");
    } else {
        ImGui::TextUnformatted(std::filesystem::path(mr.MaterialPath).filename().string().c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", mr.MaterialPath.c_str());
        if (!mr.MaterialPtr)
            ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "Файл не читается");
    }

    auto assign = [&](const std::string& raw) {
        host.PushUndoSnapshot();
        mr.MaterialPath = host.CurrentProject().AssetRef(raw);
        mr.MaterialPtr = ResourceManager::Instance().GetMaterial(mr.MaterialPath);
    };

    const bool selIsMaterial = host.SelectedAssetPath().extension() == ".sagemat";
    ImGui::BeginDisabled(!selIsMaterial);
    if (EditorIcons::Button("material", "Из Assets")) assign(host.SelectedAssetPath().string());
    ImGui::EndDisabled();
    if (!selIsMaterial && ImGui::IsItemHovered())
        ImGui::SetTooltip("Выберите .sagemat в панели Assets");
    ImGui::SameLine();
    if (EditorIcons::Button("folder", "Обзор…")) {
        FileBrowser::Config c;
        c.Title = "Выбрать материал";
        c.Filters = {".sagemat"};
        c.FilterLabel = "Материалы (*.sagemat)";
        if (host.CurrentProject().Loaded()) c.StartDir = host.CurrentProject().AssetsDir();
        m_browser.Open(c);
        m_browseTarget = &mr.MaterialPath;
        m_browseIsShader = false;
        m_browseIsMesh = false;
        m_browseIsMaterial = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(mr.MaterialPath.empty());
    if (EditorIcons::Button("trash", "Убрать")) {
        host.PushUndoSnapshot();
        mr.MaterialPath.clear();
        mr.MaterialPtr = nullptr;
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();

    // Приём перетаскивания на всю группу выше — материал из Assets мышью.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ASSET_PATH")) {
            std::string dropped((const char*)p->Data, (size_t)p->DataSize);
            if (!dropped.empty() && dropped.back() == '\0') dropped.pop_back();
            if (std::filesystem::path(dropped).extension() == ".sagemat") assign(dropped);
        }
        ImGui::EndDragDropTarget();
    }
}

// --- Mesh Renderer, часть 3: чем ЭТОТ экземпляр отличается -------------------
void InspectorPanel::DrawInstanceOverrides(EditorHost& host, MeshRendererComponent& mr) {
    ImGui::SeparatorText("Поправки экземпляра");
    // Одна подпись на всю группу вместо трёх разных правил, которые надо было
    // помнить (см. EffectiveColor/EffectiveEmissive/EffectiveOpacity).
    ImGui::TextDisabled(mr.MaterialPtr ? "Накладываются поверх материала."
                                       : "Материала нет — задают вид объекта целиком.");

    ImGui::ColorEdit3("Тон", &mr.Color.x); host.TrackLastImGuiItem();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Множится на albedo материала. Белый — как в материале.");

    ImGui::ColorEdit3("Свечение", &mr.Emissive.x); host.TrackLastImGuiItem();
    ImGui::DragFloat("Сила свечения", &mr.EmissiveStrength, 0.05f, 0.0f, 20.0f, "%.2f");
    host.TrackLastImGuiItem();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Прибавляется к свечению материала. Больше 1 — ореол (bloom).");

    // Непрозрачность < 1 уводит объект в полупрозрачный проход (сортировка
    // от дальних, блендинг, без записи глубины) — см. ecs/RenderBatch.
    ImGui::SliderFloat("Непрозрачность", &mr.Opacity, 0.0f, 1.0f); host.TrackLastImGuiItem();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Множится на непрозрачность материала.");

    // Кнопка «как в материале» — вернуть поправки в нейтраль. Без неё «я где-то
    // подкрутил цвет этому объекту» лечится только вспоминанием исходных чисел.
    const bool neutral = mr.Color == glm::vec3(1.0f) && mr.Emissive == glm::vec3(0.0f) &&
                         mr.Opacity >= 0.999f;
    ImGui::BeginDisabled(neutral);
    if (ImGui::SmallButton("Сбросить поправки")) {
        host.PushUndoSnapshot();
        mr.Color = glm::vec3(1.0f);
        mr.Emissive = glm::vec3(0.0f);
        mr.EmissiveStrength = 1.0f;
        mr.Opacity = 1.0f;
    }
    ImGui::EndDisabled();
}

// Префаб в инспекторе: та же вращаемая обложка, что у материала. Крупнее, чем в
// панели Assets, потому что здесь на неё и смотрят — выбирают, тот ли это ящик.
void InspectorPanel::DrawPrefabPreview(EditorHost& host) {
    const std::string path = host.SelectedAssetPath().string();
    const float side = std::min(ImGui::GetContentRegionAvail().x, 220.0f);
    const uint64_t tex = m_preview.RenderPrefab(path, (int)side);
    if (!tex) {
        ImGui::TextDisabled("Обложки нет: в префабе не нашлось видимой геометрии");
        ImGui::TextDisabled("(или файл не читается — подробности в Console).");
    } else {
        ImGui::Image((ImTextureID)(std::intptr_t)tex, ImVec2(side, side), ImVec2(0, 1),
                     ImVec2(1, 0));
        if (ImGui::IsItemHovered()) {
            ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                m_preview.Orbit(io.MouseDelta.x * 0.5f, -io.MouseDelta.y * 0.5f);
            if (io.MouseWheel != 0.0f) m_preview.Zoom(io.MouseWheel);
            ImGui::SetTooltip("ЛКМ — вращать, колесо — приблизить");
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextDisabled("Превью");
        if (ImGui::SmallButton("Сбросить вид")) m_preview.ResetView();
        ImGui::EndGroup();
    }

    ImGui::Spacing();
    if (ImGui::Button("Поставить в сцену", ImVec2(-1, 0))) host.InstantiatePrefab(path);
}

void InspectorPanel::DrawEntityProperties(EditorHost& host) {
    GameObject obj = host.SelectedObject();
    entt::registry& reg = host.CurrentScene().Registry();

    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", obj.Name().c_str());
    if (ImGui::InputText("Name", buf, sizeof(buf))) obj.SetName(buf);
    host.TrackLastImGuiItem();
    ImGui::TextDisabled("Id: %d", obj.Id());
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        Transform& tr = obj.GetTransform();
        ImGui::DragFloat3("Position", &tr.Position.x, 0.05f); host.TrackLastImGuiItem();
        ImGui::DragFloat3("Rotation", &tr.Rotation.x, 0.5f); host.TrackLastImGuiItem();
        ImGui::DragFloat3("Scale", &tr.Scale.x, 0.05f, 0.01f, 100.0f); host.TrackLastImGuiItem();
    }

    // --- Mesh Renderer: ОДНА секция на весь компонент -------------------------
    //
    // Раньше их было две — «Mesh Renderer» и «Material», — и вторая выглядела
    // отдельным компонентом, хотя правила ТЕ ЖЕ два поля того же
    // MeshRendererComponent. Хуже того, они противоречили друг другу: сверху
    // стоял Color, снизу подпись «материал заменяет Color», и после назначения
    // материала верхний ползунок цвета переставал что-либо делать — молча.
    //
    // Теперь порядок повторяет саму структуру компонента (см.
    // ecs/RenderComponents.h): ЧТО рисуем -> ЧЕМ красим -> чем ЭТОТ экземпляр
    // отличается от других таких же.
    if (ImGui::CollapsingHeader("Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
        MeshRendererComponent& mr = obj.Renderer();
        DrawMeshSlot(host, mr);
        DrawMaterialSlot(host, mr);
        DrawInstanceOverrides(host, mr);
    }

    // --- Камера (игровая): панель Game рендерит от первой Primary-камеры ---
    if (reg.all_of<CameraComponent>(obj.Entity()) && ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (CameraComponent* cam = reg.try_get<CameraComponent>(obj.Entity())) {
            ImGui::DragFloat("FOV", &cam->Fov, 0.5f, 10.0f, 140.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat("Near", &cam->NearClip, 0.01f, 0.001f, 10.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat("Far", &cam->FarClip, 1.0f, 1.0f, 5000.0f); host.TrackLastImGuiItem();
            if (ImGui::Checkbox("Primary", &cam->Primary)) host.PushUndoSnapshot();
            ImGui::TextDisabled("Game panel renders from the first Primary camera");
            if (ImGui::Button("Remove Camera")) {
                host.PushUndoSnapshot();
                reg.remove<CameraComponent>(obj.Entity());
            }
        }
    }

    // --- Свет (позиция — Transform сущности; тип: точечный / прожектор) ---
    if (reg.all_of<LightComponent>(obj.Entity()) && ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (LightComponent* light = reg.try_get<LightComponent>(obj.Entity())) {
            const char* types[] = {"Point", "Spot"};
            int kind = (int)light->Kind;
            if (ImGui::Combo("Type", &kind, types, 2)) {
                host.PushUndoSnapshot(); // дискретное изменение — прямая запись undo
                light->Kind = (LightComponent::Type)kind;
            }
            ImGui::ColorEdit3("Light Color", &light->Color.x); host.TrackLastImGuiItem();
            ImGui::DragFloat("Intensity", &light->Intensity, 0.02f, 0.0f, 10.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat("Range", &light->Range, 0.1f, 0.5f, 100.0f); host.TrackLastImGuiItem();
            if (light->Kind == LightComponent::Type::Spot) {
                ImGui::DragFloat("Inner Cone", &light->InnerConeDeg, 0.5f, 1.0f, 89.0f); host.TrackLastImGuiItem();
                ImGui::DragFloat("Outer Cone", &light->OuterConeDeg, 0.5f, 1.0f, 89.0f); host.TrackLastImGuiItem();
                // Внешний угол не должен быть уже внутреннего (иначе конус
                // «выворачивается»): подтягиваем внешний до внутреннего.
                if (light->OuterConeDeg < light->InnerConeDeg) light->OuterConeDeg = light->InnerConeDeg;
                ImGui::TextDisabled("Cone points along the entity's forward (-Z rotation)");
            } else {
                ImGui::TextDisabled("Point light at this entity's position");
            }
            if (ImGui::Button("Remove Light")) {
                host.PushUndoSnapshot();
                reg.remove<LightComponent>(obj.Entity());
            }
        }
    }

    // --- Наклейка (проекция картинки на геометрию сцены) ---
    if (reg.all_of<DecalComponent>(obj.Entity()) &&
        ImGui::CollapsingHeader("Decal", ImGuiTreeNodeFlags_DefaultOpen)) {
        DecalComponent& dc = reg.get<DecalComponent>(obj.Entity());
        bool changed = false;
        changed |= ImGui::DragFloat("Angle Limit", &dc.AngleLimitDeg, 1.0f, 1.0f, 89.0f, "%.0f°");
        host.TrackLastImGuiItem();
        changed |= ImGui::DragFloat("Surface Offset", &dc.Offset, 0.001f, 0.0f, 0.5f, "%.3f");
        host.TrackLastImGuiItem();

        // Треугольники — главный ответ на «почему наклейки не видно». Ноль
        // значит, что под коробкой не оказалось подходящей геометрии, а не что
        // сломался рендер, и лечится это перемещением, а не настройками.
        //
        // Подсказки — с переносом по ширине панели. Без него текст просто
        // обрезается на границе: панель у людей узкая, и «размер задаёт Scal»
        // читается как поломка редактора, а не как совет.
        if (dc.Triangles > 0) {
            ImGui::TextDisabled("Спроецировано треугольников: %d", dc.Triangles);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.3f, 1.0f));
            ImGui::TextWrapped("Ни на что не легла: под коробкой нет геометрии, либо она "
                               "отвёрнута от наклейки.");
            ImGui::PopStyleColor();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("Проекция идёт вдоль -Z; размер задаёт Scale");
        ImGui::PopStyleColor();
        if (ImGui::Button("Rebuild##decal")) changed = true;
        ImGui::SameLine();
        if (ImGui::Button("Remove##decal")) {
            host.PushUndoSnapshot();
            reg.remove<DecalComponent>(obj.Entity());
            changed = false;
        }
        // Правка параметров обязана быть видна сразу: наклейка пересобирается
        // по флагу, и без него ползунок угла не менял бы вообще ничего.
        if (changed && reg.all_of<DecalComponent>(obj.Entity()))
            reg.get<DecalComponent>(obj.Entity()).Dirty = true;
    }

    // --- Скрипт (поведение в Play-режиме) ---
    if (reg.all_of<GIStaticComponent>(obj.Entity()) && ImGui::CollapsingHeader("GI Static", ImGuiTreeNodeFlags_DefaultOpen)) {
        GIStaticComponent& gs = reg.get<GIStaticComponent>(obj.Entity());
        ImGui::Checkbox("Lightmapped", &gs.Lightmapped); host.TrackLastImGuiItem();
        ImGui::DragFloat("Texel Scale", &gs.TexelScale, 0.05f, 0.1f, 8.0f); host.TrackLastImGuiItem();
        ImGui::TextDisabled("Static occluder for baked GI; lightmapped = has own lightmap");
        ImGui::TextDisabled("Re-bake lighting after changes (Lighting panel)");
        if (ImGui::Button("Remove##gistatic")) {
            host.PushUndoSnapshot();
            reg.remove<GIStaticComponent>(obj.Entity());
        }
    }

    if (reg.all_of<ScriptComponent>(obj.Entity()) && ImGui::CollapsingHeader("Script", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ScriptComponent* sc = reg.try_get<ScriptComponent>(obj.Entity())) {
            char scriptBuf[512];
            std::snprintf(scriptBuf, sizeof(scriptBuf), "%s", sc->Path.c_str());
            if (ImGui::InputText("Lua file", scriptBuf, sizeof(scriptBuf))) sc->Path = scriptBuf;
            host.TrackLastImGuiItem();
            ImGui::TextDisabled("Runs in Play mode: OnStart(entity), OnUpdate(entity, dt)");
            if (ImGui::Button("Remove Script")) {
                host.PushUndoSnapshot();
                reg.remove<ScriptComponent>(obj.Entity());
            }
        }
    }

    // --- Твёрдое тело (симулируется в Play-режиме выбранным бэкендом физики) ---
    if (reg.all_of<RigidBodyComponent>(obj.Entity()) && ImGui::CollapsingHeader("Rigid Body", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (RigidBodyComponent* rb = reg.try_get<RigidBodyComponent>(obj.Entity())) {
            // Порядок строго совпадает с sage::physics::BodyType.
            const char* types[] = {"Static", "Dynamic", "Kinematic"};
            int kind = (int)rb->Type;
            if (ImGui::Combo("Body Type", &kind, types, IM_ARRAYSIZE(types))) {
                host.PushUndoSnapshot();
                rb->Type = (sage::physics::BodyType)kind;
            }
            ImGui::DragFloat("Mass", &rb->Mass, 0.05f, 0.0f, 1000.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat("Friction", &rb->Friction, 0.01f, 0.0f, 1.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat("Restitution", &rb->Restitution, 0.01f, 0.0f, 1.0f); host.TrackLastImGuiItem();
            ImGui::TextDisabled("Dynamic falls under gravity; Static/Kinematic don't");
            if (ImGui::Button("Remove Rigid Body")) {
                host.PushUndoSnapshot();
                reg.remove<RigidBodyComponent>(obj.Entity());
            }
        }
    }

    // --- Коллайдер (форма для физики; размеры домножаются на Transform.Scale) ---
    if (reg.all_of<ColliderComponent>(obj.Entity()) && ImGui::CollapsingHeader("Collider", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ColliderComponent* col = reg.try_get<ColliderComponent>(obj.Entity())) {
            // Порядок строго совпадает с sage::physics::ShapeType.
            const char* shapes[] = {"Box", "Sphere", "Capsule"};
            int shape = (int)col->Shape;
            if (ImGui::Combo("Shape", &shape, shapes, IM_ARRAYSIZE(shapes))) {
                host.PushUndoSnapshot();
                col->Shape = (sage::physics::ShapeType)shape;
            }
            if (col->Shape == sage::physics::ShapeType::Box) {
                ImGui::DragFloat3("Half Extents", &col->HalfExtents.x, 0.02f, 0.001f, 100.0f);
                host.TrackLastImGuiItem();
            } else if (col->Shape == sage::physics::ShapeType::Sphere) {
                ImGui::DragFloat("Radius", &col->Radius, 0.02f, 0.001f, 100.0f);
                host.TrackLastImGuiItem();
            } else { // Capsule
                ImGui::DragFloat("Radius", &col->Radius, 0.02f, 0.001f, 100.0f);
                host.TrackLastImGuiItem();
                ImGui::DragFloat("Half Height", &col->HalfHeight, 0.02f, 0.001f, 100.0f);
                host.TrackLastImGuiItem();
            }
            ImGui::TextDisabled("Sizes are scaled by the entity's Transform scale");

            // --- Составная (compound) форма: список дочерних примитивов ---
            ImGui::Separator();
            ImGui::Text("Compound parts: %d", (int)col->Parts.size());
            if (!col->Parts.empty())
                ImGui::TextDisabled("Parts override the single shape above");
            const char* shapeNames[] = {"Box", "Sphere", "Capsule"};
            int removePart = -1;
            for (int pi = 0; pi < (int)col->Parts.size(); ++pi) {
                ColliderComponent::Part& p = col->Parts[pi];
                ImGui::PushID(pi);
                if (ImGui::TreeNodeEx("part", ImGuiTreeNodeFlags_DefaultOpen, "Part %d", pi)) {
                    int ps = (int)p.Shape;
                    if (ImGui::Combo("Shape", &ps, shapeNames, IM_ARRAYSIZE(shapeNames))) {
                        host.PushUndoSnapshot();
                        p.Shape = (sage::physics::ShapeType)ps;
                    }
                    if (p.Shape == sage::physics::ShapeType::Box) {
                        ImGui::DragFloat3("Half Extents", &p.HalfExtents.x, 0.02f, 0.001f, 100.0f);
                        host.TrackLastImGuiItem();
                    } else {
                        ImGui::DragFloat("Radius", &p.Radius, 0.02f, 0.001f, 100.0f);
                        host.TrackLastImGuiItem();
                        if (p.Shape == sage::physics::ShapeType::Capsule) {
                            ImGui::DragFloat("Half Height", &p.HalfHeight, 0.02f, 0.001f, 100.0f);
                            host.TrackLastImGuiItem();
                        }
                    }
                    ImGui::DragFloat3("Offset", &p.Offset.x, 0.02f);
                    host.TrackLastImGuiItem();
                    ImGui::DragFloat3("Rotation", &p.EulerDeg.x, 0.5f);
                    host.TrackLastImGuiItem();
                    if (ImGui::SmallButton("Remove Part")) removePart = pi;
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (removePart >= 0) {
                host.PushUndoSnapshot();
                col->Parts.erase(col->Parts.begin() + removePart);
            }
            if (ImGui::Button("Add Part")) {
                host.PushUndoSnapshot();
                col->Parts.push_back(ColliderComponent::Part{});
            }

            if (ImGui::Button("Remove Collider")) {
                host.PushUndoSnapshot();
                reg.remove<ColliderComponent>(obj.Entity());
            }
        }
    }

    // --- Соединение (constraint/joint) с другим телом или миром ---
    if (reg.all_of<JointComponent>(obj.Entity()) && ImGui::CollapsingHeader("Joint", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (JointComponent* jc = reg.try_get<JointComponent>(obj.Entity())) {
            const char* types[] = {"Fixed", "Point", "Hinge", "Slider", "Distance", "Cone"};
            int t = (int)jc->Type;
            if (ImGui::Combo("Type", &t, types, IM_ARRAYSIZE(types))) {
                host.PushUndoSnapshot();
                jc->Type = (sage::physics::JointType)t;
            }
            ImGui::DragInt("Target Id (-1 = world)", &jc->TargetId, 0.1f, -1, 100000);
            host.TrackLastImGuiItem();
            ImGui::DragFloat3("Anchor (offset)", &jc->Anchor.x, 0.02f);
            host.TrackLastImGuiItem();
            using JT = sage::physics::JointType;
            if (jc->Type == JT::Hinge || jc->Type == JT::Slider || jc->Type == JT::Cone) {
                ImGui::DragFloat3("Axis", &jc->Axis.x, 0.02f);
                host.TrackLastImGuiItem();
            }
            if (jc->Type == JT::Hinge || jc->Type == JT::Slider) {
                ImGui::Checkbox("Use Limits", &jc->UseLimits);
                if (jc->UseLimits) {
                    ImGui::DragFloat("Min", &jc->MinLimit, 0.5f);
                    host.TrackLastImGuiItem();
                    ImGui::DragFloat("Max", &jc->MaxLimit, 0.5f);
                    host.TrackLastImGuiItem();
                }
            } else if (jc->Type == JT::Distance) {
                ImGui::DragFloat("Min Distance", &jc->MinDistance, 0.02f, 0.0f, 100.0f);
                host.TrackLastImGuiItem();
                ImGui::DragFloat("Max Distance", &jc->MaxDistance, 0.02f, 0.0f, 100.0f);
                host.TrackLastImGuiItem();
            } else if (jc->Type == JT::Cone) {
                ImGui::DragFloat("Cone Half Angle", &jc->ConeHalfAngle, 0.5f, 0.0f, 180.0f);
                host.TrackLastImGuiItem();
            }
            ImGui::TextDisabled("Needs a Rigid Body; only the Jolt backend simulates joints");
            if (ImGui::Button("Remove Joint")) {
                host.PushUndoSnapshot();
                reg.remove<JointComponent>(obj.Entity());
            }
        }
    }

    // --- Скелетно-анимированная модель (.glb/.gltf или процедурное демо) ---
    if (reg.all_of<AnimatedModelComponent>(obj.Entity()) && ImGui::CollapsingHeader("Animated Model", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (AnimatedModelComponent* am = reg.try_get<AnimatedModelComponent>(obj.Entity())) {
            char pathBuf[512];
            std::snprintf(pathBuf, sizeof(pathBuf), "%s", am->Path.c_str());
            if (ImGui::InputText("Model (.glb)", pathBuf, sizeof(pathBuf))) am->Path = pathBuf;
            host.TrackLastImGuiItem();
            ImGui::TextDisabled("Empty path = procedural demo (\"tentacle\")");
            if (am->Path.empty()) {
                if (ImGui::SliderInt("Demo Segments", &am->DemoSegments, 2, 16)) {
                    am->Ready = false; am->Model = nullptr; // пересобрать демо
                }
            }
            if (ImGui::Button("Reload")) { am->Ready = false; am->Model = nullptr; }

            // Список клипов — из проигрывателя (модель уже загружена системой).
            int clipCount = am->Anim.ClipCount();
            if (clipCount > 0) {
                if (am->Clip >= clipCount) am->Clip = 0;
                std::string preview = am->Anim.ClipName(am->Clip);
                if (ImGui::BeginCombo("Clip", preview.c_str())) {
                    for (int i = 0; i < clipCount; ++i) {
                        bool sel = (am->Clip == i);
                        if (ImGui::Selectable(am->Anim.ClipName(i).c_str(), sel) && i != am->Clip) {
                            am->Clip = i;
                            // Плавный кросс-фейд к выбранному клипу (или резко, если 0).
                            if (am->BlendTime > 0.0f) am->Anim.CrossFade(i, am->BlendTime, am->Loop);
                            else am->Anim.Play(i, am->Loop);
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::DragFloat("Blend Time", &am->BlendTime, 0.01f, 0.0f, 2.0f);
                host.TrackLastImGuiItem();
                if (am->Anim.Fading())
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 1.0f, 1.0f), "cross-fading %.0f%%",
                                       am->Anim.FadeWeight() * 100.0f);
            } else {
                ImGui::TextDisabled("No animation clips (bind pose)");
            }
            ImGui::DragFloat("Speed", &am->Speed, 0.02f, 0.0f, 8.0f); host.TrackLastImGuiItem();
            if (ImGui::Checkbox("Loop", &am->Loop)) am->Anim.Play(am->Clip, am->Loop);
            ImGui::SameLine();
            ImGui::Checkbox("Playing", &am->Playing);
            if (clipCount > 0) {
                ImGui::TextDisabled("t = %.2f s", am->Anim.Time());
            }
            ImGui::Checkbox("Root Motion", &am->RootMotion);
            if (ImGui::Button("Remove Animated Model")) {
                host.PushUndoSnapshot();
                reg.remove<AnimatedModelComponent>(obj.Entity());
            }
        }
    }

    // --- Зонд отражений -----------------------------------------------------
    if (reg.all_of<ReflectionProbeComponent>(obj.Entity()) &&
        ImGui::CollapsingHeader("Reflection Probe", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ReflectionProbeComponent* p = reg.try_get<ReflectionProbeComponent>(obj.Entity())) {
            ImGui::TextDisabled("Captures the scene around this point into a cubemap");
            // Любая правка охвата или разрешения означает «снять заново»: карта
            // снята под прежние числа, и оставить её значило бы показывать
            // отражение, которого в сцене уже нет.
            const int prevRes = p->Resolution;
            const char* resNames[] = {"32", "64", "128", "256"};
            const int resValues[] = {32, 64, 128, 256};
            int resIdx = 2;
            for (int i = 0; i < 4; ++i) if (resValues[i] == p->Resolution) resIdx = i;
            if (ImGui::Combo("Resolution", &resIdx, resNames, 4)) p->Resolution = resValues[resIdx];
            if (p->Resolution != prevRes) p->Dirty = true;

            if (ImGui::DragFloat3("Box Half Extents", &p->BoxHalfExtents.x, 0.1f, 0.1f, 500.0f))
                p->Dirty = true;
            host.TrackLastImGuiItem();
            ImGui::TextDisabled("Camera inside this box uses this probe");
            if (ImGui::Checkbox("Box Parallax", &p->BoxParallax)) p->Dirty = true;
            ImGui::SliderFloat("Intensity", &p->Intensity, 0.0f, 3.0f);
            host.TrackLastImGuiItem();
            if (ImGui::DragFloat("Far Clip", &p->FarClip, 0.5f, 1.0f, 2000.0f)) p->Dirty = true;
            host.TrackLastImGuiItem();
            ImGui::Checkbox("Realtime (re-capture every frame)", &p->Realtime);
            ImGui::TextDisabled("Realtime = 6 scene passes per frame - use sparingly");

            if (ImGui::Button("Bake Probe")) p->Dirty = true;
            ImGui::SameLine();
            ImGui::TextDisabled(p->Dirty ? "queued" : (p->Runtime ? "captured" : "empty"));
            if (ImGui::Button("Remove Reflection Probe")) {
                host.PushUndoSnapshot();
                reg.remove<ReflectionProbeComponent>(obj.Entity());
            }
        }
    }

    // --- Обратная кинематика: цели поверх позы клипа ------------------------
    // Кость задаётся ИМЕНЕМ, а не индексом: модель может смениться (или ещё не
    // загрузиться), а имя переживает и то, и другое. Список имён показываем
    // только когда скелет уже есть.
    if (reg.all_of<IKComponent>(obj.Entity()) && ImGui::CollapsingHeader("IK", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (IKComponent* ik = reg.try_get<IKComponent>(obj.Entity())) {
            ImGui::Checkbox("IK Enabled", &ik->Enabled);
            const AnimatedModelComponent* am = reg.try_get<AnimatedModelComponent>(obj.Entity());
            if (!am) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                                        "No Animated Model - goals do nothing");

            int remove = -1;
            for (int gi = 0; gi < (int)ik->Goals.size(); ++gi) {
                IKGoal& g = ik->Goals[(size_t)gi];
                ImGui::PushID(gi);
                const std::string title = "Goal " + std::to_string(gi) +
                                          (g.Bone.empty() ? "" : " (" + g.Bone + ")");
                if (ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    char boneBuf[128];
                    std::snprintf(boneBuf, sizeof(boneBuf), "%s", g.Bone.c_str());
                    if (ImGui::InputText("Bone", boneBuf, sizeof(boneBuf))) {
                        g.Bone = boneBuf;
                        g.Resolved = false;   // имя сменилось — искать заново
                    }
                    host.TrackLastImGuiItem();
                    // Выбор из реального скелета — чтобы не угадывать написание.
                    if (am && am->Model) {
                        const sage::anim::Skeleton& sk = am->Model->GetSkeleton();
                        if (sk.Count() > 0 && ImGui::BeginCombo("Pick Bone", g.Bone.c_str())) {
                            for (int b = 0; b < sk.Count(); ++b) {
                                const std::string& n = sk.Joints[(size_t)b].Name;
                                if (ImGui::Selectable(n.c_str(), n == g.Bone) && n != g.Bone) {
                                    g.Bone = n;
                                    g.Resolved = false;
                                }
                            }
                            ImGui::EndCombo();
                        }
                    }
                    ImGui::Checkbox("Enabled", &g.Enabled);
                    ImGui::SameLine();
                    if (ImGui::Checkbox("Aim (look-at)", &g.Aim)) g.Resolved = false;
                    if (g.Aim) {
                        ImGui::DragFloat3("Aim Axis", &g.AimAxis.x, 0.01f);
                        host.TrackLastImGuiItem();
                        ImGui::SliderFloat("Max Angle", &g.AimMaxAngle, 0.0f, 180.0f);
                        host.TrackLastImGuiItem();
                    } else {
                        if (ImGui::SliderInt("Chain Length", &g.ChainLength, 2, 8)) g.Resolved = false;
                        host.TrackLastImGuiItem();
                        ImGui::TextDisabled("2 = analytic two-bone, more = FABRIK");
                        ImGui::Checkbox("Use Pole", &g.UsePole);
                        if (g.UsePole) {
                            ImGui::DragFloat3("Pole", &g.Pole.x, 0.02f);
                            host.TrackLastImGuiItem();
                        }
                        ImGui::DragFloat3("Align Normal", &g.AlignNormal.x, 0.02f);
                        host.TrackLastImGuiItem();
                        ImGui::Checkbox("Foot Lock", &g.Lock);
                        if (g.Lock) {
                            ImGui::DragFloat("Plant Height", &g.PlantHeight, 0.005f, 0.0f, 1.0f);
                            host.TrackLastImGuiItem();
                            ImGui::DragFloat("Release Time", &g.ReleaseTime, 0.005f, 0.0f, 1.0f);
                            host.TrackLastImGuiItem();
                        }
                    }
                    ImGui::DragFloat3("Target (world)", &g.Target.x, 0.02f);
                    host.TrackLastImGuiItem();
                    ImGui::SliderFloat("Weight", &g.Weight, 0.0f, 1.0f);
                    host.TrackLastImGuiItem();
                    if (g.Resolved && g.EndJoint < 0)
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "bone not found in skeleton");
                    if (ImGui::SmallButton("Remove Goal")) remove = gi;
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (remove >= 0) {
                host.PushUndoSnapshot();
                ik->Goals.erase(ik->Goals.begin() + remove);
            }
            if (ImGui::Button("Add Goal")) ik->Goals.emplace_back();
            ImGui::SameLine();
            if (ImGui::Button("Remove IK")) {
                host.PushUndoSnapshot();
                reg.remove<IKComponent>(obj.Entity());
            }
        }
    }

    // --- Эмиттер частиц (огонь/дым/искры/…): пресеты + тонкая настройка ---
    if (reg.all_of<ParticleEmitterComponent>(obj.Entity()) && ImGui::CollapsingHeader("Particle Emitter", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ParticleEmitterComponent* em = reg.try_get<ParticleEmitterComponent>(obj.Entity())) {
            ParticleEmitterConfig& cfg = em->Config;
            // Пресеты: применяют готовый конфиг, дальше его можно править.
            const auto& presets = ParticlePresets::Registry();
            std::string preview = (em->Preset >= 0 && em->Preset < (int)presets.size())
                                      ? presets[em->Preset].Name : "Custom";
            if (ImGui::BeginCombo("Preset", preview.c_str())) {
                for (int i = 0; i < (int)presets.size(); ++i) {
                    if (ImGui::Selectable(presets[i].Name, em->Preset == i)) {
                        host.PushUndoSnapshot();
                        em->Preset = i;
                        cfg = presets[i].Make();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Checkbox("Active", &em->Active);
            ImGui::SameLine();
            ImGui::Checkbox("Continuous", &em->Continuous);
            if (em->Continuous) {
                ImGui::DragFloat("Rate (p/s)", &cfg.EmissionRate, 0.5f, 0.0f, 500.0f); host.TrackLastImGuiItem();
            } else {
                ImGui::DragInt("Burst Count", &em->BurstCount, 1, 1, 500); host.TrackLastImGuiItem();
                ImGui::DragFloat("Burst Interval", &em->BurstInterval, 0.05f, 0.05f, 30.0f); host.TrackLastImGuiItem();
            }
            ImGui::DragFloatRange2("Speed", &cfg.SpeedMin, &cfg.SpeedMax, 0.05f, 0.0f, 50.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat("Gravity", &cfg.Gravity, 0.05f, -30.0f, 30.0f); host.TrackLastImGuiItem();
            ImGui::DragFloatRange2("Lifetime", &cfg.LifetimeMin, &cfg.LifetimeMax, 0.02f, 0.02f, 20.0f); host.TrackLastImGuiItem();
            ImGui::DragFloatRange2("Start Size", &cfg.StartSizeMin, &cfg.StartSizeMax, 0.005f, 0.0f, 5.0f); host.TrackLastImGuiItem();
            ImGui::DragFloatRange2("End Size", &cfg.EndSizeMin, &cfg.EndSizeMax, 0.005f, 0.0f, 5.0f); host.TrackLastImGuiItem();
            ImGui::ColorEdit4("Start Color", &cfg.StartColor.x); host.TrackLastImGuiItem();
            ImGui::ColorEdit4("End Color", &cfg.EndColor.x); host.TrackLastImGuiItem();
            ImGui::DragFloat3("Dir Min", &cfg.DirectionMin.x, 0.02f); host.TrackLastImGuiItem();
            ImGui::DragFloat3("Dir Max", &cfg.DirectionMax.x, 0.02f); host.TrackLastImGuiItem();
            ImGui::DragFloat("Spin", &cfg.AngularVelocityMax, 0.05f, 0.0f, 20.0f); host.TrackLastImGuiItem();
            if (ImGui::Button("Remove Emitter")) {
                host.PushUndoSnapshot();
                reg.remove<ParticleEmitterComponent>(obj.Entity());
            }
        }
    }

    if (reg.all_of<UIElementComponent>(obj.Entity()) && ImGui::CollapsingHeader("UI Element", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (UIElementComponent* u = reg.try_get<UIElementComponent>(obj.Entity())) {
            const char* kinds[] = {"Panel", "Label", "Image", "Bar", "Icon",
                                   "Input", "Checkbox", "Slider"};
            int kind = (int)u->Type;
            if (ImGui::Combo("Kind", &kind, kinds, IM_ARRAYSIZE(kinds))) {
                host.PushUndoSnapshot();
                u->Type = (UIElementComponent::Kind)kind;
            }
            const char* anchors[] = {"Top Left", "Top Center", "Top Right",
                                     "Center Left", "Center", "Center Right",
                                     "Bottom Left", "Bottom Center", "Bottom Right"};
            int anchor = (int)u->Anchor;
            if (ImGui::Combo("Anchor", &anchor, anchors, IM_ARRAYSIZE(anchors))) {
                host.PushUndoSnapshot();
                u->Anchor = (UIAnchor)anchor;
            }
            ImGui::DragFloat2("Offset", &u->Offset.x, 1.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat2("Size", &u->Size.x, 1.0f, 0.0f, 4096.0f); host.TrackLastImGuiItem();
            ImGui::DragInt("Layer", &u->Layer, 1); host.TrackLastImGuiItem();
            ImGui::Checkbox("Visible", &u->Visible);
            ImGui::SameLine();
            ImGui::Checkbox("Clip Children", &u->ClipChildren);
            ImGui::SeparatorText("Style");
            ImGui::ColorEdit4("Fill / Tint", &u->Color.x); host.TrackLastImGuiItem();
            ImGui::DragFloat("Rounding", &u->Rounding, 0.5f, 0.0f, 200.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat("Border", &u->BorderThickness, 0.25f, 0.0f, 50.0f); host.TrackLastImGuiItem();
            if (u->BorderThickness > 0.0f) {
                ImGui::ColorEdit4("Border Color", &u->BorderColor.x); host.TrackLastImGuiItem();
            }
            ImGui::ColorEdit4("Gradient (a=0 off)", &u->GradientColor.x); host.TrackLastImGuiItem();
            ImGui::DragFloat("Shadow", &u->ShadowSize, 0.5f, 0.0f, 64.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat("Padding X", &u->PadX, 0.5f, 0.0f, 64.0f); host.TrackLastImGuiItem();
            ImGui::Checkbox("Auto Width", &u->AutoWidth);

            // Иконка выбирается из списка движка, а не вводится строкой: имя с
            // опечаткой рисует заглушку, и искать её потом дороже, чем показать
            // здесь готовый перечень.
            ImGui::SeparatorText("Icon");
            const std::vector<std::string>& icons = sage::ui::IconNames();
            std::string current = u->Icon.empty() ? "(none)" : u->Icon;
            if (ImGui::BeginCombo("Icon", current.c_str())) {
                if (ImGui::Selectable("(none)", u->Icon.empty())) {
                    host.PushUndoSnapshot();
                    u->Icon.clear();
                }
                for (const std::string& name : icons) {
                    if (ImGui::Selectable(name.c_str(), name == u->Icon)) {
                        host.PushUndoSnapshot();
                        u->Icon = name;
                    }
                }
                ImGui::EndCombo();
            }
            if (!u->Icon.empty()) {
                ImGui::ColorEdit4("Icon Color", &u->IconColor.x); host.TrackLastImGuiItem();
            }
            ImGui::SeparatorText("Text");
            char textBuf[256];
            std::snprintf(textBuf, sizeof(textBuf), "%s", u->Text.c_str());
            if (ImGui::InputText("Text", textBuf, sizeof(textBuf))) u->Text = textBuf;
            host.TrackLastImGuiItem();
            ImGui::DragFloat("Text Scale", &u->TextScale, 0.05f, 0.5f, 12.0f); host.TrackLastImGuiItem();
            ImGui::ColorEdit4("Text Color", &u->TextColor.x); host.TrackLastImGuiItem();
            ImGui::Checkbox("Center Text", &u->TextCentered);
            ImGui::SameLine();
            ImGui::Checkbox("Wrap", &u->WrapText);

            ImGui::SeparatorText("Interaction");
            ImGui::Checkbox("Interactive", &u->Interactive);
            ImGui::SameLine();
            ImGui::Checkbox("Enabled", &u->Enabled);
            if (u->Type == UIElementComponent::Kind::Input) {
                char phBuf[256];
                std::snprintf(phBuf, sizeof(phBuf), "%s", u->Placeholder.c_str());
                if (ImGui::InputText("Placeholder", phBuf, sizeof(phBuf))) u->Placeholder = phBuf;
                host.TrackLastImGuiItem();
                ImGui::DragInt("Max Length", &u->MaxLength, 1, 0, 4096);
                host.TrackLastImGuiItem();
                ImGui::Checkbox("Password", &u->Password);
            }
            if (u->Type == UIElementComponent::Kind::Slider) {
                ImGui::DragFloat("Min Value", &u->MinValue, 0.1f); host.TrackLastImGuiItem();
                ImGui::DragFloat("Max Value", &u->MaxValue, 0.1f); host.TrackLastImGuiItem();
            }
            if (u->Type == UIElementComponent::Kind::Slider ||
                u->Type == UIElementComponent::Kind::Checkbox) {
                ImGui::SliderFloat("Value (0..1)", &u->Value, 0.0f, 1.0f);
                host.TrackLastImGuiItem();
            }
            if (u->Type == UIElementComponent::Kind::Image) {
                ImGui::SeparatorText("Image");
                char texBuf[512];
                std::snprintf(texBuf, sizeof(texBuf), "%s", u->TexturePath.c_str());
                if (ImGui::InputText("Texture", texBuf, sizeof(texBuf))) u->TexturePath = texBuf;
                host.TrackLastImGuiItem();
                ImGui::SameLine();
                auto loadTexture = [&] {
                    u->Tex = u->TexturePath.empty()
                                 ? nullptr
                                 : u->PixelArt ? ResourceManager::Instance().GetTexture(
                                                     u->TexturePath, TextureFilter::Nearest, false)
                                               : ResourceManager::Instance().GetTexture(
                                                     u->TexturePath);
                };
                if (ImGui::SmallButton("Load")) {
                    host.PushUndoSnapshot();
                    loadTexture();
                }
                if (!u->TexturePath.empty() && !u->Tex)
                    ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Texture not loaded (press Load)");

                // Пиксель-арт меняет ФИЛЬТРАЦИЮ загруженной текстуры, поэтому
                // перезагружаем сразу: иначе галка стоит, а картинка мыльная до
                // следующего Load, и это выглядит как «галка не работает».
                if (ImGui::Checkbox("Pixel Art (nearest, no mips)", &u->PixelArt)) {
                    host.PushUndoSnapshot();
                    loadTexture();
                }
                if (u->Tex) {
                    ImGui::TextDisabled("Sheet: %d x %d px", u->Tex->Width(), u->Tex->Height());
                }
                // Спрайт задаётся в ПИКСЕЛЯХ листа — теми же числами, что
                // напечатаны в документации набора.
                ImGui::DragFloat4("Sprite x,y,w,h", &u->Sprite.x, 1.0f, 0.0f, 8192.0f);
                host.TrackLastImGuiItem();
                ImGui::TextDisabled("w or h = 0 — the whole file");
                ImGui::DragFloat4("9-slice l,t,r,b", &u->SliceBorder.x, 0.5f, 0.0f, 512.0f);
                host.TrackLastImGuiItem();
                ImGui::DragFloat("Pixel Scale", &u->PixelScale, 0.25f, 0.0f, 16.0f);
                host.TrackLastImGuiItem();
                ImGui::TextDisabled("0 — stretch to fit; >0 — source pixel size");
                // Спрайты состояний: в наборах кнопка нарисована трижды, и
                // подменить картинку правильнее, чем осветлить основную.
                ImGui::DragFloat4("Hover sprite", &u->SpriteHover.x, 1.0f, 0.0f, 8192.0f);
                host.TrackLastImGuiItem();
                ImGui::DragFloat4("Pressed sprite", &u->SpritePressed.x, 1.0f, 0.0f, 8192.0f);
                host.TrackLastImGuiItem();
            }
            if (u->Type == UIElementComponent::Kind::Bar) {
                ImGui::SeparatorText("Bar");
                ImGui::SliderFloat("Value", &u->Value, 0.0f, 1.0f); host.TrackLastImGuiItem();
                ImGui::ColorEdit4("Fill Color", &u->BarFillColor.x); host.TrackLastImGuiItem();
            }
            if (ImGui::Button("Remove UI Element")) {
                host.PushUndoSnapshot();
                reg.remove<UIElementComponent>(obj.Entity());
            }
        }
    }

    // --- Единое «Add Component»: добавляет любой ОТСУТСТВУЮЩИЙ компонент ---
    DrawAddComponentMenu(host, obj);

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
    if (ImGui::Button("Delete Entity", ImVec2(-1, 0))) host.DeleteSelected();
    ImGui::PopStyleColor();
}

// Кнопка «Add Component» + попап со списком компонентов, которых у сущности ещё
// нет. Так добавление унифицировано (а удаление — кнопкой Remove в самой секции).
void InspectorPanel::DrawAddComponentMenu(EditorHost& host, GameObject obj) {
    entt::registry& reg = host.CurrentScene().Registry();
    entt::entity e = obj.Entity();

    ImGui::Separator();
    if (ImGui::Button("Add Component", ImVec2(-1, 0))) ImGui::OpenPopup("##add_component");
    if (ImGui::BeginPopup("##add_component")) {
        bool any = false;
        auto item = [&](const char* label, bool present, auto addFn) {
            if (present) return;
            any = true;
            if (ImGui::MenuItem(label)) {
                host.PushUndoSnapshot();
                addFn();
                ImGui::CloseCurrentPopup();
            }
        };
        item("GI Static", reg.all_of<GIStaticComponent>(e), [&] { reg.emplace<GIStaticComponent>(e); });
        item("Camera", reg.all_of<CameraComponent>(e), [&] { reg.emplace<CameraComponent>(e); });
        item("Light", reg.all_of<LightComponent>(e), [&] { reg.emplace<LightComponent>(e); });
        item("Script", reg.all_of<ScriptComponent>(e),
             [&] { reg.emplace<ScriptComponent>(e, ScriptComponent{"assets/scripts/spin.lua"}); });
        item("Rigid Body", reg.all_of<RigidBodyComponent>(e), [&] { reg.emplace<RigidBodyComponent>(e); });
        item("Collider", reg.all_of<ColliderComponent>(e), [&] { reg.emplace<ColliderComponent>(e); });
        item("Joint", reg.all_of<JointComponent>(e), [&] { reg.emplace<JointComponent>(e); });
        item("Animated Model", reg.all_of<AnimatedModelComponent>(e),
             [&] { reg.emplace<AnimatedModelComponent>(e); });
        item("IK", reg.all_of<IKComponent>(e), [&] { reg.emplace<IKComponent>(e); });
        item("Reflection Probe", reg.all_of<ReflectionProbeComponent>(e),
             [&] { reg.emplace<ReflectionProbeComponent>(e); });
        item("Particle Emitter", reg.all_of<ParticleEmitterComponent>(e), [&] {
            ParticleEmitterComponent em;
            em.Config = ParticlePresets::Registry()[0].Make(); // Fire
            reg.emplace<ParticleEmitterComponent>(e, em);
        });
        item("UI Element", reg.all_of<UIElementComponent>(e), [&] { reg.emplace<UIElementComponent>(e); });
        if (!any) ImGui::TextDisabled("All components already added");
        ImGui::EndPopup();
    }
}

void InspectorPanel::Draw(EditorHost& host) {
    // Результат обзора приходит через кадр после нажатия — кладём его в то поле,
    // ради которого диалог открывали.
    if (m_browser.Draw() && m_browseTarget) {
        // AssetRef, а не Result().string(): диалог отдаёт АБСОЛЮТНЫЙ путь, и в
        // таком виде он до сих пор уезжал в сцену — работая в этом редакторе на
        // этой машине и нигде больше (см. Project::AssetRef).
        *m_browseTarget = host.CurrentProject().AssetRef(m_browser.Result());
        m_browseTarget = nullptr;
        if (m_browseIsMesh) {
            m_browseIsMesh = false;
            m_pendingMeshLoad = true;   // грузим в том же кадре, ниже по панели
        }
        if (m_browseIsMaterial) {
            m_browseIsMaterial = false;
            // Материал грузим сразу: без указателя объект остался бы с путём и
            // без вида, и это выглядело бы как «выбрал материал, ничего не произошло».
            if (GameObject sel = host.SelectedObject(); sel.Valid()) {
                MeshRendererComponent& mr = sel.Renderer();
                if (!mr.MaterialPath.empty())
                    mr.MaterialPtr = ResourceManager::Instance().GetMaterial(mr.MaterialPath);
            }
        }
        if (m_browseIsShader) {
            if (std::shared_ptr<Material> m =
                    ResourceManager::Instance().GetMaterial(host.SelectedAssetPath().string())) {
                m->ShaderPtr.reset();
            }
        }
    }

    ImGui::Begin("Inspector");

    // ------------------------------------------------------------------------
    // Две ПРИНЦИПИАЛЬНО разные вещи — в две вкладки, а не в одну простыню.
    //
    // Раньше панель просто складывала одно под другое: сверху редактор
    // материала, выбранного в Assets, снизу свойства выбранной сущности, между
    // ними голая черта. Это два независимых предмета правки — файл на диске и
    // объект в сцене, — и у них даже разная область действия: материал общий
    // для всех, кто им покрашен, а Transform принадлежит одной сущности.
    // Соседство без границы читалось как один список свойств одного объекта, и
    // человек не понимал, что именно он сейчас меняет.
    //
    // Вкладка САМА переключается на то, что человек выбрал последним: щёлкнул
    // по объекту в сцене — открыт объект, щёлкнул по файлу в Assets — открыт
    // ассет. Иначе за разделение пришлось бы платить лишним кликом на каждое
    // переключение, и оно бы только мешало.
    // ------------------------------------------------------------------------
    const AssetKind assetKind = ClassifyAsset(host.SelectedAssetPath());
    const bool hasEntity = host.SelectedObject().Valid();
    const bool hasAsset = assetKind != AssetKind::None;

    // Что выбрали последним. Сравниваем с прошлым кадром: событий выбора панель
    // не получает, а сравнение состояния даёт ровно тот же ответ.
    const int entityId = hasEntity ? host.SelectedObject().Id() : -1;
    const std::string assetPath = host.SelectedAssetPath().string();
    if (entityId != m_lastEntityId && hasEntity) { m_focus = Focus::Object; m_forceFocus = true; }
    if (assetPath != m_lastAssetPath && hasAsset) { m_focus = Focus::Asset; m_forceFocus = true; }
    m_lastEntityId = entityId;
    m_lastAssetPath = assetPath;

    if (!hasEntity && !hasAsset) {
        ImGui::TextDisabled("Ничего не выбрано");
        ImGui::Spacing();
        // TextWrapped, а не две строки текста: панель узкая и её ширину меняют,
        // а обрезанная посередине подсказка бесполезнее отсутствующей.
        ImGui::TextWrapped("Выберите объект во вьюпорте или в Hierarchy, либо файл в Assets.");
        ImGui::End();
        return;
    }

    // Одна сущность выбрана — вкладки не нужны: они бы только съедали строку.
    if (hasEntity && !hasAsset) {
        DrawObjectSection(host);
    } else if (!hasEntity && hasAsset) {
        DrawAssetSection(host, assetKind);
    } else if (ImGui::BeginTabBar("##inspector_tabs", ImGuiTabBarFlags_None)) {
        // SetSelected ставится РОВНО НА ОДИН КАДР — тот, в котором сменился
        // выбор. Передавать его каждый кадр, пока m_focus равен вкладке, нельзя:
        // человек щёлкает по «Ассету», ImGui его открывает, а на следующем кадре
        // флаг у «Объекта» всё ещё выставлен и утаскивает выбор обратно. Вкладка
        // выглядела намертво залипшей — ровно так эта панель и сломалась.
        const bool force = m_forceFocus;
        m_forceFocus = false;
        const ImGuiTabItemFlags objFlags = (force && m_focus == Focus::Object)
                                               ? ImGuiTabItemFlags_SetSelected
                                               : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem("Объект", nullptr, objFlags)) {
            m_focus = Focus::Object;
            DrawObjectSection(host);
            ImGui::EndTabItem();
        }
        const ImGuiTabItemFlags assetFlags = (force && m_focus == Focus::Asset)
                                                 ? ImGuiTabItemFlags_SetSelected
                                                 : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem("Ассет", nullptr, assetFlags)) {
            m_focus = Focus::Asset;
            DrawAssetSection(host, assetKind);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

// Заголовок раздела: что именно правится. Без него вкладка «Ассет» с полями
// Albedo/Metallic ничем не отличается от материала, назначенного объекту, —
// а это разные вещи: здесь правится ФАЙЛ, общий для всех, кто им покрашен.
void InspectorPanel::DrawSectionHeader(const char* icon, const char* kind, const std::string& name,
                                       const std::string& subtitle) {
    ImGui::Spacing();
    const float s = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    EditorIcons::Overlay(p.x, p.y + s * 0.15f, s, icon, glm::vec3(0.62f, 0.72f, 0.85f));
    ImGui::Dummy(ImVec2(s * 1.35f, s));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextUnformatted(name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", kind);
    if (!subtitle.empty()) {
        ImGui::TextDisabled("%s", subtitle.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", subtitle.c_str());
    }
    ImGui::Separator();
    ImGui::Spacing();
}

InspectorPanel::AssetKind InspectorPanel::ClassifyAsset(const std::filesystem::path& path) {
    if (path.empty()) return AssetKind::None;
    const std::string ext = path.extension().string();
    if (ext == ".sagemat") return AssetKind::Material;
    if (ext == ".sageprefab") return AssetKind::Prefab;
    if (ext == ".obj" || ext == ".gltf" || ext == ".glb") return AssetKind::Model;
    return AssetKind::Other;
}

void InspectorPanel::DrawObjectSection(EditorHost& host) {
    GameObject obj = host.SelectedObject();
    DrawSectionHeader("cube", "объект сцены", obj.Name(),
                      "Свойства этой сущности — только её.");

    // Мультивыделение: правим первичную, но подсказываем размер набора
    // (гизмо двигает все; Delete/Duplicate — по всем выбранным).
    if (host.Selection().size() > 1) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Выбрано %zu — правим первичный",
                           host.Selection().size());
        ImGui::Separator();
    }
    DrawEntityProperties(host);
}

void InspectorPanel::DrawAssetSection(EditorHost& host, AssetKind kind) {
    const std::filesystem::path& path = host.SelectedAssetPath();
    const std::string name = path.filename().string();

    switch (kind) {
        case AssetKind::Material:
            DrawSectionHeader("material", "материал", name,
                              "Файл на диске — изменится у ВСЕХ объектов с этим материалом.");
            DrawMaterialEditor(host);
            break;
        case AssetKind::Prefab:
            DrawSectionHeader("cube", "префаб", name,
                              "Заготовка-поддерево: двойной клик в Assets ставит копию в сцену.");
            DrawPrefabPreview(host);
            break;
        case AssetKind::Model:
            DrawSectionHeader("model", "модель", name,
                              "Настройки импорта запекаются в меш при загрузке.");
            DrawModelImportEditor(host);
            break;
        default: {
            // Для остальных типов редактора нет — но пустая вкладка выглядит как
            // поломка, поэтому показываем то, что известно о файле.
            DrawSectionHeader("file", "файл", name, path.string());
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (!ec) ImGui::TextDisabled("Размер: %llu байт", (unsigned long long)size);
            ImGui::Spacing();
            ImGui::TextDisabled("Для этого типа файла редактора нет.");
            ImGui::TextDisabled("Материалы (.sagemat) и модели (.obj/.gltf/.glb)");
            ImGui::TextDisabled("правятся здесь же.");
            break;
        }
    }
}
