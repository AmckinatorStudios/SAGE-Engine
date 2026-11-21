// ---------------------------------------------------------------------------
// Инспектор — материалы и меши.
//
// Слот меша, слот материала, редактор материала, подматериалы и переопределения
// экземпляра. Всё об одном: ЧЕМ и КАК нарисован объект. Здесь же и самая
// хитрая часть инспектора — правило, по которому правка попадает то в общий
// материал, то в переопределение конкретного объекта.
//
// Часть класса InspectorPanel: объявления остались в InspectorPanel.h, здесь
// только тела. Разбит потому, что дорос до двух тысяч строк, в которых рядом
// лежали редактор материала, якоря интерфейса и список компонентов.
// ---------------------------------------------------------------------------
#include "InspectorFields.h"
#include "InspectorControls.h"
#include "../ResetDefault.h"
#include <cstdarg>
#include "InspectorPanel.h"
#include "../ModelImportDialog.h"
#include "../EditorTheme.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"
#include "ShaderParamsUi.h"

#include "EditorHost.h"
#include "sage/core/Log.h"
#include <algorithm>

#include "AssetSlot.h"
#include "EditorIcons.h"
#include "ModelMaterialImport.h"
#include "Project.h"
#include "sage/render/ResourceManager.h"
#include "sage/assets/import/Importer.h"
#include "sage/render/ModelLoader.h"
#include "sage/render/ModelMaterial.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UIPresets.h"
#include "../Localization.h"
#include "ui/ColorPicker.h"

namespace ifield = sage::editor::ifield;

namespace fs = std::filesystem;

// Редактор материала: правит поля РАЗДЕЛЯЕМОГО экземпляра из кэша
// ResourceManager — все сущности с этим материалом обновляются в вьюпорте
// сразу; Save фиксирует значения на диск, Revert перечитывает файл.
// Слот текстуры: превью + путь + «Обзор…» + «Из Assets» + «Очистить».
// Слот текстуры материала. Вся механика — общий виджет (см. AssetSlot.h): и
// превью, и приём перетаскивания с проверкой типа, и «показать в Assets», и
// очистка. Здесь остаётся только то, что своё у карт материала: подпись, к
// какому каналу карта относится, и перезагрузка текстур материала после правки.
//
// Раньше этот слот был написан отдельно от слотов меша и материала и вёл себя
// иначе: путь правился полем ввода по Enter, бросок файла не того типа молча
// ничего не делал, а «где лежит эта текстура» узнавалось поиском по проекту.
void InspectorPanel::DrawShaderSlot(EditorHost& host, const char* label, std::string& path,
                                    const std::shared_ptr<Material>& material) {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    const assetslot::Result r =
        assetslot::Draw(host, label, assetslot::Kind::Shader, path, &m_preview);
    if (r.Changed) {
        host.PushUndoSnapshot();
        path = r.Path;
        // Программа собрана из ПРЕЖНЕЙ пары файлов: без сброса материал в сцене
        // продолжал бы рисоваться старым шейдером, а слот показывал бы новый.
        if (material) material->ShaderPtr.reset();
    }
    if (r.BrowseRequested) {
        FileBrowser::Config c;
        c.Title = std::string(T("Shader: ")) + label;
        c.Filters = assetslot::Extensions(assetslot::Kind::Shader);
        c.FilterLabel = T("Shaders");
        // Диалог заперт в проекте: ассет выбирается ИЗНУТРИ (см. assetslot::ProjectRoot).
        c.StartDir = c.Root = assetslot::ProjectRoot(host);
        m_browser.Open(c);
        m_browseTarget = &path;
        m_browseIsShader = true;
        m_browseIsMesh = false;
        m_browseIsMaterial = false;
    }
    ImGui::PopID();
}

void InspectorPanel::DrawTextureSlot(EditorHost& host, const char* label, std::string& path,
                                     const std::shared_ptr<Texture>& tex, const char* tooltip,
                                     Material& material) {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);

    const assetslot::Result r =
        assetslot::Draw(host, label, assetslot::Kind::Texture, path, &m_preview);
    if (r.Changed) {
        host.PushUndoSnapshot();
        path = r.Path;
        // Материал держит СВОИ указатели на текстуры: без пересборки картинка в
        // слоте новая, а объект в сцене остаётся со старой. Материал — тот,
        // что правят, а не «выбранный в Assets»: тот же слот стоит и в секции
        // Mesh объекта, где в Assets выбрано что угодно.
        ResourceManager::Instance().ResolveMaterialTextures(material);
    }
    if (r.BrowseRequested) {
        FileBrowser::Config c;
        c.Title = std::string(T("Texture: ")) + label;
        c.Filters = assetslot::Extensions(assetslot::Kind::Texture);
        c.FilterLabel = T("Images");
        // Диалог заперт в проекте: ассет выбирается ИЗНУТРИ (см. assetslot::ProjectRoot).
        c.StartDir = c.Root = assetslot::ProjectRoot(host);
        m_browser.Open(c);
        m_browseTarget = &path;
        m_browseIsShader = false;
        m_browseIsMesh = false;
        m_browseIsMaterial = false;
    }
    // Путь есть, а текстуры нет — файл не прочитался. Слот показывает сам факт,
    // а причина уходит в консоль загрузчиком.
    if (!path.empty() && !tex) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), T("Failed to load: %s"), path.c_str());
    }
    ImGui::PopID();
    ImGui::Spacing();
}

void InspectorPanel::DrawMaterialEditor(EditorHost& host) {
    std::string pathStr = host.InspectedAssetPath().string();
    std::shared_ptr<Material> material = ResourceManager::Instance().GetMaterial(pathStr);

    // Файл мог не прочитаться (удалён, битый JSON) — тогда весь редактор ниже
    // разыменовывал бы nullptr. Выбор испорченного .sagemat в Assets ронял
    // редактор: превью-то от null защищалось, а первое же поле — нет.
    if (!material) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s", T("Material cannot be read"));
        ImGui::TextDisabled("%s", T("The file is gone or damaged — details in the console."));
        return;
    }
    DrawMaterialFields(host, material, pathStr, /*preview=*/true);
}

