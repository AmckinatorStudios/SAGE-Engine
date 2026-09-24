// Материал, приезжающий ВМЕСТЕ с моделью.
//
// Проверяется не «функция вернула true», а именно то, из-за чего модели
// приезжали в сцену белыми болванками: факторы читаются, внешние карты
// находятся, а упакованные по каналам glTF-текстуры раскладываются в
// одноканальные так, как их ждёт движок (metallic и roughness — из R-канала
// СВОИХ карт). Ошибка в каналах не видна ни по какому «загрузилось/не
// загрузилось»: материал появится, просто металличность придёт из чужого
// канала — поэтому пиксели и сверяются поимённо.
#include "TestFramework.h"

#include "sage/render/ModelMaterial.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/render/Material.h"
#include "sage/ecs/RenderComponents.h"
#include "sage/render/ModelLoader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION_ALREADY_IN_LIB
#include <stb_image.h>

namespace fs = std::filesystem;

namespace {

fs::path TempDir(const char* name) {
    fs::path dir = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void WriteText(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
}

// Однопиксельный PNG заданного цвета — минимальная настоящая картинка, по
// которой видно, какой канал куда уехал.
void WritePixelPng(const fs::path& p, unsigned char r, unsigned char g, unsigned char b);

// Куб из двух треугольников — геометрия здесь не проверяется, но glTF без
// примитива невалиден.
const char* kGltfTemplate = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
     "min": [0,0,0], "max": [1,1,0]},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 6}
  ],
  "buffers": [{"byteLength": 42, "uri": "geom.bin"}],
  "images": [{"uri": "base.png"}, {"uri": "orm.png"}],
  "textures": [{"source": 0}, {"source": 1}],
  "materials": [{
    "name": "Ржавое железо",
    "pbrMetallicRoughness": {
      "baseColorFactor": [0.8, 0.4, 0.2, 0.5],
      "metallicFactor": 0.75,
      "roughnessFactor": 0.25,
      "baseColorTexture": {"index": 0},
      "metallicRoughnessTexture": {"index": 1}
    },
    "occlusionTexture": {"index": 1},
    "emissiveFactor": [0.1, 0.2, 0.3]
  }]
})";

// Геометрия для шаблона выше: 3 вершины float3 + 3 индекса uint16.
void WriteGeomBin(const fs::path& p) {
    const float verts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const unsigned short idx[3] = {0, 1, 2};
    std::ofstream f(p, std::ios::binary);
    f.write((const char*)verts, sizeof(verts));
    f.write((const char*)idx, sizeof(idx));
}

// Читает первый пиксель одноканальной картинки. -1 — файл не читается.
int FirstPixel(const std::string& path) {
    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &comp, 1);
    if (!data) return -1;
    const int value = data[0];
    stbi_image_free(data);
    return value;
}

} // namespace

TEST(model_material_gltf_reads_factors) {
    const fs::path dir = TempDir("sage_test_modelmat_factors");
    const fs::path gltf = dir / "iron.gltf";
    WriteText(gltf, kGltfTemplate);
    WriteGeomBin(dir / "geom.bin");
    WritePixelPng(dir / "base.png", 200, 100, 50);
    // ORM: R = ambient occlusion, G = roughness, B = metallic. Три РАЗНЫХ
    // значения — иначе перепутанные каналы дали бы тот же ответ.
    WritePixelPng(dir / "orm.png", 10, 120, 240);

    const ModelLoader::ExtractedMaterial m = ModelLoader::ExtractMaterial(gltf.string());
    CHECK_TRUE(m.Found);
    CHECK_EQ(m.Name, std::string("Ржавое железо"));
    // Цвет файла ЛИНЕЙНЫЙ (так требует glTF), материал движка хранит его в
    // sRGB (как показывает инспектор) — сверяем то, что дойдёт до шейдера.
    CHECK_NEAR(SrgbToLinear(m.Albedo.r), 0.8f, 1e-4f);
    CHECK_NEAR(SrgbToLinear(m.Albedo.g), 0.4f, 1e-4f);
    CHECK_NEAR(SrgbToLinear(m.Albedo.b), 0.2f, 1e-4f);
    CHECK_TRUE(m.Albedo.g > 0.6f);   // и хранится он именно в sRGB, а не как в файле
    CHECK_NEAR(m.Opacity, 0.5f, 1e-4f);   // альфа baseColorFactor
    CHECK_NEAR(m.Metallic, 0.75f, 1e-4f);
    CHECK_NEAR(m.Roughness, 0.25f, 1e-4f);
    CHECK_NEAR(SrgbToLinear(m.Emissive.b), 0.3f, 1e-4f);
}

