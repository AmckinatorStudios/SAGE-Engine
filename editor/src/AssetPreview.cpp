#include "AssetPreview.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <system_error>
#include <unordered_map>

#include "sage/render/ModelMaterial.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/AlphaBleed.h"
#include "sage/render/ModelLoader.h"
#include "sage/render/Texture.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/core/Application.h"
#include "sage/ecs/RenderBatch.h"
#include "sage/render/Material.h"
#include "sage/render/Mesh.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ScenePasses.h"
#include "sage/scene/Prefab.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/assets/AssetDatabase.h"

namespace {

// Студийный свет превью — ФИКСИРОВАННЫЙ, а не из текущей сцены.
//
// Так и надо: материал настраивают, чтобы он выглядел правильно ВООБЩЕ, а не
// при том освещении, которое сейчас в уровне. Судить о нём под чужим закатным
// солнцем — значит перекрасить его, а потом обнаружить, что днём он серый.
LightingEnvironment StudioLight() {
    LightingEnvironment env;
    env.Sun.Direction = glm::normalize(glm::vec3(-0.4f, -0.7f, -0.55f));
    env.Sun.Color = glm::vec3(1.0f, 0.97f, 0.92f);
    env.Sun.Intensity = 2.6f;
    env.AmbientStrength = 0.55f;
    // Полусферический ambient: холодное небо сверху, тёплая земля снизу. На
    // плоском сером фоне металл и диэлектрик выглядят почти одинаково — вся
    // разница между ними в том, ЧТО они отражают.
    env.SkyColor = glm::vec3(0.45f, 0.58f, 0.78f);
    env.GroundColor = glm::vec3(0.24f, 0.20f, 0.17f);
    env.Fog.Enabled = false;
    // Небо превью включено: оно и есть то, что отражает металл.
    env.Skybox.Enabled = true;
    env.Skybox.TopColor = glm::vec3(0.20f, 0.34f, 0.62f);
    env.Skybox.HorizonColor = glm::vec3(0.78f, 0.80f, 0.86f);
    env.Skybox.Intensity = 1.0f;
    return env;
}

// Габаритный радиус меша — чтобы вписать модель в кадр независимо от её
// размера. Модель на десять метров и модель на десять сантиметров должны
// смотреться в превью одинаково: превью отвечает на вопрос «как выглядит», а
// не «насколько большая».
float BoundingRadius(const Mesh& mesh) {
    const std::vector<Vertex>* verts = mesh.CpuVertices();
    if (!verts) return 1.0f;   // геометрия только на GPU — считать нечем
    float r = 0.0f;
    for (const Vertex& v : *verts) r = std::max(r, glm::length(v.Position));
    return r > 1e-4f ? r : 1.0f;
}

} // namespace

void AssetPreview::Init() {
    if (m_ready) return;
    m_sphere = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Sphere);
    if (!m_sky) m_sky.emplace();
    // Куб окружения снимается ОДИН раз: небо превью не меняется, и пересъёмка
    // его каждый кадр стоила бы шести проходов там, где нужен ноль.
    m_reflections.SetEnabled(true);
    m_reflections.UpdateSky(*m_sky, StudioLight());
    m_ready = m_sphere != nullptr;
}

// Кэш материалов моделей — НЕ функциональная статика.
//
// Материал держит текстуры, текстура — имя объекта OpenGL, а её деструктор
// зовёт glDeleteTextures. Статика внутри функции умирает в самом конце, на
// выходе из программы: контекста к этому моменту уже нет, и удаление уходит в
// адрес, которого не существует. Редактор именно так и падал — молча, БЕЗ
// отчёта о падении, потому что валился уже после main, при разборе глобальных
// объектов. Снаружи это выглядело так: закрыл редактор с моделью в сцене —
// «программа завершилась некорректно».
//
// Кэш живёт здесь, а чистит его Shutdown (см. ниже) — то есть пока контекст
// ещё жив и удалять текстуры есть чем.
namespace {
std::unordered_map<std::string, std::vector<std::shared_ptr<Material>>>& ModelMaterialCache() {
    static std::unordered_map<std::string, std::vector<std::shared_ptr<Material>>> cache;
    return cache;
}
} // namespace

