#include "Window.h"
#include "Log.h"
#include <stdexcept>

static void FramebufferSizeCallback(GLFWwindow* handle, int width, int height) {
    auto* win = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (win) win->OnResize(width, height);
}

Window::Window(int width, int height, const std::string& title)
    : m_width(width), m_height(height) {

    if (!glfwInit()) {
        LOG_ERROR("Window") << "Не удалось инициализировать GLFW";
        throw std::runtime_error("Не удалось инициализировать GLFW");
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    m_handle = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_handle) {
        glfwTerminate();
        LOG_ERROR("Window") << "Не удалось создать окно GLFW";
        throw std::runtime_error("Не удалось создать окно GLFW");
    }

    glfwMakeContextCurrent(m_handle);
    glfwSetWindowUserPointer(m_handle, this);
    glfwSetFramebufferSizeCallback(m_handle, FramebufferSizeCallback);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        LOG_ERROR("Window") << "Не удалось инициализировать glad (OpenGL)";
        throw std::runtime_error("Не удалось инициализировать glad (OpenGL)");
    }

    glViewport(0, 0, width, height);
    glEnable(GL_DEPTH_TEST);

    // Backface culling: не рисуем грани треугольников, обращённые от камеры.
    // Особенно важно для вокселей — у каждого видимого блока обычно не все
    // 6 граней смотрят на камеру, отсечение "невидимых" граней экономит
    // значительную часть работы GPU. Все меши движка (Mesh, VoxelMesh,
    // WaterPlane) используют CCW-порядок вершин при взгляде снаружи/сверху.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    // Без этого на стыках граней кубических (cubemap) текстур — то есть у
    // skybox — видны швы: резкий скачок цвета ровно на границе между
    // гранями вместо плавного перехода. Драйвер не интерполирует между
    // соседними гранями куба, если это явно не включено.
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    LOG_INFO("Window") << "OpenGL версия: " << glGetString(GL_VERSION);
}

Window::~Window() {
    if (m_handle) glfwDestroyWindow(m_handle);
    glfwTerminate();
}

bool Window::ShouldClose() const {
    return glfwWindowShouldClose(m_handle);
}

void Window::SwapBuffers() {
    glfwSwapBuffers(m_handle);
}

void Window::PollEvents() {
    glfwPollEvents();
}

void Window::OnResize(int width, int height) {
    m_width = width;
    m_height = height;
    glViewport(0, 0, width, height);
}