TEST(model_material_gltf_unpacks_orm_channels) {
    const fs::path dir = TempDir("sage_test_modelmat_orm");
    const fs::path gltf = dir / "iron.gltf";
    WriteText(gltf, kGltfTemplate);
    WriteGeomBin(dir / "geom.bin");
    WritePixelPng(dir / "base.png", 200, 100, 50);
    WritePixelPng(dir / "orm.png", 10, 120, 240);

    const ModelLoader::ExtractedMaterial m = ModelLoader::ExtractMaterial(gltf.string());
    CHECK_TRUE(m.Found);

    // Albedo — целая картинка, её копировать незачем: ссылка на соседний файл.
    CHECK_TRUE(m.AlbedoMap.find("base.png") != std::string::npos);

    // А вот эти трое в glTF лежат В ОДНОЙ текстуре по разным каналам, и движок
    // читает каждую свою карту из R. Значит, рядом с моделью должны появиться
    // три отдельных одноканальных файла — с ТЕМИ значениями, что стояли в
    // соответствующих каналах ORM.
    CHECK_FALSE(m.MetallicMap.empty());
    CHECK_FALSE(m.RoughnessMap.empty());
    CHECK_FALSE(m.AOMap.empty());
    CHECK_TRUE(m.MetallicMap != m.RoughnessMap);
    CHECK_EQ(FirstPixel(m.MetallicMap), 240);   // B
    CHECK_EQ(FirstPixel(m.RoughnessMap), 120);  // G
    CHECK_EQ(FirstPixel(m.AOMap), 10);          // R
}

TEST(model_material_obj_reads_mtl) {
    const fs::path dir = TempDir("sage_test_modelmat_obj");
    WriteText(dir / "box.obj",
              "mtllib box.mtl\n"
              "usemtl painted\n"
              "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
              "f 1 2 3\n");
    WriteText(dir / "box.mtl",
              "newmtl painted\n"
              "Kd 0.2 0.6 0.9\n"
              "Ke 0.0 0.0 0.0\n"
              "d 0.75\n"
              "Pm 0.3\n"
              "Pr 0.6\n"
              "map_Kd paint.png\n"
              "norm paint_n.png\n");
    WritePixelPng(dir / "paint.png", 50, 150, 230);
    WritePixelPng(dir / "paint_n.png", 128, 128, 255);

    const ModelLoader::ExtractedMaterial m = ModelLoader::ExtractMaterial((dir / "box.obj").string());
    CHECK_TRUE(m.Found);
    CHECK_NEAR(SrgbToLinear(m.Albedo.g), 0.6f, 1e-4f);   // Kd линейный, .sagemat — sRGB
    CHECK_NEAR(m.Opacity, 0.75f, 1e-4f);
    CHECK_NEAR(m.Metallic, 0.3f, 1e-4f);
    CHECK_NEAR(m.Roughness, 0.6f, 1e-4f);
    CHECK_TRUE(m.AlbedoMap.find("paint.png") != std::string::npos);
    CHECK_TRUE(m.NormalMap.find("paint_n.png") != std::string::npos);
}

TEST(model_material_missing_file_does_not_throw) {
    // Импорт материала — вспомогательный шаг поверх уже удавшейся загрузки
    // геометрии, и уронить её он не имеет права.
    const ModelLoader::ExtractedMaterial m = ModelLoader::ExtractMaterial("нет-такого-файла.gltf");
    CHECK_FALSE(m.Found);
    CHECK_FALSE(m.Warnings.empty());
}

TEST(model_material_geometry_without_material_is_not_an_error) {
    const fs::path dir = TempDir("sage_test_modelmat_bare");
    WriteText(dir / "bare.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    const ModelLoader::ExtractedMaterial m =
        ModelLoader::ExtractMaterial((dir / "bare.obj").string());
    CHECK_FALSE(m.Found);       // материала нет
    CHECK_TRUE(m.Warnings.empty());  // и жаловаться не на что
}

// --- вспомогательное ---------------------------------------------------------

namespace {
// Однопиксельный PNG пишется вручную, а не через stb_image_write: тест
// линкуется с движком, где реализация записи уже развёрнута (Screenshot.cpp),
// и второе разворачивание дало бы дублирующиеся символы. PNG без сжатия
// (deflate "stored") — формат простой ровно настолько, чтобы это было проще
// зависимости.
unsigned long Crc32(const unsigned char* data, size_t len) {
    static unsigned long table[256];
    static bool ready = false;
    if (!ready) {
        for (unsigned long n = 0; n < 256; ++n) {
            unsigned long c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        ready = true;
    }
    unsigned long c = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}

void PutBE32(std::vector<unsigned char>& v, unsigned long x) {
    v.push_back((unsigned char)((x >> 24) & 0xFF));
    v.push_back((unsigned char)((x >> 16) & 0xFF));
    v.push_back((unsigned char)((x >> 8) & 0xFF));
    v.push_back((unsigned char)(x & 0xFF));
}

void Chunk(std::vector<unsigned char>& out, const char* type,
           const std::vector<unsigned char>& data) {
    PutBE32(out, (unsigned long)data.size());
    std::vector<unsigned char> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    PutBE32(out, Crc32(body.data(), body.size()));
}

void WritePixelPng(const fs::path& p, unsigned char r, unsigned char g, unsigned char b) {
    std::vector<unsigned char> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<unsigned char> ihdr;
    PutBE32(ihdr, 1);            // width
    PutBE32(ihdr, 1);            // height
    ihdr.push_back(8);           // bit depth
    ihdr.push_back(2);           // colour type: truecolour RGB
    ihdr.push_back(0);           // compression
    ihdr.push_back(0);           // filter
    ihdr.push_back(0);           // interlace
    Chunk(png, "IHDR", ihdr);

    // Сырые данные скан-линии: фильтр 0 + RGB.
    const std::vector<unsigned char> raw = {0, r, g, b};

    // zlib-обёртка вокруг НЕСЖАТОГО deflate-блока.
    std::vector<unsigned char> z = {0x78, 0x01};
    z.push_back(0x01);                                  // финальный, тип «stored»
    z.push_back((unsigned char)(raw.size() & 0xFF));    // LEN
    z.push_back((unsigned char)((raw.size() >> 8) & 0xFF));
    z.push_back((unsigned char)(~raw.size() & 0xFF));   // NLEN
    z.push_back((unsigned char)((~raw.size() >> 8) & 0xFF));
    z.insert(z.end(), raw.begin(), raw.end());
    unsigned long a = 1, bsum = 0;                      // Adler-32
    for (unsigned char byte : raw) { a = (a + byte) % 65521; bsum = (bsum + a) % 65521; }
    PutBE32(z, (bsum << 16) | a);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});

    std::ofstream f(p, std::ios::binary);
    f.write((const char*)png.data(), (std::streamsize)png.size());
}
} // namespace

