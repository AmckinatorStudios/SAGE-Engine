#pragma once
#include <ctime>
#include <string>

// Сохраняет содержимое текущего кадра (back buffer или привязанного буфера) в
// PNG-файл. Вызывать ДО SwapBuffers(), пока нужный кадр ещё в back buffer'е.
// width/height — размер читаемого буфера. Папка создаётся при надобности.
// false — не удалось (нет прав, нет места); причина уходит в лог.
bool SaveScreenshot(const std::string& path, int width, int height);

namespace sage::render {

// Имя снимка по времени: <dir>/ГГГГ-ММ-ДД_ЧЧ.ММ.СС.png. Если такой файл уже
// есть (два снимка за секунду) — _2, _3… Этот же вид имён у снимков большинства
// игр, и папку можно отсортировать по имени — получится по времени.
std::string ScreenshotFileName(const std::string& dir, const std::tm& when);
// То же для текущего местного времени.
std::string TimestampedScreenshotPath(const std::string& dir);

} // namespace sage::render
