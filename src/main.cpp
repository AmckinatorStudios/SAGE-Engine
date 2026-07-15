// ============================================================================
// Точка входа SAGE Engine.
//
// main() больше НЕ содержит игровой логики — только бутстрап: создать движок,
// создать игру, запустить. Вся игра (The Boat) живёт в src/game/TheBoat.*,
// реализуя интерфейс core/Game.h, а движок (core/Engine.*) её «крутит», ничего
// о ней не зная. Чтобы собрать другую игру на этом же движке — подставь сюда
// свой класс с Game-интерфейсом; код движка (core/render/scene/scripting/ui)
// не меняется.
//
// Порядок объявления важен: Engine РАНЬШЕ игры, чтобы окно/GL-контекст
// существовали к моменту загрузки ресурсов игры и пережили её разрушение
// (см. комментарий у класса Engine).
// ============================================================================
#include "core/Engine.h"
#include "core/Log.h"
#include "game/TheBoat.h"

#include <exception>
#include <iostream>

int main() {
    Log::Init("sage_engine.log");

    try {
        Engine engine(TheBoat::DefaultConfig()); // окно + GL-контекст
        TheBoat game(engine);                    // игра грузит свои ресурсы (контекст уже есть)
        engine.Run(game);                        // OnStart -> цикл -> OnShutdown
    } catch (const std::exception& e) {
        LOG_ERROR("Main") << "Фатальная ошибка: " << e.what();
        std::cerr << "Фатальная ошибка: " << e.what() << std::endl;
        return -1;
    }

    LOG_INFO("Main") << "Завершение работы";
    return 0;
}