// ---------------------------------------------------------------------------
// МОДЕЛЬ, ЛЕЖАЩАЯ В ПРОЕКТЕ, А НЕ В ТЕКУЩЕМ КАТАЛОГЕ ПРОЦЕССА
//
// Из отчёта человека, дословно из его лога:
//
//   [Editor] Внесена папка: C:\...\SAGE Projects\MyGame\low_poly_environment
//   [Model] low_poly_environment/scene.gltf: файл модели не найден:
//           low_poly_environment/scene.gltf
//
// Папка с моделью лежала в проекте, геометрия рисовалась (обложка в Assets
// показывала модель), а материалы «не находились» — потому что путь к модели
// здесь брался КАК ЕСТЬ и открывался относительно каталога, из которого
// запущен редактор (C:\...\Downloads\SageEditor-Windows). Всё остальное в
// движке давно ходит через AssetDatabase::LocatePath — и только разбор
// материалов ходил мимо.
//
// Проверка воспроизводит ровно это: проект в одном месте, текущий каталог — в
// другом, ссылка на модель относительная.
TEST(model_material_resolves_a_project_relative_path) {
    const fs::path project = TempDir("sage_test_modelmat_project");
    const fs::path inner = project / "low_poly_environment";
    std::error_code ec;
    fs::create_directories(inner, ec);
    WriteText(inner / "scene.gltf", kGltfTemplate);
    WriteGeomBin(inner / "geom.bin");
    WritePixelPng(inner / "base.png", 200, 100, 50);
    WritePixelPng(inner / "orm.png", 10, 120, 240);

    sage::AssetDatabase::Instance().ScanProject(project.string());

    // Ровно та строка, которую держит MeshRendererComponent::Ref.path.
    const ModelLoader::ExtractedMaterial m =
        ModelLoader::ExtractMaterial("low_poly_environment/scene.gltf");
    CHECK_TRUE(m.Found);
    CHECK_EQ(m.Name, std::string("Ржавое железо"));
    // И карты найдены рядом с моделью, а не «где-то относительно процесса».
    CHECK_TRUE(m.AlbedoMap.find("base.png") != std::string::npos);
    // Путь не просто похож на правильный — по нему ОТКРЫВАЕТСЯ файл. Ссылка,
    // которую нельзя открыть, и есть «Не удалось загрузить» в слоте материала.
    CHECK_TRUE(FirstPixel(m.AlbedoMap) >= 0);
}

// Экспорт, не донёсший материалы: имена есть, а вида нет ни у одного.
//
// Это и стоит за жалобой «модель грузится без текстур». В файле четырнадцать
// материалов с правильными именами — и все белые, без единой карты: так
// экспортёр glTF выгружает узловые материалы Blender, из которых он понимает
// только Principled BSDF. Молчать об этом нельзя: белая модель без объяснения
// читается как «движок не грузит текстуры», и человек ищет поломку не там.
TEST(model_material_export_without_maps_and_colours_is_reported) {
    const fs::path dir = TempDir("sage_test_modelmat_blank");
    WriteGeomBin(dir / "geom.bin");
    // Тот же шаблон, но материал — пустышка: только имя и doubleSided, ровно
    // как в разбираемом файле.
    std::string gltf = kGltfTemplate;
    const size_t at = gltf.find("\"images\"");
    gltf = gltf.substr(0, at) +
           "\"materials\": [{\"name\": \"Suit1\", \"doubleSided\": true}]\n}";
    WriteText(dir / "blank.gltf", gltf);

    const ModelLoader::ExtractedMaterialSet set =
        ModelLoader::ExtractMaterials((dir / "blank.gltf").string());
    CHECK_EQ((int)set.Materials.size(), 1);
    CHECK_TRUE(set.Found());
    CHECK_FALSE(set.Warnings.empty());
    if (!set.Warnings.empty()) {
        // Предупреждение обязано называть ПРИЧИНУ и что делать, а не просто
        // отмечать факт: «нет текстур» само по себе бесполезно.
        CHECK_TRUE(set.Warnings.front().find("экспорт их не донёс") != std::string::npos);
        CHECK_TRUE(set.Warnings.front().find("fbx") != std::string::npos);
    }
}