namespace ictl = sage::editor::ictl;

namespace {
// Поле занимает всё, кроме места под самую длинную подпись редактора материала.
struct MaterialFieldWidth {
    MaterialFieldWidth() {
        float label = 0.0f;
        for (const char* s : {T("Emissive Strength"), T("Roughness"), T("Metallic"), T("Opacity")})
            label = std::max(label, ImGui::CalcTextSize(s).x);
        ImGui::PushItemWidth(-(label + ImGui::GetStyle().ItemInnerSpacing.x * 2.0f));
    }
    ~MaterialFieldWidth() { ImGui::PopItemWidth(); }
};
} // namespace

void InspectorPanel::DrawMaterialFields(EditorHost& host, const std::shared_ptr<Material>& material,
                                        const std::string& pathStr, bool preview) {
    if (!material) return;
    // ШИРИНА ПОЛЕЙ — ОТ САМОЙ ДЛИННОЙ ПОДПИСИ. По умолчанию ImGui отдаёт полю
    // долю окна, а подпись справа получает остаток: в узком инспекторе
    // «Сила свечения» обрезалась, и редактор материала выглядел сжатым.
    MaterialFieldWidth widthScope;
    // Пути карт могли смениться выбором в диалоге «Обзор…» — пересборка карт
    // по слепку путей (шесть сравнений строк, если ничего не менялось).
    ResourceManager::Instance().RefreshMaterialTextures(*material);

    // --- 3D-ПРЕВЬЮ — СВЕРХУ, ВНЕ КАТЕГОРИЙ ------------------------------------
    //
    // Материал настраивают, глядя на него: металл или диэлектрик, гладкий или
    // матовый, как легла карта нормалей. Поэтому превью не сворачивается и
    // занимает всю ширину инспектора, а не квадрат 220 точек сбоку. Кадр —
    // по ширине панели, высота — три четверти от неё (в разумных пределах);
    // проекция строится по его сторонам (AssetPreview::RenderMaterial).
    if (preview) {
        static const MeshRef::Type kShapes[] = {MeshRef::Type::Sphere, MeshRef::Type::Cube,
                                                MeshRef::Type::Plane, MeshRef::Type::Cylinder,
                                                MeshRef::Type::Cone, MeshRef::Type::Capsule};
        m_materialShape = std::clamp(m_materialShape, 0, 5);
        const float w = std::floor(ImGui::GetContentRegionAvail().x);
        const float h = std::floor(std::clamp(w * 0.75f, 160.0f, 420.0f));
        // Буфер — в пикселях экрана, а не в точках интерфейса: на мониторе с
        // масштабом 2 превью иначе было бы мыльным.
        const float scale = std::max(1.0f, ImGui::GetIO().DisplayFramebufferScale.x);
        const uint64_t tex = w > 1.0f ? m_preview.RenderMaterial(material, (int)(w * scale), (int)(h * scale),
                                                                 kShapes[m_materialShape], m_materialFlat)
                                      : 0;
        if (tex) {
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)(std::intptr_t)tex, ImVec2(w, h), ImVec2(0, 1), ImVec2(1, 0));
            // КАРТИНКА + НЕВИДИМАЯ КНОПКА ПОВЕРХ: кнопка захватывает мышь до
            // отпускания, и вращение не обрывается на границе картинки.
            ImGui::SetCursorScreenPos(p0);
            ImGui::InvisibleButton("##preview_drag", ImVec2(w, h));
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                const ImVec2 d = ImGui::GetIO().MouseDelta;
                m_preview.Orbit(d.x * 0.5f, -d.y * 0.5f);
            }
            if (ImGui::IsItemHovered()) {
                // Колесо — превью, пока курсор над ним: иначе его забирала
                // прокрутка инспектора, и приближение срабатывало через раз.
                ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
                const float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f) m_preview.Zoom(wheel);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) m_preview.ResetView();
                ImGui::SetTooltip("%s", T("Drag to turn, wheel to zoom, double-click to reset"));
            }
        }
        // Под картинкой — одна строка: форма, сглаживание формы, сброс вида.
        // Smooth/Flat — это нормали ГЕОМЕТРИИ превью, а не фильтрация текстур
        // (у той свои «Сглаживание/Ближайший пиксель» в разделе Textures).
        const char* shapes[] = {T("Sphere"), T("Cube"), T("Plane"), T("Cylinder"), T("Cone"), T("Capsule")};
        ictl::Combo("##preview_shape", &m_materialShape, shapes, 6, ictl::DropLook::Compact);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Preview shape"));
        ImGui::SameLine();
        ictl::GroupItem shading[2];
        shading[0].Text = T("Smooth");
        shading[0].Tooltip = T("Smooth shading of the preview shape");
        shading[0].Active = !m_materialFlat;
        shading[1].Text = T("Flat");
        shading[1].Tooltip = T("Flat faces of the preview shape");
        shading[1].Active = m_materialFlat;
        const int pick = ictl::ButtonGroup("##preview_shading", shading, 2, ictl::ButtonLook::Standard);
        if (pick >= 0) m_materialFlat = pick == 1;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
        if (EditorIcons::IconOnlyButton("refresh", T("Reset the preview view"))) m_preview.ResetView();
        ImGui::Spacing();
    }

    // Правка видна сразу, а на диск попадает по «Сохранить» (см. drawSave).
    auto drawSave = [&] {
        // Правка видна сразу, а на диск попадает по «Сохранить»: о том, что в
        // файле ещё старое, говорит метка, иначе это узнают, только перезапустив
        // редактор и потеряв полчаса настройки.
        const bool unsaved = material->HasUnsavedChanges();
        if (ImGui::Button(unsaved ? T("Save Material *###SaveMaterial") : T("Save Material###SaveMaterial"))) {
            try {
                // Путь объекта — относительно проекта («assets/x.sagemat»), а
                // редактор живёт в своём каталоге: пишем туда, откуда материал
                // читался (LocatePath), а не в каталог процесса.
                material->SaveToFile(sage::AssetDatabase::Instance().LocatePath(pathStr));
                LOG_INFO("Editor") << "Material saved: " << pathStr;
            } catch (const std::exception& e) {
                LOG_ERROR("Editor") << "Material save failed: " << e.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Revert"))) ResourceManager::Instance().ReloadMaterial(pathStr);
        if (unsaved) {
            ImGui::SameLine();
            ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s", T("unsaved changes"));
        }
    };

    // МАТЕРИАЛ-ШЕЙДЕР — СВОЙ ИНСПЕКТОР. Его поверхность целиком описывает шейдер:
    // альбедо, металличность, карты и повтор текстуры обычного материала он
    // не читает, и показывать их — значит предлагать ручки, которые ни к чему
    // не подключены. Здесь только шейдер, то, что ОН объявил настраиваемым
    // (числа, цвета, текстуры, группы — ShaderParamsUi.h), и состояние
    // отрисовки, которое из шейдера не задать.
    if (material->HasCustomShader()) {
        ictl::SeparatorText(T("Shader"));
        // Шейдер выбирается слотом, как любой ассет (AssetSlot.h).
        DrawShaderSlot(host, T("Vertex"), material->VertexShaderPath, material);
        DrawShaderSlot(host, T("Fragment"), material->FragmentShaderPath, material);
        sage::editor::shaderparamsui::Draw(host, material, [this, &host](std::string& target) {
            FileBrowser::Config c;
            c.Title = T("Shader texture");
            c.Filters = assetslot::Extensions(assetslot::Kind::Texture);
            c.FilterLabel = T("Images");
            c.StartDir = c.Root = assetslot::ProjectRoot(host);
            m_browser.Open(c);
            m_browseTarget = &target;
            m_browseIsShader = m_browseIsMesh = m_browseIsMaterial = false;
        });

        ictl::SeparatorText(T("Drawing"));
        for (const MaterialRenderField& f : MaterialRenderFields()) {
            if (std::string(f.Key) != "cull" || !f.GetEnum || !f.SetEnum) continue;
            int value = f.GetEnum(material->Render);
            if (ictl::Combo(f.Label, &value, f.EnumLabels, std::min(f.EnumCount, 8)))
                f.SetEnum(material->Render, value);
            if (f.Tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.Tooltip);
        }
        ifield::Slider(host, T("uOpacity"), &material->Opacity, 0.0f, 1.0f);
        host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Passed to the shader as uOpacity.\n"
                                      "Below 1 the material is drawn in the transparent pass (blending, back to front)."));
        drawSave();
        return;
    }

    // ПЯТЬ КАТЕГОРИЙ, каждая сворачивается сама по себе, раскрытие
    // запоминается (настройки редактора) и не зависит от выбранного материала.
    // В инспекторе ассета это разделы (BeginSection); внутри секции Mesh
    // объекта — подгруппы (SubSection): разделы в разделе выглядели бы
    // второй панелью.
    auto beginCat = [&](const char* id, const char* title, bool open, const char* icon) {
        return preview ? ictl::BeginSection(id, title, open, icon) : ictl::SubSection(id, title, open);
    };
    auto endCat = [&] {
        if (preview) ictl::EndSection();
        else ictl::EndSubSection();
    };

    // --- MATERIAL: цвет, свечение, металл, шероховатость ---------------------
    if (beginCat("material.cat.material", T("Material"), true, "material")) {
        // BASE COLOR — ТОЛЬКО RGB. Прозрачность у материала одна — Opacity в
        // разделе Transparency; альфа в цветовом поле была вторым входом в то
        // же число, и цвет, выбранный пипеткой, молча менял прозрачность.
        // С картой Base Color Map цвет умножается на неё (белый — карта как
        // есть), без карты он и есть цвет поверхности.
        Sage::UI::ColorField3(T("Base Color"), &material->Albedo.x);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Main colour of the surface. With a Base Color Map it multiplies the map:\n"
                                      "white keeps the map's own colours."));
        sage::editor::ResetField(&host, *material, material->Albedo);
        Sage::UI::ColorField3(T("Emissive Color"), &material->Emissive.x);
        sage::editor::ResetField(&host, *material, material->Emissive);
        // Сила свечения отдельным ползунком, и его предел заметно больше
        // единицы: bloom срабатывает от яркости ВЫШЕ 1, а цвет зажат в 0..1.
        ifield::Float(host, T("Emissive Strength"), &material->EmissiveStrength, 0.05f, 0.0f, 20.0f, "%.2f");
        sage::editor::ResetField(&host, *material, material->EmissiveStrength);
        // Рамка элемента включает подпись: вышла за край — подпись обрезана.
        m_materialLabelsFit = ImGui::GetItemRectMax().x <=
                              ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x + 0.5f;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Emissive multiplier. Values above 1 produce a bloom halo."));
        ifield::Slider(host, T("Metallic"), &material->Metallic, 0.0f, 1.0f);
        sage::editor::ResetField(&host, *material, material->Metallic);
        ifield::Slider(host, T("Roughness"), &material->Roughness, 0.0f, 1.0f);
        sage::editor::ResetField(&host, *material, material->Roughness);
        endCat();
    }

    // --- TEXTURES: карты и их фильтрация -------------------------------------
    //
    // Подписи — по тому, ЧТО карта задаёт: «Base Color Map» рядом с полем
    // «Base Color», а не два разных «Albedo». Имена полей файла и API те же.
    if (beginCat("material.cat.textures", T("Textures"), true, "texture")) {
        DrawTextureSlot(host, T("Base Color Map"), material->TexturePath, material->AlbedoTex,
                        T("Image of the surface. Multiplied by Base Color."), *material);
        DrawTextureSlot(host, T("Normal Map"), material->NormalMapPath, material->NormalTex,
                        T("Normal map, tangent space (OpenGL: green is up)."), *material);
        DrawTextureSlot(host, T("Metallic Map"), material->MetallicMapPath, material->MetallicTex,
                        T("Channel R. Multiplied by Metallic."), *material);
        DrawTextureSlot(host, T("Roughness Map"), material->RoughnessMapPath, material->RoughnessTex,
                        T("Channel R. Multiplied by Roughness."), *material);
        DrawTextureSlot(host, T("Emissive Map"), material->EmissiveMap, material->EmissiveTex,
                        T("Where it glows and where it does not. Multiplied by the emissive colour and strength."),
                        *material);
        DrawTextureSlot(host, T("AO Map"), material->AOMapPath, material->AOTex,
                        T("Ambient occlusion, channel R. Empty means AO = 1."), *material);
        // ФИЛЬТРАЦИЯ — под картами: решают её, глядя на картинку. Подписи и
        // ключи — из таблицы, что пишет файл (MaterialRenderFields).
        for (const MaterialRenderField& f : MaterialRenderFields()) {
            if (std::strcmp(f.Key, "filtering") != 0 || !f.GetEnum || !f.SetEnum) continue;
            int value = f.GetEnum(material->Render);
            if (ictl::Combo(T("Filtering"), &value, f.EnumLabels, f.EnumCount)) f.SetEnum(material->Render, value);
            if (f.Tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.Tooltip);
        }
        endCat();
    }

    // --- TILING: повтор и сдвиг ----------------------------------------------
    //
    // Повтор двигает ТЕКСТУРЫ — без загруженной карты ему нечего двигать, и
    // поля, ни на что не влияющие, выключены (с объяснением). Состояние
    // считается каждый кадр: назначили или убрали карту — раздел ожил сам.
    if (beginCat("material.cat.tiling", T("Tiling"), false, "grid")) {
        const bool anyMap = material->AlbedoTex || material->NormalTex || material->MetallicTex ||
                            material->RoughnessTex || material->EmissiveTex || material->AOTex;
        if (!anyMap) EditorTheme::Hint(T("Assign a texture in Textures: tiling moves texture maps"));
        ImGui::BeginDisabled(!anyMap);
        using Mode = MaterialRender::TilingMode;
        // РЕЖИМ ПЕРВЫМ: от него зависит, что значат числа ниже.
        const char* kModes[] = {T("Uniform"), T("Per axis"), T("Per object size")};
        int mode = (int)material->Render.Tiling;
        if (ictl::Combo(T("Tiling mode"), &mode, kModes, 3)) material->Render.Tiling = (Mode)std::clamp(mode, 0, 2);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", T("Uniform — one number for both axes.\n"
                                      "Per axis — width and height apart (planks, brickwork).\n"
                                      "Per object size — repeats PER METRE: the tile keeps its\n"
                                      "physical size however the object is scaled."));
        // Пределов ползунка нет: повтор берут и сотнями, и долями.
        const float kMin = 0.001f, kMax = 512.0f;
        if (material->Render.Tiling == Mode::Uniform) {
            float uniform = material->Render.UVScaleX;
            if (ifield::Float(host, T("Tiling"), &uniform, 0.05f, kMin, kMax, "%.3f"))
                material->Render.UVScaleX = std::clamp(uniform, kMin, kMax);
        } else {
            float tiling[2] = {material->Render.UVScaleX, material->Render.UVScaleY};
            if (ictl::DragVec(&host, T("Tiling"), tiling, 2, 0.05f, kMin, kMax, "%.3f")) {
                material->Render.UVScaleX = std::clamp(tiling[0], kMin, kMax);
                material->Render.UVScaleY = std::clamp(tiling[1], kMin, kMax);
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("How many times the texture repeats across the UV.\n"
                                      "Without it a picture on a large object is stretched over its whole length."));
        if (material->Render.Tiling == Mode::WorldSize)
            EditorTheme::Hint(T("Repeats per metre of each face: a 4 x 1 x 2 box gets 4 x 1 tiles in front and 2 x 1 on the side."));
        float offset[2] = {material->Render.UVOffsetX, material->Render.UVOffsetY};
        if (ictl::DragVec(&host, T("Offset"), offset, 2, 0.01f, -64.0f, 64.0f, "%.3f")) {
            material->Render.UVOffsetX = std::clamp(offset[0], -64.0f, 64.0f);
            material->Render.UVOffsetY = std::clamp(offset[1], -64.0f, 64.0f);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Shifts the texture in tiles: a seam landing in the middle\n"
                                      "of a wall is cheaper to move than to re-model."));
        ImGui::EndDisabled();
        endCat();
    }

    // Свойства рендера рисуются ПО ТАБЛИЦЕ (MaterialRenderFields): новое
    // свойство попадает в свою категорию само — по MaterialRenderField::Group.
    auto drawRenderFields = [&](MaterialRenderField::Group group) {
        for (const MaterialRenderField& f : MaterialRenderFields()) {
            if (f.In != group) continue;
            if (f.Type == MaterialRenderField::Kind::Enum && f.GetEnum && f.SetEnum) {
                int value = f.GetEnum(material->Render);
                if (ictl::Combo(f.Label, &value, f.EnumLabels, std::min(f.EnumCount, 8)))
                    f.SetEnum(material->Render, value);
            } else if (f.Type == MaterialRenderField::Kind::Bool && f.AsBool) {
                ictl::Check(f.Label, &(material->Render.*f.AsBool));
            } else if (f.AsFloat) {
                ifield::Slider(host, f.Label, &(material->Render.*f.AsFloat), f.Min, f.Max);
            }
            if (f.Tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.Tooltip);
        }
    };

    // --- TRANSPARENCY: непрозрачность — ЕДИНСТВЕННОЕ место, где она правится -
    if (beginCat("material.cat.transparency", T("Transparency"), false, "eye")) {
        ifield::Slider(host, T("Opacity"), &material->Opacity, 0.0f, 1.0f);
        sage::editor::ResetField(&host, *material, material->Opacity);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Below 1 the material is drawn in the transparent pass (blending, back to front)."));
        drawRenderFields(MaterialRenderField::Group::Transparency);
        endCat();
    }

    // --- RENDER: остальное поведение отрисовки -------------------------------
    if (beginCat("material.cat.render", T("Render"), false, "gear")) {
        drawRenderFields(MaterialRenderField::Group::Render);
        endCat();
    }

    drawSave();
}