void AssetPreview::Shutdown() {
    // Первым делом — материалы моделей: в них текстуры, и отпустить их надо
    // при живом контексте (см. ModelMaterialCache).
    ModelMaterialCache().clear();
    m_modelJobs.clear();   // дождаться фоновых разборов: их результат уже некуда класть
    m_fbo.reset();
    m_targets.clear();
    m_sphere.reset();
    m_sky.reset();
    m_reflections = sage::render::ReflectionSystem{};
    m_ready = false;
}

void AssetPreview::Orbit(float dYaw, float dPitch) {
    m_yaw += dYaw;
    m_pitch = std::clamp(m_pitch + dPitch, -85.0f, 85.0f);
}

void AssetPreview::Zoom(float delta) {
    m_distance = std::clamp(m_distance * (delta > 0 ? 0.88f : 1.14f), 1.2f, 12.0f);
}

void AssetPreview::ResetView() {
    m_yaw = 35.0f;
    m_pitch = 20.0f;
    m_distance = 3.0f;
}

uint64_t AssetPreview::RenderMaterial(const std::shared_ptr<Material>& material, int size,
                                      const std::string& key) {
    Init();
    if (!m_sphere) return 0;
    return Render(m_sphere, {material}, size, 1.0f, key);
}

uint64_t AssetPreview::RenderMesh(const std::shared_ptr<Mesh>& mesh, int size,
                                  const std::string& key,
                                  const std::vector<std::shared_ptr<Material>>& materials) {
    Init();
    if (!mesh) return 0;
    return Render(mesh, materials, size, BoundingRadius(*mesh), key);
}

const std::vector<std::shared_ptr<Material>>& AssetPreview::MaterialsForModel(
    const std::string& path) {
    std::unordered_map<std::string, std::vector<std::shared_ptr<Material>>>& cache =
        ModelMaterialCache();
    // ПЕРЕЧИТАЛИ МОДЕЛЬ — РАЗБОР УСТАРЕЛ. Материалы вынуты из файла один раз и
    // лежат здесь по пути; переэкспортировав модель из 3D-редактора с другими
    // картами, человек видел бы в превью прежние. Сверяться с поколением
    // ресурсов дешевле, чем держать здесь свои времена правки (см.
    // ResourceManager::AssetsGeneration).
    static uint64_t cachedGen = 0;
    const uint64_t gen = ResourceManager::Instance().AssetsGeneration();
    if (gen != cachedGen) {
        cachedGen = gen;
        cache.clear();
    }
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;

    // Картинки, встроенные в .glb, ExtractMaterials распаковывает в файлы. Класть
    // их РЯДОМ С МОДЕЛЬЮ нельзя: обложка — действие пассивное, человек просто
    // открыл папку, а движок насорил бы в ней png-шками (и в чужой папке, куда
    // писать вообще не звали). Поэтому у превью свой временный каталог.
    std::error_code ec;
    const std::filesystem::path cacheDir =
        std::filesystem::temp_directory_path() / "sage_preview_textures";
    std::filesystem::create_directories(cacheDir, ec);

    std::vector<std::shared_ptr<Material>> materials;
    for (const ModelLoader::ExtractedMaterial& extracted :
         ModelLoader::ExtractMaterials(path, cacheDir.string()).Materials) {
        // Порядок сохраняется даже для непрочитавшихся материалов (nullptr):
        // индекс здесь — индекс из разметки меша, и сжать список значило бы
        // покрасить части модели чужими материалами.
        if (!extracted.Found) { materials.push_back(nullptr); continue; }
        auto material = std::make_shared<Material>();
        material->Albedo = extracted.Albedo;
        material->Emissive = extracted.Emissive;
        material->EmissiveStrength = extracted.EmissiveStrength;
        material->Metallic = extracted.Metallic;
        material->Roughness = extracted.Roughness;
        material->Opacity = extracted.Opacity;
        material->TexturePath = extracted.AlbedoMap;
        material->NormalMapPath = extracted.NormalMap;
        material->MetallicMapPath = extracted.MetallicMap;
        material->RoughnessMapPath = extracted.RoughnessMap;
        material->AOMapPath = extracted.AOMap;
        material->EmissiveMap = extracted.EmissiveMap;
        // Вырез и двусторонность — как в сцене (ModelMaterialImport.cpp):
        // иначе листва на превью квадратная, хотя в сцене вырезана.
        if (extracted.AlphaMode == 1) material->Render.AlphaCutoff = extracted.AlphaCutoff;
        if (extracted.DoubleSided) material->Render.Cull = CullFaces::None;
        if (extracted.AlphaMode == 1 && extracted.DoubleSided) material->Render.Translucency = 0.5f;
        material->Render.Unlit = extracted.Unlit;
        ResourceManager::Instance().ResolveMaterialTextures(*material);
        materials.push_back(std::move(material));
    }
    // И пустой ответ кэшируется: иначе файл разбирался бы каждый кадр.
    return cache.emplace(path, std::move(materials)).first->second;
}