// Обратная сторона: материал с цветом (без единой карты) — законный результат,
// и ругаться на него нельзя. Иначе предупреждение обесценится: его перестанут
// читать ровно там, где оно важно.
TEST(model_material_coloured_material_without_maps_is_not_reported) {
    const fs::path dir = TempDir("sage_test_modelmat_colour");
    WriteGeomBin(dir / "geom.bin");
    std::string gltf = kGltfTemplate;
    const size_t at = gltf.find("\"images\"");
    gltf = gltf.substr(0, at) +
           "\"materials\": [{\"name\": \"Suit1\", \"pbrMetallicRoughness\": "
           "{\"baseColorFactor\": [0.36, 0.21, 0.03, 1]}}]\n}";
    WriteText(dir / "colour.gltf", gltf);

    const ModelLoader::ExtractedMaterialSet set =
        ModelLoader::ExtractMaterials((dir / "colour.gltf").string());
    CHECK_EQ((int)set.Materials.size(), 1);
    CHECK_TRUE(set.Warnings.empty());
}

// ============================================================================
//  ПОВЕДЕНИЕ МАТЕРИАЛА: прозрачность, двусторонность, масштаб развёртки
//
//  Файл описывает их наравне с цветом, а до движка они не доезжали. Из-за
//  этого листва и решётки приезжали непрозрачными прямоугольниками (вырез в
//  альфе никто не смотрел), односторонние листы исчезали с изнанки, а
//  свечение из KHR_materials_emissive_strength оставалось просто светлым
//  цветом — bloom его не подхватывал.
// ============================================================================

// Тот же шаблон, что и выше, но материал описывает своё ПОВЕДЕНИЕ.
const char* kGltfBehaviourTemplate = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
     "min": [0,0,0], "max": [1,1,0]},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 6}
  ],
  "buffers": [{"byteLength": 42, "uri": "geom.bin"}],
  "images": [{"uri": "leaf%20base.png"}],
  "textures": [{"source": 0}],
  "materials": [{
    "name": "Листва",
    "doubleSided": true,
    "alphaMode": "MASK",
    "alphaCutoff": 0.33,
    "pbrMetallicRoughness": {
      "baseColorTexture": {"index": 0,
        "extensions": {"KHR_texture_transform": {"scale": [3.0, 2.0]}}}
    },
    "emissiveFactor": [1.0, 1.0, 1.0],
    "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 4.5}}
  }]
})";

TEST(model_material_gltf_reads_alpha_mode_and_double_sided) {
    const fs::path dir = TempDir("sage_test_modelmat_behaviour");
    const fs::path gltf = dir / "leaf.gltf";
    WriteText(gltf, kGltfBehaviourTemplate);
    WriteGeomBin(dir / "geom.bin");
    // Имя С ПРОБЕЛОМ: в URI оно закодировано как %20, и без раскодирования
    // карта не находится никогда.
    WritePixelPng(dir / "leaf base.png", 40, 160, 40);

    const ModelLoader::ExtractedMaterial m = ModelLoader::ExtractMaterial(gltf.string());
    CHECK_TRUE(m.Found);
    CHECK_TRUE(m.DoubleSided);
    CHECK_EQ(m.AlphaMode, 1);                      // MASK
    CHECK_NEAR(m.AlphaCutoff, 0.33f, 1e-4f);
    CHECK_NEAR(m.EmissiveStrength, 4.5f, 1e-4f);
    CHECK_NEAR(m.UVScale.x, 3.0f, 1e-4f);
    CHECK_NEAR(m.UVScale.y, 2.0f, 1e-4f);
    // И сама карта нашлась, несмотря на пробел в имени.
    CHECK_TRUE(m.AlbedoMap.find("leaf base.png") != std::string::npos);
}

TEST(model_material_alpha_cutoff_reaches_the_engine_material) {
    // Порог доезжает до материала движка: шейдер умел отсекать давно
    // (uAlphaCutoff), но ставил порог только скиновый путь, и одна и та же
    // ветка выглядела по-разному со скелетом и без.
    Material mat;
    CHECK_NEAR(mat.Render.AlphaCutoff, 0.0f, 1e-6f);   // по умолчанию режима нет

    mat.Render.AlphaCutoff = 0.4f;
    mat.Render.Cull = CullFaces::None;
    const fs::path dir = TempDir("sage_test_material_cutoff");
    const fs::path file = dir / "leaf.sagemat";
    mat.SaveToFile(file.string());

    const Material back = Material::LoadFromFile(file.string());
    CHECK_NEAR(back.Render.AlphaCutoff, 0.4f, 1e-4f);
    CHECK_TRUE(back.Render.Cull == CullFaces::None);
}

