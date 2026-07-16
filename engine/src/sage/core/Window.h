#pragma once
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <string>
#include <functional>

// Обёртка над GLFW-окном + контекстом OpenGL
class Window {
public:
    Window(int width, int height, const std::string& title);
    ~Window();

    bool ShouldClose() const;
    void SwapBuffers();
    void PollEvents();

    GLFWwindow* Handle() const { return m_handle; }
    int Width() const { return m_width; }
    int Height() const { return m_height; }

    // Вызывается GLFW при изменении размера окна
    void OnResize(int width, int height);

private:
    GLFWwindow* m_handle = nullptr;
    int m_width;
    int m_height;
};