// Настройки импорта выбранной модели — ТЕ ЖЕ, что в окне импорта
// (ModelImportDialog.h): трансформ, геометрия, материалы, анимация. Пишутся в
// сайдкар .sageimport; «Переимпортировать» перечитывает меш и обновляет все
// экземпляры модели в сцене.
void InspectorPanel::DrawModelImportEditor(EditorHost& host) {
    const std::string path = host.InspectedAssetPath().string();
    // Правки копятся здесь до нажатия кнопки: перечитывать модель на каждое
    // движение ползунка масштаба значило бы разбирать файл десятки раз в секунду.
    static std::string editingPath;
    static ModelLoader::ImportSettings editing;
    static bool overwrite = false;
    if (editingPath != path) {
        editingPath = path;
        editing = ModelLoader::LoadImportSettings(path);
        overwrite = false;
    }
    sage::editor::modelimport::DrawSettings(editing, &overwrite);

    ImGui::Spacing();
    if (ImGui::Button(T("Reimport"))) {
        if (!ModelLoader::SaveImportSettings(path, editing)) {
            host.SetStatusMessage(T("Import settings save failed"));
        } else {
            Project& project = host.CurrentProject();
            if (overwrite) RemoveModelMaterialFiles(path);
            std::shared_ptr<Mesh> mesh = ResourceManager::Instance().ReloadModel(path);
            // Экземпляры держат ссылку ПРОЕКТА («assets/tree.fbx»), а инспектор —
            // настоящий путь: сравнивать надо ссылки. Здесь сравнивались пути
            // как есть, и переимпорт не обновлял ни одного экземпляра в сцене.
            const std::string ref = project.AssetRef(path);
            Scene& scene = host.CurrentScene();
            std::error_code ec;
            int updated = 0;
            auto view = scene.Registry().view<MeshRendererComponent>();
            for (auto e : view) {
                MeshRendererComponent& mr = view.get<MeshRendererComponent>(e);
                if (mr.Ref.type != MeshRef::Type::Model || mr.Ref.path != ref) continue;
                mr.MeshPtr = mesh;
                if (overwrite) {
                    // Слоты, чьих файлов больше нет, освобождаются — импорт
                    // заполнит их заново; назначенное человеком остаётся.
                    for (MaterialSlot& slot : mr.Slots) {
                        if (!slot.Path.empty() &&
                            !fs::exists(sage::AssetDatabase::Instance().LocatePath(slot.Path), ec))
                            slot = MaterialSlot{};
                    }
                    if (!mr.MaterialPath.empty() &&
                        !fs::exists(sage::AssetDatabase::Instance().LocatePath(mr.MaterialPath), ec)) {
                        mr.MaterialPath.clear();
                        mr.MaterialPtr = nullptr;
                    }
                    ImportModelMaterials(project, mr);
                    // Материалы в кэше ресурсов — те же объекты, что держат
                    // другие экземпляры: перечитываем НА МЕСТЕ.
                    for (const MaterialSlot& slot : mr.Slots)
                        if (!slot.Path.empty()) ResourceManager::Instance().ReloadMaterial(slot.Path);
                    if (!mr.MaterialPath.empty())
                        ResourceManager::Instance().ReloadMaterial(mr.MaterialPath);
                }
                ++updated;
            }
            overwrite = false;
            host.SetStatusMessage(std::string(T("Reimported: ")) +
                                  host.InspectedAssetPath().filename().string() + " (" +
                                  std::to_string(updated) + ")");
        }
    }
    EditorTheme::Hint(T("Baked into the mesh on load — affects editor and built game"));
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
    const ModelMaterialImportResult r = ImportModelMaterials(host.CurrentProject(), mr);
    // ОТВЕТ ЕСТЬ ВСЕГДА. Раньше при нуле назначенных и пустом предупреждении
    // функция молча возвращалась: нажал «Загрузить» — не произошло ничего, и
    // понять, разобрал ли движок файл, было неоткуда.
    if (r.Assigned > 0) {
        host.SetStatusMessage(T("Model materials assigned: ") + std::to_string(r.Assigned) +
                              (r.Created > 0
                                   ? T(", files created: ") + std::to_string(r.Created)
                                   : std::string()) +
                              (r.AnyMaps ? T(" (with maps)") : ""));
        LOG_INFO("Editor") << "Материалов модели назначено: " << r.Assigned
                           << ", файлов создано: " << r.Created;
        return;
    }
    if (!r.FirstWarning.empty()) {
        host.SetStatusMessage(T("Model material: ") + r.FirstWarning);
    } else if (r.Parts > 0 && r.FileMaterials == 0) {
        host.SetStatusMessage(T("The model carries no materials — it stays white"));
    } else if (r.Parts > 0) {
        // Слоты уже заполнены руками — импортировать нечего, и это не отказ.
        host.SetStatusMessage(T("All parts already have their own materials"));
    }
}

