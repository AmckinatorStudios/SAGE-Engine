#pragma once
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/anim/Skeleton.h"
#include "sage/render/ShadowMap.h"
#include "sage/render/Reflection.h"
#include "sage/render/Texture.h"
#include "sage/rhi/Resources.h"

struct LightingEnvironment; // sage/scene/Light.h — вперёд, чтобы не тянуть весь заголовок

// ---------------------------------------------------------------------------
// SkinnedModel — анимируемая скелетная модель: геометрия с весами костей +
// скелет + набор анимационных клипов. Разделяемый неизменяемый ассет (skeleton
// и клипы одни на всех; покадровое состояние держит Animator у каждой сущности).
//
//   auto model = SkinnedModel::Load("assets/models/hero.glb");
//   Animator anim; anim.SetRig(&model->GetSkeleton(), &model->Clips());
//   anim.Play("Walk");
//   ... каждый кадр: anim.Update(dt); model->Draw(m, v, p, env, anim.BoneMatrices());
//
// ОСВЕЩАЕТСЯ РОВНО ТАК ЖЕ, КАК ОБЫЧНЫЙ МЕШ. Отличие у скина одно — как
// получается позиция и нормаль вершины:
//
//   обычный меш:   позиция -> модельная матрица -> мир -> ОСВЕЩЕНИЕ
//   скелетный:     позиция -> скиннинг костями -> модельная матрица -> мир -> ОСВЕЩЕНИЕ
//
// Поэтому свой здесь только вершинный шейдер. Фрагментная стадия — общая со
// статическим текстурным проходом (sage::render::TexturedPbrFragSource, см.
// render/PbrShader.h), и материал подмеша полный: albedo, нормали,
// металличность, шероховатость, затенение, свечение, прозрачность,
// двусторонность. Отдельная упрощённая модель освещения здесь была, и стоила
// она дорого: персонаж не принимал рассеянный свет (карта металличности не
// читалась, оставался множитель — то есть полуметалл), не имел ни рельефа, ни
// затенения, ни прозрачности, и выглядел бледной пластиковой копией того же
// объекта, поставленного статическим мешем.
// ---------------------------------------------------------------------------
namespace sage::render {

// Вершина скина: геометрия + до 4 влияющих костей (индексы) и их веса.
//
// ПОЛЕЙ СТОЛЬКО ЖЕ, СКОЛЬКО У СТАТИЧЕСКОЙ ВЕРШИНЫ, плюс кости. Так и должно
// быть: скин отличается от обычного меша ровно тем, КАК получается позиция и
// нормаль, а не тем, что он умеет меньше. Пока здесь не было касательной и
// второй развёртки, у скина не могло быть ни карты нормалей, ни лайтмапы — не
// потому, что так решили, а потому, что данные до шейдера не доезжали.
struct SkinnedVertex {
    glm::vec3 Position{0.0f};
    glm::vec3 Normal{0.0f, 1.0f, 0.0f};
    glm::vec2 TexCoords{0.0f};
    glm::vec4 Joints{0.0f};  // индексы костей (как float — читаются в шейдере через int())
    glm::vec4 Weights{0.0f}; // веса (нормируются при загрузке; сумма ~1)
    // Касательная для карты нормалей: xyz — направление вдоль U, w — знак
    // бинормали (±1). Считается при разборе, если её нет в файле.
    glm::vec4 Tangent{1.0f, 0.0f, 0.0f, 1.0f};
    glm::vec2 TexCoords2{0.0f}; // вторая развёртка (лайтмапа/AO), 0 — её нет
};

class SkinnedMesh {
public:
    SkinnedMesh(const std::vector<SkinnedVertex>& vertices, const std::vector<unsigned int>& indices);
    SkinnedMesh(const SkinnedMesh&) = delete;
    SkinnedMesh& operator=(const SkinnedMesh&) = delete;
    void Draw() const;

private:
    std::unique_ptr<sage::rhi::Geometry> m_geometry;
    size_t m_indexCount = 0;
};

// ---------------------------------------------------------------------------
// Блендшейпы (морф-цели). Мимика и всё, что скелетом не выражается: скелет
// двигает жёсткие части, морфы меняют саму форму поверхности.
//
// Хранение — ДЕЛЬТЫ в float-текстуре, а не дополнительные вершинные атрибуты.
// Атрибутов в GL 3.3 шестнадцать, пять уже заняты скиннингом, и на каждую цель
// ушло бы по два (позиция + нормаль): четыре-пять целей — потолок, а лицу их
// нужны десятки. Текстура же ограничена только памятью, и цена выборки платится
// лишь за АКТИВНЫЕ цели: в кадр уходят индексы и веса только тех, чей вес не ноль.
//
// Раскладка: ширина фиксирована (kMorphTexWidth), вершина v цели t лежит в
// строке t*RowsPerTarget + v/W, колонке v%W. Такая свёртка нужна, потому что
// вершин в меше бывает больше, чем максимальная ширина текстуры.
struct MorphData {
    std::unique_ptr<sage::rhi::Texture2D> Positions; // дельты позиций (RGB, float)
    std::unique_ptr<sage::rhi::Texture2D> Normals;   // дельты нормалей (RGB, float)
    int Count = 0;        // сколько целей
    int Width = 0;        // ширина текстуры в вершинах
    int RowsPerTarget = 0;

