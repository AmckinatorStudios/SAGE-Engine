#pragma once
#include <string>

// Сохраняет содержимое текущего кадра (back buffer) в PNG-файл.
// Вызывать ДО SwapBuffers(), пока нужный кадр ещё в back buffer'е.
// width/height — размер framebuffer'а окна.
void SaveScreenshot(const std::string& path, int width, int height);