TEST(model_material_obj_dissolve_becomes_blending) {
    // У .mtl нет режима прозрачности — есть только d. Значение меньше единицы
    // и означает смешивание: другого способа сказать это в формате нет.
    const fs::path dir = TempDir("sage_test_modelmat_dissolve");
    WriteText(dir / "glass.obj", "mtllib glass.mtl\nusemtl Glass\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    WriteText(dir / "glass.mtl", "newmtl Glass\nKd 0.6 0.8 1.0\nd 0.35\n");

    const ModelLoader::ExtractedMaterial m = ModelLoader::ExtractMaterial((dir / "glass.obj").string());
    CHECK_TRUE(m.Found);
    CHECK_NEAR(m.Opacity, 0.35f, 1e-3f);
    CHECK_EQ(m.AlphaMode, 2);   // BLEND
}

// ============================================================================
//  ПОВТОР ТЕКСТУРЫ: ТРИ РЕЖИМА
//
//  Развёртка примитива — 0..1 на грань, поэтому без повтора картинка на
//  большом объекте растянута на всю его длину, а с постоянным повтором
//  растягивается заново при каждом изменении РАЗМЕРА объекта: тот же пол,
//  растянутый с 5 до 50 метров, снова показывает одну плитку на всю длину.
//  Арифметика режима вынесена отдельно (TilingFactor) и проверяется числами,
//  без видеокарты: подставляют её три разных прохода отрисовки, и разойтись
//  им нельзя.
// ============================================================================
#include <glm/gtc/matrix_transform.hpp>

TEST(material_uniform_tiling_uses_one_number_for_both_axes) {
    MaterialRender r;
    r.Tiling = MaterialRender::TilingMode::Uniform;
    r.UVScaleX = 6.0f;
    r.UVScaleY = 2.0f;   // второе число режим не трогает...
    const glm::vec2 f = TilingFactor(r, glm::vec3(1.0f));
    CHECK_NEAR(f.x, 6.0f, 1e-4f);
    CHECK_NEAR(f.y, 6.0f, 1e-4f);

    // ...и оно сохраняется: вернувшись к режиму «по осям», человек получает
    // подобранную пару, а не единицу.
    r.Tiling = MaterialRender::TilingMode::Separate;
    const glm::vec2 s = TilingFactor(r, glm::vec3(1.0f));
    CHECK_NEAR(s.x, 6.0f, 1e-4f);
    CHECK_NEAR(s.y, 2.0f, 1e-4f);
}

TEST(material_world_size_tiling_keeps_the_tile_the_same_size) {
    MaterialRender r;
    r.Tiling = MaterialRender::TilingMode::WorldSize;
    r.UVScaleX = 0.5f;   // пол-плитки на метр
    r.UVScaleY = 0.5f;

    // Пол 10 x 10 метров толщиной 0.2 — большие измерения X и Z.
    const glm::vec2 floor = TilingFactor(r, glm::vec3(10.0f, 0.2f, 10.0f));
    CHECK_NEAR(floor.x, 5.0f, 1e-4f);
    CHECK_NEAR(floor.y, 5.0f, 1e-4f);

    // Растянули пол вдвое — плитка осталась того же физического размера,
    // то есть повторов стало вдвое больше. Ровно этого и нет у постоянного
    // повтора: там растяжение объекта растягивает и рисунок.
    const glm::vec2 wider = TilingFactor(r, glm::vec3(20.0f, 0.2f, 10.0f));
    CHECK_NEAR(wider.x, 10.0f, 1e-4f);
    CHECK_NEAR(wider.y, 5.0f, 1e-4f);

    // Стена 10 x 5 толщиной 0.2 — большие измерения X и Y, и правило то же:
    // «пол» и «стену» различать не приходится.
    const glm::vec2 wall = TilingFactor(r, glm::vec3(10.0f, 5.0f, 0.2f));
    CHECK_NEAR(wall.x, 5.0f, 1e-4f);
    CHECK_NEAR(wall.y, 2.5f, 1e-4f);

    // Сплющенный в ноль объект не даёт ни нуля, ни NaN: такое число ушло бы в
    // шейдер и покрасило объект в чёрное.
    const glm::vec2 flat = TilingFactor(r, glm::vec3(0.0f));
    CHECK_TRUE(flat.x > 0.0f && flat.y > 0.0f);
    CHECK_TRUE(flat.x == flat.x && flat.y == flat.y);   // не NaN
}

TEST(material_world_scale_comes_from_the_model_matrix) {
    const glm::mat4 m = glm::scale(glm::rotate(glm::mat4(1.0f), 1.1f, glm::vec3(0, 1, 0)),
                                   glm::vec3(3.0f, 1.0f, 7.0f));
    const glm::vec3 s = WorldScaleOf(m);
    // Поворот масштаб не меняет — иначе повтор «по размеру» ехал бы от одного
    // только разворота объекта.
    CHECK_NEAR(s.x, 3.0f, 1e-3f);
    CHECK_NEAR(s.y, 1.0f, 1e-3f);
    CHECK_NEAR(s.z, 7.0f, 1e-3f);
}

TEST(material_tiling_mode_is_written_as_a_name) {
    // Числом режим в файл не пишется: вставка нового режима в середину списка
    // переназначила бы уже сохранённые материалы.
    MaterialRender::TilingMode mode = MaterialRender::TilingMode::Uniform;
    CHECK_TRUE(TilingModeFromKey("worldSize", mode));
    CHECK_TRUE(mode == MaterialRender::TilingMode::WorldSize);
    CHECK_EQ(std::string(TilingModeKey(mode)), std::string("worldSize"));
    // Опечатка не сбрасывает настройку молча.
    CHECK_FALSE(TilingModeFromKey("worldsize", mode));
    CHECK_TRUE(mode == MaterialRender::TilingMode::WorldSize);
}

TEST(model_material_obj_alpha_map_becomes_a_double_sided_cutout) {
    // map_d — КАРТА прозрачности: так Blender пишет листву, траву и решётки
    // (Stylized Nature MegaKit — у каждого дерева). Она не читалась, и листва
    // приезжала сплошными непрозрачными квадратами.
    const fs::path dir = TempDir("sage_test_modelmat_alphamap");
    WriteText(dir / "tree.obj", "mtllib tree.mtl\nusemtl Leaves\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    WriteText(dir / "tree.mtl",
              "newmtl Leaves\nKd 1 1 1\nmap_Kd leaves.png\nmap_d leaves.png\n");
    WritePixelPng(dir / "leaves.png", 60, 160, 40);

    const ModelLoader::ExtractedMaterial m = ModelLoader::ExtractMaterial((dir / "tree.obj").string());
    CHECK_TRUE(m.Found);
    CHECK_EQ(m.AlphaMode, 1);   // вырез, а не смешивание
    CHECK_TRUE(m.DoubleSided);
    CHECK_NEAR(m.Opacity, 1.0f, 1e-4f);
}

TEST(model_material_gltf_uv_matches_the_flipped_textures_of_static_meshes) {
    // В glTF v=0 — ВЕРХНИЙ край картинки, а текстуры статического меша
    // грузятся перевёрнутыми под OpenGL (v=0 — нижний край). Развёртка,
    // взятая как есть, читала картинку вверх ногами: на атласе и палитре
    // каждая часть модели получала чужой кусок.
    const fs::path dir = TempDir("sage_test_gltf_uv");
    const float data[] = {0, 0, 0, 1, 0, 0, 0, 1, 0,     // позиции
                          0.0f, 0.25f, 1.0f, 0.25f, 0.0f, 0.75f};   // uv
    const unsigned short idx[3] = {0, 1, 2};
    {
        std::ofstream f(dir / "uv.bin", std::ios::binary);
        f.write((const char*)data, sizeof(data));
        f.write((const char*)idx, sizeof(idx));
    }
    WriteText(dir / "uv.gltf", R"({
  "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0]},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2"},
    {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 24},
    {"buffer": 0, "byteOffset": 60, "byteLength": 6}
  ],
  "buffers": [{"byteLength": 66, "uri": "uv.bin"}]
})");
    const sage::render::MeshData mesh = ModelLoader::LoadMeshData((dir / "uv.gltf").string());
    CHECK_EQ((int)mesh.Vertices.size(), 3);
    if (mesh.Vertices.size() != 3) return;
    CHECK_NEAR(mesh.Vertices[0].TexCoords.x, 0.0f, 1e-5f);
    CHECK_NEAR(mesh.Vertices[0].TexCoords.y, 0.75f, 1e-5f);
    CHECK_NEAR(mesh.Vertices[2].TexCoords.y, 0.25f, 1e-5f);
}