    bool Valid() const { return Count > 0 && Positions && Normals; }
};

// Материал подмеша скина — ТОТ ЖЕ НАБОР, что у статического меша (см.
// sage/render/Material.h): фактор + карта на каждое свойство.
//
// Раньше здесь были только «текстура, тон, металличность, шероховатость», и
// это было не упрощение, а потеря: модель, у которой металличность лежит В
// КАРТЕ (обычный glTF из Blender или со Sketchfab), получала множитель
// metallicFactor на ВСЮ поверхность. Половинный металл диффузного света почти
// не принимает — отсюда и «персонаж не реагирует на ambient и еле реагирует на
// источники», при том что статический меш с тем же материалом выглядел верно.
struct SkinnedMaterial {
    std::string Name;
    std::shared_ptr<Texture> Albedo;    // nullptr — красит только Tint
    std::shared_ptr<Texture> Normal;
    std::shared_ptr<Texture> MetallicMap;
    std::shared_ptr<Texture> RoughnessMap;
    std::shared_ptr<Texture> AOMap;
    std::shared_ptr<Texture> EmissiveMap;
    // Канал каждой из трёх карт: у glTF это одна текстура, где R — затенение,
    // G — шероховатость, B — металличность (см. uMetallicMask в PbrShader.h).
    glm::vec4 MetallicMask{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 RoughnessMask{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 AOMask{1.0f, 0.0f, 0.0f, 0.0f};

    glm::vec3 Tint{1.0f};
    float Opacity = 1.0f;
    float Metallic = 0.0f;
    float Roughness = 0.6f;
    glm::vec3 Emissive{0.0f};

    // Как обращаться с прозрачностью: glTF alphaMode. Blend — рисуется вторым
    // проходом без записи глубины, Mask — отбрасывает пиксель по порогу.
    enum class Alpha { Opaque, Mask, Blend };
    Alpha Mode = Alpha::Opaque;
    float AlphaCutoff = 0.5f;
    // Двусторонний материал: отсечение задних граней выключается. У сканов и
    // «плоских» деталей (провода, ремни, ткань) без этого пропадает половина
    // поверхности — и выглядит это как дырки в модели.
    bool DoubleSided = false;

    bool Transparent() const { return Mode == Alpha::Blend || Opacity < 0.999f; }
};

struct SkinnedSubMesh {
    std::shared_ptr<SkinnedMesh> Mesh;
    SkinnedMaterial Material;
    MorphData Morphs;        // пусто, если у примитива нет морф-целей
};

class SkinnedModel {
public:
    // Загружает скелетную модель из .gltf/.glb (skins + animations). Бросает при
    // ошибке или если в файле нет скина. Меши без скина игнорируются.
    static std::unique_ptr<SkinnedModel> Load(const std::string& path);

    // Процедурная демонстрация: гибкий сегментированный «щупалец» из нескольких
    // костей со встроенным клипом «Wave» (волна изгиба). Без внешних ассетов —
    // используется для примеров и headless-тестов пайплайна анимации.
    static std::unique_ptr<SkinnedModel> CreateDemoTentacle(int segments = 6);

    const sage::anim::Skeleton& GetSkeleton() const { return m_skeleton; }
    const std::vector<sage::anim::AnimationClip>& Clips() const { return m_clips; }

    // Забрать клипы у ЧУЖОЙ модели, переложив их на свой скелет (ретаргет по
    // именам костей, см. anim/Retarget.h). Возвращает, сколько клипов добавлено.
    //
    // Библиотеки анимаций продаются отдельно от персонажей — в этом их смысл:
    // один набор движений на любого героя. Без переноса такой набор применим
    // ровно к той болванке, с которой поставляется, то есть бесполезен.
    //
    // Клипы кладутся В МОДЕЛЬ, а не в сущность: результат зависит только от
    // исходных клипов и ЦЕЛЕВОГО СКЕЛЕТА — оба принадлежат модели. Держи их
    // сущность, две тысячи NPC хранили бы две тысячи копий одних и тех же
    // ключей.
    int BorrowClipsFrom(const SkinnedModel& source);
    int SubMeshCount() const { return (int)m_subMeshes.size(); }

    // --- Блендшейпы ---
    // Имена морф-целей модели. Их порядок — это и есть порядок весов, которые
    // передаются в Draw: индекс в этом списке = индекс веса.
    const std::vector<std::string>& MorphNames() const { return m_morphNames; }
    int MorphCount() const { return (int)m_morphNames.size(); }
    // Веса по умолчанию из файла (glTF mesh.weights) — стартовое выражение лица,
    // если художник его задал.
    const std::vector<float>& DefaultMorphWeights() const { return m_morphDefaults; }
    // Индекс цели по имени, -1 — нет такой. Имя устойчиво к переэкспорту, номер нет.
    int FindMorph(const std::string& name) const;

    // Рисует все submesh со скиннингом по палитре костей bones (из Animator),
    // с ПОЛНЫМ освещением сцены (ambient/солнце+тени/точечные/прожекторы/туман) —
    // тем же, что и статические меши. shadows — карты теней от солнца
    // (каскады, см. ShadowMap.h), как в статическом lit-проходе. Если bones пуст —
    // bind-поза (единичные кости).
    // morphWeights — веса блендшейпов в порядке MorphNames(); пусто или
    // nullptr — форма как в файле.
    // shadingMode — номер отладочного вида (см. render/DebugView.h), 0 —
    // обычная отрисовка. Тот же номер и тот же код разбора, что у статических
    // мешей: персонаж обязан участвовать в разборе кадра наравне со всем
    // остальным, иначе в режиме «Нормали» посреди цветной картинки стоит
    // обычный освещённый герой.
    void Draw(const glm::mat4& model, const glm::mat4& view, const glm::mat4& proj,
              const glm::vec3& viewPos, const LightingEnvironment& env,
              const std::vector<glm::mat4>& bones,
              const ShadowBinding& shadows,
              const sage::render::ReflectionBinding* reflections = nullptr,
              const std::vector<float>* morphWeights = nullptr,
              int shadingMode = 0) const;

    // Рисует геометрию ТОЛЬКО в глубину для карты теней, со скиннингом в текущей
    // позе — чтобы анимированная модель ОТБРАСЫВАЛА тень. Вызывается внутри
    // depth-прохода солнца (после static-геометрии, до EndRender). lightMatrix —
    // uLightSpace прохода. bones пуст — bind-поза.
    void DrawDepth(const glm::mat4& model, const glm::mat4& lightMatrix,
                   const std::vector<glm::mat4>& bones,
                   const std::vector<float>* morphWeights = nullptr) const;

    // Силуэт модели СПЛОШНЫМ ЦВЕТОМ в текущей позе — кайма выделения в
    // редакторе.
    //
    // Нужен отдельным входом, потому что силуэт обязан совпадать с тем, что
    // человек ВИДИТ. У персонажа рисуется скелетная модель в позе клипа, а
    // кайму редактор обводил по статической копии — то есть по позе привязки:
    // персонаж стоит, а вокруг него растопыренная Т-поза. Выглядит это как
    // «выделение не туда», хотя выделено ровно то, что нужно.
    //
    // bones пуст — поза привязки (как и у DrawDepth).
    void DrawSilhouette(const glm::mat4& model, const glm::mat4& viewProj,
                        const std::vector<glm::mat4>& bones,
                        const std::vector<float>* morphWeights = nullptr) const;

private:
    // Сборка модели из уже разобранных данных (ModelData). Частный член, а не
    // свободная функция: только отсюда заполняются приватные поля, и открывать
    // их наружу ради одной функции незачем.
    static std::unique_ptr<SkinnedModel> BuildFromData(struct ModelData& data);

    std::vector<SkinnedSubMesh> m_subMeshes;
    sage::anim::Skeleton m_skeleton;
    std::vector<sage::anim::AnimationClip> m_clips;
    std::vector<std::string> m_morphNames;
    std::vector<float> m_morphDefaults;
};

} // namespace sage::render