// --- Обложка модели фоном ----------------------------------------------------

namespace {

// Всё, что нужно для обложки, собранное БЕЗ видеокарты.
struct ModelCoverData {
    bool Ok = false;
    sage::render::MeshData Mesh;
    struct Mat {
        bool Found = false;
        glm::vec3 Albedo{1.0f}, Emissive{0.0f};
        float Metallic = 0.0f, Roughness = 0.5f, Opacity = 1.0f;
        float AlphaCutoff = 0.0f;
        bool DoubleSided = false;
        std::vector<unsigned char> Pixels;   // альбедо, уменьшенное под обложку
        int W = 0, H = 0;
    };
    std::vector<Mat> Materials;
};

// Сторона карты на обложке: кадр обложки 96-192 пикселя, и карта крупнее
// этого на снимке неотличима — а в памяти и в декоде стоит в сотни раз дороже.
constexpr int kCoverMapSide = 128;

ModelCoverData LoadModelCoverData(const std::string& file, const std::string& scratchDir) {
    ModelCoverData out;
    try {
        out.Mesh = ModelLoader::LoadMeshData(file);
    } catch (const std::exception&) {
        return out;   // причину покажет обычная загрузка, если модель поставят в сцену
    }
    out.Ok = !out.Mesh.Empty();
    for (const ModelLoader::ExtractedMaterial& ex :
         ModelLoader::ExtractMaterials(file, scratchDir).Materials) {
        ModelCoverData::Mat m;
        m.Found = ex.Found;
        m.Albedo = ex.Albedo;
        m.Emissive = ex.Emissive * ex.EmissiveStrength;
        m.Metallic = ex.Metallic;
        m.Roughness = ex.Roughness;
        m.Opacity = ex.Opacity;
        m.AlphaCutoff = ex.AlphaMode == 1 ? ex.AlphaCutoff : 0.0f;
        m.DoubleSided = ex.DoubleSided;
        if (!ex.AlbedoMap.empty() &&
            ResourceManager::DecodeImageFile(ex.AlbedoMap, m.Pixels, m.W, m.H)) {
            if (m.AlphaCutoff > 0.0f) sage::render::BleedTransparentColor(m.Pixels, m.W, m.H);
            std::vector<unsigned char> half;
            while (std::max(m.W, m.H) > kCoverMapSide) {
                ResourceManager::DownscaleRGBA(m.Pixels, m.W, m.H, half, m.W, m.H);
                m.Pixels.swap(half);
            }
        }
        out.Materials.push_back(std::move(m));
    }
    return out;
}

} // namespace

struct AssetPreview::ModelCoverJob {
    std::future<ModelCoverData> Future;
};