// --- Mesh Renderer, часть 1: ЧТО рисуем --------------------------------------
void InspectorPanel::DrawMeshSlot(EditorHost& host, entt::entity entity,
                                  MeshRendererComponent& mr, bool animated) {
    entt::registry& reg = host.CurrentScene().Registry();
    // Порядок строго совпадает с MeshRef::Type (индекс комбо = значение enum).
    const char* kinds[] = {T("None"),     T("Cube"), T("Sphere"),  T("Plane"),
                           T("Cylinder"), T("Cone"), T("Capsule"), T("Model")};
    int kind = (int)mr.Ref.type;
    if (sage::editor::ifield::Combo(host, T("Mesh"), &kind, kinds, IM_ARRAYSIZE(kinds))) {
        const MeshRef::Type chosen = (MeshRef::Type)kind;
        if (chosen != MeshRef::Type::Model) {
            // Примитив вместо модели: путь и слоты её частей уходят вместе с ней.
            SetEntityMesh(host.CurrentProject(), reg, entity, chosen, {},
                          ResourceManager::Instance().GetPrimitive(chosen));
            // Другая форма — с её умолчаниями: число секторов капсулы не
            // переезжает в шар.
            mr.Ref.prim = {};
        } else {
            mr.Ref.type = chosen;   // Model — путь задаётся ниже и грузится кнопкой
        }
        // Model — путь задаётся ниже и грузится кнопкой.
    }

    if (mr.Ref.type == MeshRef::Type::Model) {
        // Слот модели — тот же виджет, что у материала и текстур (см.
        // AssetSlot.h). Раньше здесь было поле ввода пути: путь к своей модели
        // надо было ЗНАТЬ и напечатать без опечатки, обложки не было, а бросок
        // файла не той породы молча не делал ничего.
        const assetslot::Result r =
            assetslot::Draw(host, "mesh", assetslot::Kind::Model, mr.Ref.path, &m_preview,
                            T("No model assigned"));
        if (r.Changed) {
            host.PushUndoSnapshot();
            // Сам меш подгружается ниже (m_pendingMeshLoad) — здесь важно, что
            // вместе со сменой модели сбрасываются слоты её частей.
            // Скелет и клипы приезжают уже отсюда: у персонажа, назначенного
            // ЭТИМ слотом, статического меша не будет никогда (его рисует
            // скелетный проход), и ждать загрузки меша значило бы не настроить
            // его вовсе.
            SetEntityMesh(host.CurrentProject(), reg, entity, MeshRef::Type::Model, r.Path,
                          nullptr);
            if (!r.Cleared) m_pendingMeshLoad = true;
        }
        if (r.BrowseRequested) {
            FileBrowser::Config c;
            c.Title = T("Choose a model");
            // Список берётся у РЕЕСТРА импортёров, а не пишется здесь руками.
            // Пока он был жёстким, добавленный в движок формат (или свой, из
            // плагина) в диалог не попадал — файл лежал рядом и не показывался,
            // из чего честно следовал вывод «свою модель загрузить нельзя».
            c.Filters = assetslot::Extensions(assetslot::Kind::Model);
            std::string label = T("Models");
            label += " (";
            for (size_t i = 0; i < c.Filters.size(); ++i) {
                if (i) label += ", ";
                label += "*" + c.Filters[i];
            }
            label += ")";
            c.FilterLabel = label;
            // Диалог заперт в проекте: ассет выбирается ИЗНУТРИ (см. assetslot::ProjectRoot).
            c.StartDir = c.Root = assetslot::ProjectRoot(host);
            m_browser.Open(c);
            m_browseTarget = &mr.Ref.path;
            m_browseIsShader = false;
            m_browseIsMesh = true;
        }

        if (m_pendingMeshLoad) {
            m_pendingMeshLoad = false;
            // Анимированному объекту статическая копия модели не нужна: его
            // рисует скелетный проход, а вторая копия геометрии просто заняла
            // бы видеопамять. Материал модели при этом всё равно подбираем.
            mr.MeshPtr = animated ? nullptr : ResourceManager::Instance().GetModel(mr.Ref.path, mr.Ref.part);
            // GetModel сам логирует причину и отдаёт nullptr — сообщаем об
            // этом ЗДЕСЬ, в панели: строчку в консоли легко не заметить, а
            // «модель не появилась» без объяснения выглядит как поломка
            // редактора.
            if (!mr.MeshPtr && !animated) {
                host.SetStatusMessage(T("The model failed to load: ") + mr.Ref.path +
                                      T(" — details in Console"));
            } else {
                // Материал модели — вместе с моделью, а не «потом руками».
                // Подробности см. в AutoAssignModelMaterial.
                AutoAssignModelMaterial(host, mr);
            }
        }
        if (animated && !mr.Ref.path.empty()) {
            // Не тревога, а объяснение: у анимированного объекта геометрию
            // держит скелетная модель, и пустой статический меш здесь — норма.
            ImGui::TextDisabled("%s", T("Drawn as a skinned model (see Animation)"));
        } else if (!mr.Ref.path.empty() && !mr.MeshPtr) {
            ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", T("Mesh not loaded"));
            ImGui::SameLine();
            if (EditorIcons::Button("refresh", T("Load"))) m_pendingMeshLoad = true;
            if (!ModelLoader::IsSupportedModel(mr.Ref.path)) {
                // Тот же реестр, что и в диалоге: подсказка обязана называть
                // ровно те форматы, которые движок в самом деле откроет.
                std::string list;
                for (const std::string& ext : assetslot::Extensions(assetslot::Kind::Model)) {
                    if (!list.empty()) list += ", ";
                    list += ext;
                }
                ImGui::TextDisabled("%s %s", T("Supported:"), list.c_str());
            }
        }
        // Абсолютный путь работает в редакторе и НЕ работает нигде больше —
        // говорим об этом сразу, а не после сборки игры.
        if (!mr.Ref.path.empty() && std::filesystem::path(mr.Ref.path).is_absolute()) {
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), "%s", T("File outside the project"));
            ImGui::TextDisabled("%s", T("The built game will not find it. Bring it into the project:"));
            ImGui::TextDisabled("%s", T("Assets -> Import..."));
        }
    }

}