// --- Листва из .obj: матовость и вырез без map_d ----------------------------
#include <stb_image_write.h>   // реализация — в движке (render/Screenshot.cpp)

#include "sage/render/AlphaBleed.h"

TEST(model_material_obj_shininess_becomes_roughness) {
    // Ns не читался, и любой .obj получал шероховатость 0.5: у совсем матовой
    // листвы и коры (Ns 0 у Blender) — белёсый блик неба по всей кроне.
    const fs::path dir = TempDir("sage_test_modelmat_ns");
    WriteText(dir / "tree.obj", "mtllib tree.mtl\nusemtl Matte\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"
                                "usemtl Glossy\nf 1 3 2\n");
    WriteText(dir / "tree.mtl", "newmtl Matte\nNs 0.000000\nKd 1 1 1\n"
                                "newmtl Glossy\nNs 900.000000\nKd 1 1 1\n");
    const ModelLoader::ExtractedMaterialSet set = ModelLoader::ExtractMaterials((dir / "tree.obj").string());
    CHECK_EQ((int)set.Materials.size(), 2);
    if (set.Materials.size() != 2) return;
    CHECK_NEAR(set.Materials[0].Roughness, 1.0f, 1e-3f);
    CHECK_NEAR(set.Materials[1].Roughness, 0.04f, 1e-3f);
}