uint64_t AssetPreview::RenderModelCover(const std::string& path, int size, const std::string& key,
                                        bool& pending) {
    pending = false;
    auto it = m_modelJobs.find(path);
    if (it == m_modelJobs.end()) {
        // Не больше двух разборов разом: остальные ядра нужны кадру, а папка
        // набора — это сотни моделей, и все они разом положили бы машину.
        int running = 0;
        for (const auto& kv : m_modelJobs) running += kv.second->Future.valid() ? 1 : 0;
        pending = true;
        if (running >= 2) return 0;
        // Путь, который откроется: разрешает его главный поток (база ассетов
        // живёт здесь), фоновый получает готовую строку.
        const std::string file = sage::AssetDatabase::Instance().LocatePath(path);
        std::error_code ec;
        const std::filesystem::path scratch = std::filesystem::temp_directory_path() /
                                              "sage_preview_textures" /
                                              std::to_string(std::hash<std::string>{}(file));
        std::filesystem::create_directories(scratch, ec);
        auto job = std::make_shared<ModelCoverJob>();
        job->Future = std::async(std::launch::async, LoadModelCoverData, file, scratch.string());
        m_modelJobs.emplace(path, std::move(job));
        return 0;
    }
    ModelCoverJob& job = *it->second;
    if (job.Future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        pending = true;
        return 0;
    }
    ModelCoverData data = job.Future.get();
    m_modelJobs.erase(it);   // разобранное нужно ровно на один снимок
    if (!data.Ok) return 0;

    Init();
    auto mesh = std::make_shared<Mesh>(data.Mesh.Vertices, data.Mesh.Indices, data.Mesh.Submeshes,
                                       /*keepCpuData=*/true);
    std::vector<std::shared_ptr<Material>> materials;
    for (ModelCoverData::Mat& m : data.Materials) {
        if (!m.Found) { materials.push_back(nullptr); continue; }
        auto material = std::make_shared<Material>();
        material->Albedo = m.Albedo;
        material->Emissive = m.Emissive;
        material->Metallic = m.Metallic;
        material->Roughness = m.Roughness;
        material->Opacity = m.Opacity;
        material->Render.AlphaCutoff = m.AlphaCutoff;
        if (m.DoubleSided) material->Render.Cull = CullFaces::None;
        if (m.AlphaCutoff > 0.0f && m.DoubleSided) material->Render.Translucency = 0.5f;
        if (!m.Pixels.empty() && m.W > 0 && m.H > 0)
            material->AlbedoTex = std::make_shared<Texture>(m.Pixels.data(), m.W, m.H,
                                                            TextureFilter::Trilinear, true,
                                                            /*srgb=*/true);
        materials.push_back(std::move(material));
    }
    return Render(mesh, materials, size, BoundingRadius(*mesh), key);
}

void AssetPreview::ReleaseTarget(const std::string& key) { m_targets.erase(key); }

Framebuffer& AssetPreview::TargetFor(const std::string& key, int size) {
    if (key.empty()) {
        if (!m_fbo) m_fbo.emplace(size, size);
        return *m_fbo;
    }
    auto it = m_targets.find(key);
    if (it == m_targets.end()) it = m_targets.emplace(key, Framebuffer(size, size)).first;
    return it->second;
}

// Смена проекта: все именованные буферы — прочь (см. заголовок).
void AssetPreview::ForgetProject() {
    m_targets.clear();
    // Разборы моделей старого проекта: пути у нового проекта свои, и ответ
    // для «assets/tree.fbx» прошлого проекта лёг бы обложкой чужому файлу.
    m_modelJobs.clear();
}

uint64_t AssetPreview::Render(const std::shared_ptr<Mesh>& mesh,
                              const std::vector<std::shared_ptr<Material>>& materials, int size,
                              float fitRadius, const std::string& key) {
    if (!mesh) return 0;

    // Сцена превью строится заново каждый кадр и живёт один вызов. Это дёшево
    // (одна сущность) и избавляет от целого класса ошибок: превью не может
    // «залипнуть» на прошлом материале, потому что помнить ему нечем.
    Scene scene("preview");
    GameObject obj = scene.CreateObject("Preview");
    MeshRendererComponent& mr = obj.Renderer();
    mr.MeshPtr = mesh;
    mr.Ref.type = MeshRef::Type::Sphere;

    std::shared_ptr<Material> first;
    for (const std::shared_ptr<Material>& m : materials) {
        if (m) { first = m; break; }
    }
    if (first) {
        mr.MaterialPtr = first;
        mr.MaterialPath = "preview";
        // Части модели — по своим материалам: обложка обязана показывать то же,
        // что человек увидит, поставив модель в сцену. Разметки нет (шарик
        // материала, одноматериальная модель) — слоты не нужны, всё красится
        // материалом объекта.
        if (mesh->HasExplicitSubmeshes()) {
            const std::vector<sage::render::Submesh>& subs = mesh->Submeshes();
            mr.Slots.resize(subs.size());
            for (size_t i = 0; i < subs.size(); ++i) {
                const int index = subs[i].Material;
                if (index < 0 || (size_t)index >= materials.size() || !materials[(size_t)index])
                    continue;
                mr.Slots[i].Path = "preview";
                mr.Slots[i].Ptr = materials[(size_t)index];
            }
        }
    } else {
        mr.Color = glm::vec3(0.78f, 0.78f, 0.80f);
    }
    return RenderScene(scene, size, fitRadius, glm::vec3(0.0f), key);
}