// Создать материал для объекта, у которого его ещё нет.
//
// КУДА ПИШЕМ. В ассеты проекта, если проект открыт, — там ему и место, и
// собранная игра его найдёт. Без проекта пишем рядом со сценой; если и сцена
// не сохранена, спрашиваем путь диалогом. Молча отказать («сначала откройте
// проект») здесь нельзя: демо-сцена редактора живёт без проекта, и именно в
// ней человек первым делом пробует покрасить куб.
void InspectorPanel::CreateMaterialForObject(EditorHost& host, MeshRendererComponent& mr) {
    namespace fs = std::filesystem;

    // Имя — по объекту: «Red Cube» -> Red Cube.sagemat. Так в панели ассетов
    // видно, чей это материал, без открытия файла.
    std::string name = "Material";
    if (GameObject sel = host.InspectedObject(); sel.Valid()) name = sel.Name();
    for (char& c : name) {
        if (std::string("/\\:*?\"<>|").find(c) != std::string::npos) c = '_';
    }

    fs::path dir;
    std::error_code ec;
    dir = host.CurrentProject().AssetsDir();

    if (dir.empty()) {
        // Проекта нет — спрашиваем, куда положить. Молча отказать («сначала
        // откройте проект») нельзя: демо-сцена редактора живёт без проекта, и
        // именно в ней человек первым делом пробует покрасить куб.
        FileBrowser::Config c;
        c.Mode = FileBrowser::PickMode::SaveFile;
        c.Title = T("Where to save the material");
        c.Filters = {".sagemat"};
        c.FilterLabel = T("Materials (*.sagemat)");
        c.DefaultName = name + ".sagemat";
        // Материал ПРОЕКТА и сохраняется в проект: положенный в «Документы», он
        // не попадёт ни в сборку игры, ни на другую машину.
        c.StartDir = c.Root = assetslot::ProjectRoot(host);
        m_browser.Open(c);
        m_browseTarget = &mr.MaterialPath;
        m_browseIsShader = false;
        m_browseIsMesh = false;
        m_browseIsMaterial = true;
        m_browseCreateMaterial = true; // после выбора пути файл ещё надо записать
        return;
    }

    fs::create_directories(dir, ec);
    fs::path path = dir / (name + ".sagemat");
    for (int i = 2; fs::exists(path, ec) && i < 1000; ++i) {
        path = dir / (name + " " + std::to_string(i) + ".sagemat");
    }
    WriteMaterialForObject(host, mr, path.string());
}