TEST(model_material_albedo_with_cut_out_alpha_is_detected_without_map_d) {
    // Кусты и цветы из набора: карта прозрачности в .mtl не записана, а
    // альбедо режет по альфе. Без выреза — квадраты с чёрными полосами.
    const fs::path dir = TempDir("sage_test_modelmat_cutout");
    std::vector<unsigned char> px(16 * 16 * 4, 0);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            unsigned char* p = &px[(size_t)(y * 16 + x) * 4];
            p[1] = 160;
            p[3] = x < 8 ? 255 : 0;   // половина — лист, половина — пусто
        }
    stbi_write_png((dir / "bush.png").string().c_str(), 16, 16, 4, px.data(), 16 * 4);
    // Та же картинка без прозрачности — вырезать там нечего.
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    stbi_write_png((dir / "bark.png").string().c_str(), 16, 16, 4, px.data(), 16 * 4);

    WriteText(dir / "bush.obj", "mtllib bush.mtl\nusemtl Leaves\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"
                                "usemtl Bark\nf 1 3 2\n");
    WriteText(dir / "bush.mtl", "newmtl Leaves\nKd 1 1 1\nmap_Kd bush.png\n"
                                "newmtl Bark\nKd 1 1 1\nmap_Kd bark.png\n");
    const ModelLoader::ExtractedMaterialSet set = ModelLoader::ExtractMaterials((dir / "bush.obj").string());
    CHECK_EQ((int)set.Materials.size(), 2);
    if (set.Materials.size() != 2) return;
    CHECK_EQ(set.Materials[0].AlphaMode, 1);
    CHECK_TRUE(set.Materials[0].DoubleSided);
    CHECK_EQ(set.Materials[1].AlphaMode, 0);
    CHECK_TRUE(!set.Materials[1].DoubleSided);
}

TEST(alpha_bleed_gives_transparent_pixels_the_leaf_colour) {
    // Под прозрачностью листвы лежит ЧЁРНОЕ, и мип-уровни подмешивали его в
    // край листа: тёмная кайма вблизи, потемневшая крона вдали.
    std::vector<unsigned char> px(8 * 8 * 4, 0);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 2; ++x) {
            unsigned char* p = &px[(size_t)(y * 8 + x) * 4];
            p[0] = 60; p[1] = 160; p[2] = 40; p[3] = 255;
        }
    sage::render::BleedTransparentColor(px, 8, 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const unsigned char* p = &px[(size_t)(y * 8 + x) * 4];
            CHECK_EQ((int)p[1], 160);                 // цвет листа — везде
            CHECK_EQ((int)p[3], x < 2 ? 255 : 0);     // вырез — прежний
        }
}

// --- .obj: текстура без Kd — множитель единица -------------------------------
//
// Blender не пишет Kd, когда цвет взят из карты, а tinyobj подставляет тогда
// 0.6. Кора и листва из .obj выходили на 40 % темнее той же модели в glTF.
TEST(model_material_obj_texture_without_kd_is_not_darkened) {
    const fs::path dir = TempDir("sage_test_modelmat_nokd");
    WriteText(dir / "tree.obj", "mtllib tree.mtl\nusemtl Bark\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"
                                "usemtl Plain\nf 1 3 2\n");
    WriteText(dir / "tree.mtl", "newmtl Bark\nKs 0.5 0.5 0.5\nmap_Kd bark.png\n"
                                "newmtl Plain\nKd 0.6 0.6 0.6\nmap_Kd bark.png\n");
    WritePixelPng(dir / "bark.png", 140, 88, 67);
    const ModelLoader::ExtractedMaterialSet set = ModelLoader::ExtractMaterials((dir / "tree.obj").string());
    CHECK_EQ((int)set.Materials.size(), 2);
    if (set.Materials.size() != 2) return;
    CHECK_NEAR(set.Materials[0].Albedo.r, 1.0f, 1e-4f);                 // Kd не записан
    CHECK_NEAR(SrgbToLinear(set.Materials[1].Albedo.r), 0.6f, 1e-4f);   // записан — как есть

    // Тот же ответ у реестра импорта (материалы модели в сцене).
    std::vector<sage::assets::ImportedMaterial> imported;
    ModelLoader::LoadObjData((dir / "tree.obj").string(), &imported);
    CHECK_EQ((int)imported.size(), 2);
    if (imported.size() == 2) CHECK_NEAR(imported[0].Albedo.r, 1.0f, 1e-4f);
}

