# example_stats — плагин-пример: панель с графиком времени кадра + пункт меню.
# Показывает полный цикл жизни плагина: load -> OnImGui каждый кадр -> unload.
sage_add_editor_plugin(
    NAME example_stats
    SOURCES ExampleStatsPlugin.cpp
)