// Записывает материал по указанному пути, забирая себе цвет объекта.
//
// Цвет — единственное, что у объекта было своего, и потерять его при создании
// материала нельзя: человек покрасил куб красным и нажал «Создать материал»,
// материал обязан быть красным, а не белым по умолчанию.
void InspectorPanel::WriteMaterialForObject(EditorHost& host, MeshRendererComponent& mr,
                                                const std::string& path) {
    Material material;
    material.Albedo = mr.Color;
    material.SaveToFile(path);

    host.PushUndoSnapshot();
    AssignMaterial(mr, host.CurrentProject().AssetRef(path),
                   ResourceManager::Instance().GetMaterial(host.CurrentProject().AssetRef(path)));
    host.SetStatusMessage(std::string(T("Material created: ")) +
                          std::filesystem::path(path).filename().string());
}

// --- Mesh Renderer, часть 2: ЧЕМ красим --------------------------------------
void InspectorPanel::DrawMaterialSlot(EditorHost& host, MeshRendererComponent& mr) {
    sage::editor::ictl::GroupTitle(T("Material"));

    // Общий слот ассета: обложка (материал показывается шариком — по квадратику
    // цвета не отличить металл от диэлектрика и гладкое от матового, то есть
    // ровно то, ради чего материал и назначают), приём перетаскивания с
    // проверкой типа, «показать в Assets», очистка.
    // «Создать» и «Править» — В САМОМ СЛОТЕ (см. assetslot::Actions), а не
    // строкой под ним. Новый материал забирает СЕБЕ текущие поправки объекта
    // (цвет, свечение, прозрачность), а сами поправки возвращаются в
    // нейтраль: иначе объект побелел бы или покрасился дважды. «Править»
    // открывает поля материала прямо здесь — правится тот же общий экземпляр,
    // что рисует сцена, и изменения видны во вьюпорте сразу.
    assetslot::Actions actions;
    actions.Create = [this, &host, &mr]() { CreateMaterialForObject(host, mr); };
    actions.EditActive = !mr.MaterialPath.empty() && m_inlineMaterial == mr.MaterialPath;
    if (mr.MaterialPtr) {
        actions.Edit = [this, &mr, open = actions.EditActive]() {
            m_inlineMaterial = open ? std::string() : mr.MaterialPath;
        };
    }
    const assetslot::Result r =
        assetslot::Draw(host, "material", assetslot::Kind::Material, mr.MaterialPath, &m_preview,
                        T("The look of an object is defined by its material."), &actions);
    if (r.Changed) {
        host.PushUndoSnapshot();
        // Через AssignMaterial: назначение материала возвращает поправки
        // экземпляра в нейтраль (см. RenderComponents.h). Иначе назначенный
        // материал показывался бы умноженным на старый тон объекта.
        AssignMaterial(mr, r.Path,
                       r.Path.empty() ? nullptr
                                      : ResourceManager::Instance().GetMaterial(r.Path));
    }
    if (r.BrowseRequested) {
        FileBrowser::Config c;
        c.Title = T("Choose a material");
        c.Filters = assetslot::Extensions(assetslot::Kind::Material);
        c.FilterLabel = T("Materials (*.sagemat)");
        // Диалог заперт в проекте: ассет выбирается ИЗНУТРИ (см. assetslot::ProjectRoot).
        c.StartDir = c.Root = assetslot::ProjectRoot(host);
        m_browser.Open(c);
        m_browseTarget = &mr.MaterialPath;
        m_browseIsShader = false;
        m_browseIsMesh = false;
        m_browseIsMaterial = true;
    }
    if (!mr.MaterialPath.empty() && !mr.MaterialPtr) {
        ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", T("File cannot be read"));
    }

    // ПРАВИТЬ МАТЕРИАЛ НА МЕСТЕ, не уходя в Assets.
    //
    // Раньше материал правился только после выбора его ФАЙЛА в панели ассетов:
    // выделил объект — видишь слот, хочешь покрутить шероховатость — ищи файл,
    // теряй выделение объекта, возвращайся. Правится тот же общий экземпляр
    // из кэша, что рисует сцена, поэтому изменения видны во вьюпорте сразу.
    if (mr.MaterialPtr && !mr.MaterialPath.empty()) {
        if (mr.MaterialPtr->HasUnsavedChanges())
            ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s", T("unsaved changes"));
        if (m_inlineMaterial == mr.MaterialPath) {
            // Без отступа: он отнимал ширину у и без того узких полей.
            ImGui::PushID("inlineMaterial");
            DrawMaterialFields(host, mr.MaterialPtr, mr.MaterialPath, /*preview=*/false);
            ImGui::PopID();
        }
    }

    DrawSubmeshMaterials(host, mr);
}

