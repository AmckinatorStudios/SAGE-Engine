// Тесты границы RHI и выбора бэкенда: разбор имён, доступность, инициализация
// устройства Vulkan там, где оно есть.
//
// ЗАЧЕМ ЭТИ ТЕСТЫ. Второй бэкенд ломается не там, где рисует, а там, где
// ВЫБИРАЕТСЯ. Настройка приходит строкой из sage.cfg или переменной окружения,
// на машине игрока драйвера может не быть вовсе — и самый дорогой отказ здесь
// не «картинка не та», а «редактор не запустился из-за строчки в конфиге,
// которую человек не писал». Поэтому проверяется именно цепочка выбора:
// строка → бэкенд → доступен ли → откат.
#include "TestFramework.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include <glm/glm.hpp>

#include "sage/render/PbrShader.h"
#include <vector>

#include "sage/rhi/Conformance.h"
#include "sage/rhi/GraphicsDevice.h"

using sage::rhi::Backend;
using sage::rhi::GraphicsDevice;

TEST(Rhi_backend_names_round_trip) {
    // Имя ↔ бэкенд должны сходиться в обе стороны: имя пишется в sage.cfg и
    // читается оттуда же, и разъехавшись однажды, они молча сбросили бы выбор
    // игрока на значение по умолчанию.
    for (Backend b : {Backend::OpenGL, Backend::Vulkan, Backend::Null}) {
        Backend parsed = Backend::Null;
        CHECK_TRUE(GraphicsDevice::ParseBackend(GraphicsDevice::BackendId(b), parsed));
        CHECK_TRUE(parsed == b);
    }
}

TEST(Rhi_backend_names_are_case_insensitive_and_have_aliases) {
    Backend b = Backend::Null;
    CHECK_TRUE(GraphicsDevice::ParseBackend("Vulkan", b));
    CHECK_TRUE(b == Backend::Vulkan);
    CHECK_TRUE(GraphicsDevice::ParseBackend("VK", b));
    CHECK_TRUE(b == Backend::Vulkan);
    CHECK_TRUE(GraphicsDevice::ParseBackend("GL", b));
    CHECK_TRUE(b == Backend::OpenGL);

    // Мусор — не бэкенд, и молча становиться каким-то из них он не должен:
    // опечатка в конфиге обязана быть заметна, а не тихо включить другое.
    CHECK_FALSE(GraphicsDevice::ParseBackend("directx12", b));
    CHECK_FALSE(GraphicsDevice::ParseBackend("", b));
}

TEST(Rhi_null_backend_is_always_available) {
    // Null не требует ни драйвера, ни окна — на нём держатся headless-тесты.
    CHECK_TRUE(GraphicsDevice::Available(Backend::Null));
    std::unique_ptr<GraphicsDevice> dev = GraphicsDevice::Create(Backend::Null);
    CHECK_TRUE(dev != nullptr);
    CHECK_TRUE(std::string(dev->BackendName()) == "Null");
}

// Доступность Vulkan — ответ про КОНКРЕТНУЮ машину, а не про сборку. Тест
// проверяет не «Vulkan есть», а что ответ СОГЛАСОВАН: если Available сказал
// «да», устройство обязано создаться и назвать себя; если «нет» — Create либо
// вернёт nullptr (собрано без Vulkan), либо отдаст устройство, которое честно
// останется неинициализированным, но не уронит процесс.
TEST(Rhi_vulkan_availability_matches_what_the_device_can_do) {
    const bool available = GraphicsDevice::Available(Backend::Vulkan);
    std::unique_ptr<GraphicsDevice> dev = GraphicsDevice::Create(Backend::Vulkan);

    if (!available) {
        // Не собрано или нет драйвера — движок обязан пережить это молча.
        CHECK_TRUE(dev == nullptr || std::string(dev->BackendName()) == "Vulkan");
        return;
    }

    CHECK_TRUE(dev != nullptr);
    CHECK_TRUE(std::string(dev->BackendName()) == "Vulkan");

    // Init без оконного контекста: аргумент-загрузчик относится к OpenGL, и
    // Vulkan обязан обойтись без него — иначе headless-прогон невозможен.
    dev->Init(nullptr);

    const std::string version = dev->ApiVersion();
    // Строка версии должна называть API И ВЕРСИЮ ЧИСЛОМ («Vulkan 1.3.280 — …»),
    // а не остаться сообщением об отказе. Проверять «начинается с Vulkan»
    // недостаточно: с этого начинаются и строки неудачи («Vulkan (не
    // инициализирован)»), то есть тест прошёл бы на устройстве, которое не
    // поднялось. Именно эту строку показывают в «О программе» и в отчёте о
    // падении — она обязана быть правдой.
    CHECK_TRUE(version.rfind("Vulkan ", 0) == 0);
    CHECK_TRUE(version.size() > 8);
    CHECK_TRUE(std::isdigit((unsigned char)version[7]) != 0);
    CHECK_TRUE(dev->MaxAnisotropy() >= 1.0f);
}