// --- .obj: вогнутая грань режется по диагонали ВНУТРИ неё ---------------------
//
// tinyobj делит четырёхугольник по КОРОТКОЙ диагонали. У «дротика» ниже она
// идёт снаружи: один треугольник накрывает вырез, второй вывернут и
// отсекается — на изгибе ветки это «пила» из дыр и тёмных зубцов.
TEST(model_obj_concave_faces_are_triangulated_inside) {
    const fs::path dir = TempDir("sage_test_obj_concave");
    // Дротик: остриё (0,10), крылья (-1,0) и (1,0), вогнутая вершина (0,0.5).
    // Диагональ от вогнутой вершины (9.5) длиннее внешней (2).
    // Второй — пятиугольник с вогнутой вершиной, его tinyobj режет веером.
    WriteText(dir / "dart.obj",
              "v 0 10 0\nv -1 0 0\nv 0 0.5 0\nv 1 0 0\n"
              "v 3 0 0\nv 7 0 0\nv 7 4 0\nv 5 1 0\nv 3 4 0\n"
              "f 1 2 3 4\n"
              "f 5 6 7 8 9\n");
    const sage::render::MeshData mesh = ModelLoader::LoadObjData((dir / "dart.obj").string());
    CHECK_EQ((int)mesh.Indices.size(), (2 + 3) * 3);
    float area = 0.0f;
    bool allFront = true;
    for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3) {
        const glm::vec3 a = mesh.Vertices[mesh.Indices[i]].Position;
        const glm::vec3 b = mesh.Vertices[mesh.Indices[i + 1]].Position;
        const glm::vec3 c = mesh.Vertices[mesh.Indices[i + 2]].Position;
        const float z = glm::cross(b - a, c - a).z;   // обе грани — против часовой, +Z
        if (z <= 0.0f) allFront = false;
        area += 0.5f * std::abs(z);
    }
    CHECK_TRUE(allFront);                    // ни одного вывернутого треугольника
    // Площадь сумм треугольников = площади граней: без перекрытий и дыр.
    // Дротик 9.5, пятиугольник 4*4 - (вырез 0.5*4*3) = 10.
    CHECK_NEAR(area, 9.5f + 10.0f, 1e-3f);
}

// --- glTF: старое расширение specular-glossiness ------------------------------
//
// Выгрузки Sketchfab и конвертеры из FBX до сих пор пишут материал только в
// KHR_materials_pbrSpecularGlossiness. Не читая его, движок получал материал
// glTF по умолчанию — белый металл без текстур: модель целиком белая.
TEST(model_material_gltf_specular_glossiness_becomes_colour_and_texture) {
    const fs::path dir = TempDir("sage_test_modelmat_specgloss");
    std::string json = kGltfTemplate;
    const size_t at = json.find("\"materials\"");
    json = json.substr(0, at) + R"("extensionsUsed": ["KHR_materials_pbrSpecularGlossiness"],
  "materials": [{
    "name": "Fur",
    "extensions": {"KHR_materials_pbrSpecularGlossiness": {
      "diffuseFactor": [1.0, 0.6, 0.0, 1.0],
      "diffuseTexture": {"index": 0},
      "glossinessFactor": 0.25,
      "specularFactor": [1.0, 1.0, 1.0]
    }}
  }]
})";
    WriteText(dir / "fur.gltf", json);
    WriteGeomBin(dir / "geom.bin");
    WritePixelPng(dir / "base.png", 200, 150, 40);
    WritePixelPng(dir / "orm.png", 255, 128, 0);

    const ModelLoader::ExtractedMaterial m = ModelLoader::ExtractMaterial((dir / "fur.gltf").string());
    CHECK_TRUE(m.Found);
    CHECK_NEAR(SrgbToLinear(m.Albedo.g), 0.6f, 1e-3f);   // цвет из diffuseFactor
    CHECK_NEAR(m.Metallic, 0.0f, 1e-4f);                  // не металл по умолчанию
    CHECK_NEAR(m.Roughness, 0.75f, 1e-3f);                // 1 - блеск
    CHECK_TRUE(m.AlbedoMap.find("base.png") != std::string::npos);
}

// --- glTF: KHR_materials_unlit — мультяшная заливка без света ----------------
TEST(model_material_gltf_unlit_reaches_the_material) {
    const fs::path dir = TempDir("sage_test_modelmat_unlit");
    std::string json = kGltfTemplate;
    const size_t at = json.find("\"materials\"");
    json = json.substr(0, at) + R"("extensionsUsed": ["KHR_materials_unlit"],
  "materials": [{
    "name": "Toon",
    "extensions": {"KHR_materials_unlit": {}},
    "pbrMetallicRoughness": {"baseColorFactor": [1, 0.5, 0, 1], "baseColorTexture": {"index": 0}}
  }]
})";
    WriteText(dir / "toon.gltf", json);
    WriteGeomBin(dir / "geom.bin");
    WritePixelPng(dir / "base.png", 200, 150, 40);
    WritePixelPng(dir / "orm.png", 255, 128, 0);
    const ModelLoader::ExtractedMaterial m = ModelLoader::ExtractMaterial((dir / "toon.gltf").string());
    CHECK_TRUE(m.Found);
    CHECK_TRUE(m.Unlit);

    // И переживает запись в .sagemat.
    Material mat;
    mat.Render.Unlit = true;
    mat.SaveToFile((dir / "toon.sagemat").string());
    CHECK_TRUE(Material::LoadFromFile((dir / "toon.sagemat").string()).Render.Unlit);
}