// --- Материалы ЧАСТЕЙ модели --------------------------------------------------
//
// Модель из редактора почти никогда не одноматериальна: у
// персонажа отдельно кожа, отдельно ткань, отдельно глаза. Пока сущность
// держала один материал, всё это красилось одним — и выглядело как «текстуры не
// работают», хотя текстуры были на месте: не было места, где сказать, какая
// часть чем красится. Оно здесь.
//
// Слот на КАЖДУЮ часть меша, в порядке разметки. Пустой слот — не дыра: часть
// красится материалом объекта (см. MaterialForSubmesh), поэтому очистка слота
// возвращает часть к общему виду, а не делает её невидимой.
void InspectorPanel::DrawSubmeshMaterials(EditorHost& host, MeshRendererComponent& mr) {
    if (!mr.MeshPtr || !mr.MeshPtr->HasExplicitSubmeshes()) return;
    const std::vector<sage::render::Submesh>& subs = mr.MeshPtr->Submeshes();

    ictl::SeparatorText(T("Model parts"));
    EditorTheme::Hint(T("Each part of the model has its own material. Empty means "
                                "the object material above."));

    // --- «ИМПОРТИРОВАТЬ МАТЕРИАЛЫ МОДЕЛИ» -----------------------------------
    //
    // Импорт запускался ровно в одном месте — когда модель НАЗНАЧАЮТ. Модель,
    // уже стоящая в сцене, переимпортировать было нечем: сцена, сохранённая
    // тогда, когда импорт не работал, открывалась с пустыми слотами навсегда, и
    // единственным способом это исправить было переназначить меш заново.
    //
    // Кнопка показывается всегда, а не только «когда пусто»: набор материалов у
    // модели меняют и снаружи (перекрасили в 3D-редакторе, дописали карту), и
    // «перечитать» — законное желание, а не аварийная мера.
    int filled = 0;
    for (const MaterialSlot& slot : mr.Slots)
        if (!slot.Path.empty()) ++filled;

    if (EditorIcons::Button("import", T("Import model materials"),
                            T("Reads the materials from the model file, creates the missing\n"
                              ".sagemat next to it and fills the empty slots.\n"
                              "Slots you filled yourself are not touched."))) {
        AutoAssignModelMaterial(host, mr);
    }
    if (filled == 0 && !subs.empty()) {
        // Пустые слоты — это не обязательно поломка (модель может и не нести
        // материалов), поэтому не «ошибка», а объяснение с готовым действием.
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.45f, 1.0f), "%s",
                           T("all parts are unpainted"));
    }

    // Слоты держим ровно по числу частей: меш могли переимпортировать, и число
    // частей меняется вместе с ним. Лишние обрезаем, недостающие добавляем —
    // молча, потому что это не решение человека, а следствие правки модели.
    if (mr.Slots.size() != subs.size()) mr.Slots.resize(subs.size());

    for (size_t i = 0; i < subs.size(); ++i) {
        ImGui::PushID((int)i);
        // Подпись — имя части из файла модели: по нему видно, что красим, а по
        // «Slot 3» — нет.
        const std::string label = subs[i].Name.empty()
                                      ? std::string(T("Part ")) + std::to_string(i + 1)
                                      : subs[i].Name;
        ImGui::TextUnformatted(label.c_str());
        const assetslot::Result r =
            assetslot::Draw(host, "submesh_material", assetslot::Kind::Material, mr.Slots[i].Path,
                            &m_preview, T("Painted with the object material."));
        if (r.Changed) {
            host.PushUndoSnapshot();
            mr.Slots[i].Path = r.Path;
            mr.Slots[i].Ptr = r.Path.empty()
                                  ? nullptr
                                  : ResourceManager::Instance().GetMaterial(r.Path);
        }
        if (r.BrowseRequested) {
            FileBrowser::Config c;
            c.Title = T("Choose a material");
            c.Filters = assetslot::Extensions(assetslot::Kind::Material);
            c.FilterLabel = T("Materials (*.sagemat)");
            // Диалог заперт в проекте: ассет выбирается ИЗНУТРИ (см. assetslot::ProjectRoot).
            c.StartDir = c.Root = assetslot::ProjectRoot(host);
            m_browser.Open(c);
            m_browseTarget = &mr.Slots[i].Path;
            m_browseIsShader = false;
            m_browseIsMesh = false;
            m_browseIsMaterial = true;
        }
        if (!mr.Slots[i].Path.empty() && !mr.Slots[i].Ptr) {
            ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", T("File cannot be read"));
        }
        ImGui::PopID();
    }
}
