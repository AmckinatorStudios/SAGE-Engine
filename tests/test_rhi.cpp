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
#include <cstdlib>
#include <memory>
#include <string>

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