// Набор соответствия RHI на Vulkan — тот же исполняемый контракт, который
// проходят Null и OpenGL. Это и есть настоящая приёмка бэкенда: пока
// реализация одна, «интерфейс» и «то, что делает OpenGL» неразличимы, и только
// второй бэкенд, прошедший тот же набор, превращает абстракцию из гипотезы в
// абстракцию.
//
// Оба уровня. Структурный: хендлы, дескрипторы, времена жизни, безопасность
// вызовов с невалидным аргументом. Пиксельный: соглашения о системе координат —
// начало отсчёта в левом НИЖНЕМ углу и строки чтения снизу вверх. У Vulkan
// начало сверху, то есть это ровно то место, где второй бэкенд обязан
// переворачивать, и молчаливо не переворачивать тоже.
TEST(Rhi_vulkan_passes_the_conformance_suite) {
    if (!GraphicsDevice::Available(Backend::Vulkan)) return; // нет драйвера — нечего проверять

    std::unique_ptr<GraphicsDevice> dev = GraphicsDevice::Create(Backend::Vulkan);
    CHECK_TRUE(dev != nullptr);
    dev->Init(nullptr);

    sage::rhi::ConformanceOptions options;
    // Пиксельный уровень ВКЛЮЧЁН: он проверяет соглашения о системе координат —
    // ровно то, где Vulkan расходится с OpenGL (начало отсчёта сверху, а не
    // снизу). Расхождение здесь не ломает сборку и не даёт сообщения: картинка
    // просто оказывается перевёрнутой, и ищут это неделями.
    options.Rasterizes = true;
    const sage::rhi::ConformanceResult result = sage::rhi::RunConformance(*dev, options);

    // Каждый провал печатается отдельно: «набор не прошёл» без списка означало
    // бы читать код набора, чтобы понять, что именно сломано.
    for (const std::string& failure : result.Failures) {
        ::sagetest::ReportFail(__FILE__, __LINE__, "соответствие RHI (Vulkan): " + failure);
    }
    CHECK_TRUE(result.Ok());
    // Порог, а не «больше нуля»: набор, который молча пропустил почти всё,
    // выглядел бы пройденным. Столько проверок структурного уровня он делает
    // на Null-бэкенде — значит и Vulkan обязан пройти не меньше.
    CHECK_TRUE(result.Checked >= 19);
    std::printf("      (соответствие RHI на Vulkan: проверок %d, пропущено %d)\n",
                result.Checked, result.Skipped);
}

// --- Компиляция шейдеров движка под Vulkan ----------------------------------
//
// САМОЕ РИСКОВАННОЕ МЕСТО ВСЕГО БЭКЕНДА. Движок написан на GLSL 3.30 со
// СВОБОДНЫМИ униформами (`uniform mat4 uModel;` вне блока) и задаёт их ПО ИМЕНИ.
// В Vulkan такого не бывает вовсе: униформа обязана лежать в блоке и
// адресоваться смещением. Держится это на одной возможности glslang —
// ослабленных правилах (setEnvInputVulkanRulesRelaxed), которые сами собирают
// свободные униформы в блок.
//
// Проверяем не игрушечный шейдер, а НАСТОЯЩИЙ общий PBR-блок движка
// (kPbrSharedGlsl): массивы, samplerCube, sampler3D, каскады теней. Если
// ослабленные правила где-то не справятся, выяснить это надо здесь, а не на
// чёрном экране.
TEST(Rhi_vulkan_compiles_the_engine_pbr_shader) {
    if (!GraphicsDevice::Available(Backend::Vulkan)) return;

    std::unique_ptr<GraphicsDevice> dev = GraphicsDevice::Create(Backend::Vulkan);
    dev->Init(nullptr);

    const std::string vert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
out vec3 FragPos;
out vec3 Normal;
void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    FragPos = world.xyz;
    Normal = aNormal;
    gl_Position = uProjection * uView * world;
}
)";

    // Преамбула — ровно та, в которой блок живёт в движке (см. RenderBatch.cpp).
    const std::string frag = std::string(R"(#version 330 core
in vec3 FragPos;
in vec3 Normal;
out vec4 FragColor;
uniform bool uLightmapEnabled;
uniform sampler2D uLightmap;
)") + sage::render::kPbrSharedGlsl + R"(
void main() {
    vec3 N = normalize(Normal);
    vec3 indirect = uLightmapEnabled ? texture(uLightmap, vec2(0.5)).rgb
                                     : DefaultIndirect(FragPos, N);
    FragColor = vec4(indirect, 1.0);
}
)";

    std::unique_ptr<sage::rhi::ShaderProgram> program = dev->CreateShaderProgram(vert, frag);
    CHECK_TRUE(program != nullptr);
    if (!program) return;

    // Установка униформы по имени обязана быть безопасной И для существующей, и
    // для несуществующей: в GL промах по имени молча игнорировался, и весь
    // движок на это рассчитывает.
    program->Use();
    program->SetMat4("uModel", glm::mat4(1.0f));
    program->SetMat4("uView", glm::mat4(1.0f));
    program->SetVec3("uSunColor", glm::vec3(1.0f));
    program->SetFloat("нет-такой-униформы", 1.0f);
    program->SetInt("uLightmap", 3); // сэмплер: номер юнита, а не значение

    // Заведомо битый шейдер обязан дать nullptr, а не «что-нибудь». Без этой
    // половины тест прошёл бы и на реализации, которая всегда возвращает
    // объект: успех компиляции нечем было бы отличить от её отсутствия.
    std::unique_ptr<sage::rhi::ShaderProgram> broken =
        dev->CreateShaderProgram(vert, "#version 330 core\nвот это не шейдер\n");
    CHECK_TRUE(broken == nullptr);
}

