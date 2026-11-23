// Реализация stb_image и stb_image_write — одна на весь движок.
//
// Почему здесь, в основе. Картинки читают и пишут несколько слоёв: ресурсы
// (иконка и снимок игры в GameInfo), мир (текстуры, снимок экрана), скрипты.
// Реализация обязана лежать не выше самого нижнего из них: слои собираются
// отдельными библиотеками, и каждая видит только слои под собой. Раньше она
// жила в render (Texture.cpp, Screenshot.cpp), и ресурсы звали её снизу вверх.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
