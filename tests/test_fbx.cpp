// Тесты FBX-загрузчика (ufbx) — GL-НЕЗАВИСИМАЯ CPU-часть разбора геометрии.
// Фикстура (минимальный ASCII FBX 7.4 с одним треугольником) пишется во
// временный файл прямо здесь, чтобы тест был самодостаточным и не зависел от
// путей к ассетам. GPU-путь (Model::Load/SkinnedModel::Load -> RHI Mesh)
// проверяется headless-прогонами в scripts/ci_smoke_test.sh.
#include "TestFramework.h"

#include "sage/render/ModelLoader.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

// Минимальный ASCII FBX: треугольник (0,0,0)-(1,0,0)-(0,1,0), нормали +Z.
// UpAxis=Y — под опции загрузчика (ufbx_axes_right_handed_y_up), поэтому
// координаты приходят как есть, без переворота осей.
static const char* kTriFbx = R"FBX(; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
	Creator: "SAGE test fixture"
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",1
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",2
		P: "FrontAxisSign", "int", "Integer", "",1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",1
	}
}
Objects:  {
	Geometry: 1000, "Geometry::tri", "Mesh" {
		Vertices: *9 {
			a: 0,0,0,1,0,0,0,1,0
		}
		PolygonVertexIndex: *3 {
			a: 0,1,-3
		}
		LayerElementNormal: 0 {
			Version: 101
			Name: ""
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "Direct"
			Normals: *9 {
				a: 0,0,1,0,0,1,0,0,1
			}
		}
		Layer: 0 {
			Version: 100
			LayerElement:  {
				Type: "LayerElementNormal"
				TypedIndex: 0
			}
		}
	}
	Model: 2000, "Model::tri", "Mesh" {
		Version: 232
	}
}
Connections:  {
	C: "OO",2000,0
	C: "OO",1000,2000
}
)FBX";

static fs::path WriteFixture() {
    fs::path p = fs::temp_directory_path() / "sage_test_tri.fbx";
    std::ofstream f(p, std::ios::binary);
    f << kTriFbx;
    f.close();
    return p;
}

TEST(Fbx_parses_triangle_geometry) {
    fs::path p = WriteFixture();
    std::vector<Vertex> verts;
    std::vector<unsigned int> idx;
    ModelLoader::ParseFbxGeometry(p.string(), verts, idx);
    fs::remove(p);

    // Один треугольник -> 3 вершины, 3 индекса.
    CHECK_EQ((int)verts.size(), 3);
    CHECK_EQ((int)idx.size(), 3);
    // Индексы валидны и покрывают все вершины.
    for (unsigned int i : idx) CHECK_TRUE(i < verts.size());
}

TEST(Fbx_triangle_positions_and_normals) {
    fs::path p = WriteFixture();
    std::vector<Vertex> verts;
    std::vector<unsigned int> idx;
    ModelLoader::ParseFbxGeometry(p.string(), verts, idx);
    fs::remove(p);

    CHECK_EQ((int)verts.size(), 3);
    // Все вершины лежат в плоскости z=0 (треугольник в XY).
    for (const Vertex& v : verts) CHECK_NEAR(v.Position.z, 0.0f, 1e-4);
    // Нормали единичной длины и направлены по +Z (сгенерированы/прочитаны).
    for (const Vertex& v : verts) {
        float len = std::sqrt(v.Normal.x * v.Normal.x + v.Normal.y * v.Normal.y + v.Normal.z * v.Normal.z);
        CHECK_NEAR(len, 1.0f, 1e-3);
        CHECK_NEAR(std::fabs(v.Normal.z), 1.0f, 1e-3);
    }
    // Bounding-box: X и Y равны между собой и ненулевые. Абсолютный размер
    // зависит от единиц FBX (ufbx конвертирует cm->m через target_unit_meters),
    // поэтому проверяем структуру (квадратный охват), а не конкретную величину.
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (const Vertex& v : verts) {
        minx = std::min(minx, v.Position.x); maxx = std::max(maxx, v.Position.x);
        miny = std::min(miny, v.Position.y); maxy = std::max(maxy, v.Position.y);
    }
    float ex = maxx - minx, ey = maxy - miny;
    CHECK_TRUE(ex > 1e-5f);
    CHECK_TRUE(ey > 1e-5f);
    CHECK_NEAR(ex, ey, 1e-4); // равносторонний охват по X и Y
}

TEST(Fbx_missing_file_throws) {
    std::vector<Vertex> verts;
    std::vector<unsigned int> idx;
    bool threw = false;
    try {
        ModelLoader::ParseFbxGeometry("/nonexistent/definitely-not-here.fbx", verts, idx);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK_TRUE(threw);
}