// --- Отрисовка через Vulkan: от вершин до пикселей ---------------------------
//
// Набор соответствия проверяет очистку и систему координат, но не рисование.
// Здесь — полный путь: формат вершин → конвейер → набор дескрипторов → команда
// отрисовки → чтение пикселей. Если сломано хоть одно звено, картинка выйдет
// не той, а сборка и валидация об этом молчат.
//
// Треугольник закрывает ЛЕВУЮ половину: так проверяется не только «что-то
// нарисовалось», но и ориентация — перевёрнутый по вертикали viewport дал бы
// ту же заливку, а перепутанные оси X/Y — нет.
TEST(Rhi_vulkan_draws_a_triangle_where_asked) {
    if (!GraphicsDevice::Available(Backend::Vulkan)) return;

    std::unique_ptr<GraphicsDevice> dev = GraphicsDevice::Create(Backend::Vulkan);
    dev->Init(nullptr);

    const std::string vert = R"(#version 330 core
layout(location = 0) in vec2 aPos;
uniform vec2 uOffset;
void main() { gl_Position = vec4(aPos + uOffset, 0.0, 1.0); }
)";
    const std::string frag = R"(#version 330 core
out vec4 FragColor;
uniform vec3 uColor;
void main() { FragColor = vec4(uColor, 1.0); }
)";

    std::unique_ptr<sage::rhi::ShaderProgram> shader = dev->CreateShaderProgram(vert, frag);
    CHECK_TRUE(shader != nullptr);
    if (!shader) return;

    sage::rhi::VertexLayout layout;
    layout.Stride = sizeof(float) * 2;
    layout.Attributes.push_back({0, 2, sage::rhi::AttribType::Float, 0});
    std::unique_ptr<sage::rhi::Geometry> geometry = dev->CreateGeometry(layout);
    CHECK_TRUE(geometry != nullptr);
    if (!geometry) return;

    // Треугольник, накрывающий левую половину экрана целиком.
    const float verts[] = {-1.0f, -3.0f, -1.0f, 3.0f, 0.0f, 0.0f};
    geometry->SetVertexData(verts, sizeof(verts), /*dynamic=*/false);

    constexpr int kW = 16, kH = 16;
    sage::rhi::RenderTargetDesc desc;
    desc.Width = kW;
    desc.Height = kH;
    desc.Kind = sage::rhi::RenderTargetKind::ColorHDR;
    std::unique_ptr<sage::rhi::RenderTarget> rt = dev->CreateRenderTarget(desc);
    CHECK_TRUE(rt != nullptr);
    if (!rt) return;

    rt->Bind();
    dev->SetScissor(false);
    dev->SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    dev->Clear(true, false);

    dev->SetDepthTest(false);
    dev->SetCullMode(sage::rhi::CullMode::Off);
    dev->SetBlend(false);
    shader->Use();
    shader->SetVec3("uColor", glm::vec3(0.0f, 1.0f, 0.0f));
    shader->SetVec2("uOffset", glm::vec2(0.0f, 0.0f));
    geometry->DrawArrays(3);

    std::vector<unsigned char> pixels((size_t)kW * kH * 3, 0);
    dev->ReadPixelsRGB(0, 0, kW, kH, pixels.data());

    auto green = [&](int col, int row) {
        const size_t i = ((size_t)row * kW + (size_t)col) * 3;
        return pixels[i] < 96 && pixels[i + 1] > 128 && pixels[i + 2] < 96;
    };

    // Левая половина закрашена, правая — нет, и так на обеих границах по высоте:
    // проверка одной строки прошла бы и при перевёрнутой картинке.
    CHECK_TRUE(green(1, 1));
    CHECK_TRUE(green(1, kH - 2));
    CHECK_FALSE(green(kW - 2, 1));
    CHECK_FALSE(green(kW - 2, kH - 2));

    dev->BindDefaultFramebuffer();
}
