#pragma once

// ---------------------------------------------------------------------
// GameSounds — пути ко всем звукам The Boat в одном месте (как GameActions
// централизует раскладку). Остальной код игры ссылается на эти константы, а
// не на строковые литералы, разбросанные по обработчикам — чтобы поменять
// звук достаточно подменить файл в assets/audio/ или строку здесь.
//
// Сами звуки — процедурные плейсхолдеры (сгенерированы из тонов/шума,
// без стороннего копирайта); их можно спокойно заменить на «настоящие»,
// сохранив имена файлов.
// ---------------------------------------------------------------------
namespace GameSounds {

inline constexpr const char* Ocean    = "assets/audio/ocean.wav";     // зацикленный эмбиент моря
inline constexpr const char* Music    = "assets/audio/music.wav";     // фоновая музыка (стриминг)

inline constexpr const char* Splash   = "assets/audio/splash.wav";    // всплеск воды
inline constexpr const char* Pickup   = "assets/audio/pickup.wav";    // подбор мусора
inline constexpr const char* Break    = "assets/audio/break.wav";     // сломать блок
inline constexpr const char* Place    = "assets/audio/place.wav";     // поставить блок
inline constexpr const char* Craft    = "assets/audio/craft.wav";     // успешный крафт
inline constexpr const char* Error    = "assets/audio/error.wav";     // нельзя/не хватает
inline constexpr const char* Bite     = "assets/audio/bite.wav";      // рыба клюёт
inline constexpr const char* Cast     = "assets/audio/cast.wav";      // заброс удочки
inline constexpr const char* Eat      = "assets/audio/eat.wav";       // съесть/выпить
inline constexpr const char* CookDone = "assets/audio/cook_done.wav"; // рыба пожарена

} // namespace GameSounds
