#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>

// Пока вода — не воксельный блок, а один большой полупрозрачный квад на
// фиксированной высоте. Этого достаточно для ощущения "открытого моря" и
// сильно дешевле, чем честные воксели воды. Настоящую воксельную воду
// (с течениями/уровнями) добавим отдельно, когда понадобится геймплейно.
class WaterPlane {
public:
    WaterPlane(float halfSize, float seaLevel) {
        float y = seaLevel;
        float s = halfSize;
        float vertices[] = {
            -s, y, -s,
             s, y, -s,
             s, y,  s,
            -s, y,  s,
        };
        unsigned int indices[] = { 0, 2, 1, 2, 0, 3 };
        m_indexCount = 6;

        glGenVertexArrays(1, &m_vao);
        glGenBuffers(1, &m_vbo);
        glGenBuffers(1, &m_ebo);

        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glBindVertexArray(0);
    }

    ~WaterPlane() {
        glDeleteVertexArrays(1, &m_vao);
        glDeleteBuffers(1, &m_vbo);
        glDeleteBuffers(1, &m_ebo);
    }

    void Draw() const {
        glBindVertexArray(m_vao);
        glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }

private:
    unsigned int m_vao = 0, m_vbo = 0, m_ebo = 0;
    unsigned int m_indexCount = 0;
};
