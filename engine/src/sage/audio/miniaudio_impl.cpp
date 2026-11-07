// Единственная единица трансляции, в которой разворачивается реализация
// miniaudio (~90k строк: декодеры WAV/FLAC/MP3, микшер, бэкенды устройств).
// Вынесена отдельно от AudioEngine.cpp специально: правки логики звука не
// должны заставлять перекомпилировать весь бэкенд. Аналогично тому, как
// реализация stb_image включается в одном месте (см. render/Texture.cpp).
//
// OGG VORBIS. Своего декодера Vorbis у miniaudio нет: он берёт stb_vorbis,
// если тот подключён ВОКРУГ реализации — объявления до неё, тело после
// (порядок из документации miniaudio, раздел «Vorbis»). Без этого .ogg —
// самый распространённый формат музыки игр — молча не открывался: файл
// есть, а звука нет.
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
