#pragma once

// Константы, общие для нескольких игровых систем — вынесены сюда, чтобы
// не расходились между собой (было: PlayerController и GameState хранили
// два разных значения SeaLevel, и только конструктор GameState патчил одно
// поверх другого во время выполнения).
namespace GameConstants {
    constexpr float SeaLevel = 7.4f;
}
