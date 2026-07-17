// Единственная единица трансляции, разворачивающая реализацию stb_truetype
// (как miniaudio_impl.cpp для miniaudio). Заголовок stb_truetype.h вендорится
// в external/stb и подключается как обычный header остальными файлами.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