uint64_t AssetPreview::RenderPrefab(const std::string& path, int size, const std::string& key) {
    Init();
    if (path.empty()) return 0;

    // Префаб — мини-сцена, и ставится он в превью тем же InstantiatePrefab, что
    // и в настоящую сцену. Отдельный «облегчённый разбор для картинки» означал
    // бы второй загрузчик префабов, который надо держать в согласии с первым, —
    // и обложка расходилась бы с тем, что человек получит, положив префаб в мир.
    Scene scene("prefab-preview");
    if (sage::scene::InstantiatePrefab(scene, path) < 0) return 0;

    // Меши восстанавливает не сериализатор, а ResourceManager по MeshRef —
    // без этого шага превью было бы пустым кадром (сущности есть, рисовать
    // нечего).
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    bool anyGeometry = false;
    scene.Registry().view<MeshRendererComponent>().each(
        [&](entt::entity e, MeshRendererComponent& mr) {
            if (!mr.MeshPtr) {
                if (mr.Ref.type == MeshRef::Type::Model && !mr.Ref.path.empty())
                    mr.MeshPtr = ResourceManager::Instance().GetModel(mr.Ref.path);
                else if (mr.Ref.type != MeshRef::Type::None)
                    mr.MeshPtr = ResourceManager::Instance().GetPrimitive(mr.Ref.type);
            }
            if (!mr.MaterialPtr && !mr.MaterialPath.empty())
                mr.MaterialPtr = ResourceManager::Instance().GetMaterial(mr.MaterialPath);
            if (!mr.MeshPtr) return;

            // Габариты — по МИРОВЫМ углам коробки каждого меша: префаб из
            // нескольких деталей должен влезать в кадр целиком, а его детали
            // стоят не в начале координат.
            const glm::mat4 world = scene.WorldMatrix(e);
            const glm::vec3 bmin = mr.MeshPtr->BoundsMin();
            const glm::vec3 bmax = mr.MeshPtr->BoundsMax();
            for (int corner = 0; corner < 8; ++corner) {
                const glm::vec3 local((corner & 1) ? bmax.x : bmin.x, (corner & 2) ? bmax.y : bmin.y,
                                      (corner & 4) ? bmax.z : bmin.z);
                const glm::vec3 p = glm::vec3(world * glm::vec4(local, 1.0f));
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
            anyGeometry = true;
        });

    if (!anyGeometry) return 0;   // префаб без видимой геометрии — обложке взяться неоткуда

    const glm::vec3 center = (lo + hi) * 0.5f;
    const float radius = std::max(glm::length(hi - lo) * 0.5f, 0.05f);
    return RenderScene(scene, size, radius, center, key);
}

uint64_t AssetPreview::RenderScene(Scene& scene, int size, float fitRadius,
                                   const glm::vec3& focus, const std::string& key) {
    size = std::clamp(size, 32, 1024);
    Framebuffer& target = TargetFor(key, size);
    target.Resize(size, size);
    target.Bind();

    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();
    device.SetClearColor(0.13f, 0.14f, 0.17f, 1.0f);
    device.Clear();

    const float yaw = glm::radians(m_yaw);
    const float pitch = glm::radians(m_pitch);
    const float dist = m_distance * fitRadius;
    const glm::vec3 eye = focus + glm::vec3(dist * std::cos(pitch) * std::sin(yaw),
                                            dist * std::sin(pitch),
                                            dist * std::cos(pitch) * std::cos(yaw));
    const glm::mat4 view = glm::lookAt(eye, focus, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj =
        glm::perspective(glm::radians(35.0f), 1.0f, 0.05f, dist * 8.0f + 10.0f);

    const LightingEnvironment env = StudioLight();
    sage::render::SceneColorInput input;
    input.View = view;
    input.Proj = proj;
    input.ViewPos = eye;
    input.Env = &env;
    // Ни теней, ни отсечения перекрытием: один объект в пустоте, и тень ему не
    // на что отбрасывать. Проход теней здесь стоил бы столько же, сколько само
    // превью, и не давал бы ничего.
    input.Shadows = ShadowBinding();
    input.ShadingMode = 0;
    input.OcclusionCulling = false;
    input.Time = 0.0f;   // анимированные материалы в превью стоят на месте
    input.Reflection = m_reflections.Binding(size, size);

    sage::render::RenderSceneColor(scene, m_batch, input);

    device.BindDefaultFramebuffer();
    return target.NativeColorTexture();
}
