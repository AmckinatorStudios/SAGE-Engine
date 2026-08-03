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
    CHECK_NEAR(m.Albedo.r, 0.8f, 1e-4f);
    CHECK_NEAR(m.Albedo.g, 0.4f, 1e-4f);
    CHECK_NEAR(m.Albedo.b, 0.2f, 1e-4f);
    CHECK_NEAR(m.Opacity, 0.5f, 1e-4f);   // альфа baseColorFactor
    CHECK_NEAR(m.Metallic, 0.75f, 1e-4f);
    CHECK_NEAR(m.Roughness, 0.25f, 1e-4f);
    CHECK_NEAR(m.Emissive.b, 0.3f, 1e-4f);
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
    CHECK_NEAR(m.Albedo.g, 0.6f, 1e-4f);
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
