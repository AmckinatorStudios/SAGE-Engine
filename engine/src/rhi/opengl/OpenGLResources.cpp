#include "rhi/opengl/OpenGLResources.h"
#include "sage/core/Log.h"
#include "sage/core/Stats.h"
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>
#include <stdexcept>

namespace sage::rhi {

// ============================================================================
//  GLShaderProgram
// ============================================================================

unsigned int GLShaderProgram::Compile(unsigned int type, const std::string& source) {
    unsigned int id = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(id, 1, &src, nullptr);
    glCompileShader(id);

    int success;
    glGetShaderiv(id, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info[1024];
        glGetShaderInfoLog(id, 1024, nullptr, info);
        std::string kind = (type == GL_VERTEX_SHADER) ? "VERTEX" : "FRAGMENT";
        LOG_ERROR("RHI") << "Ошибка компиляции шейдера (" << kind << "): " << info;
        throw std::runtime_error("Ошибка компиляции шейдера (" + kind + "): " + info);
    }
    return id;
}

GLShaderProgram::GLShaderProgram(const std::string& vertexSrc, const std::string& fragmentSrc) {
    unsigned int vShader = Compile(GL_VERTEX_SHADER, vertexSrc);
    unsigned int fShader = Compile(GL_FRAGMENT_SHADER, fragmentSrc);

    m_id = glCreateProgram();
    glAttachShader(m_id, vShader);
    glAttachShader(m_id, fShader);
    glLinkProgram(m_id);

    int success;
    glGetProgramiv(m_id, GL_LINK_STATUS, &success);
    if (!success) {
        char info[1024];
        glGetProgramInfoLog(m_id, 1024, nullptr, info);
        LOG_ERROR("RHI") << "Ошибка линковки шейдерной программы: " << info;
        throw std::runtime_error(std::string("Ошибка линковки шейдерной программы: ") + info);
    }

    glDeleteShader(vShader);
    glDeleteShader(fShader);
}

GLShaderProgram::~GLShaderProgram() {
    if (m_id) glDeleteProgram(m_id);
}

void GLShaderProgram::Use() const { glUseProgram(m_id); }

void GLShaderProgram::SetMat4(const std::string& name, const glm::mat4& v) const {
    glUniformMatrix4fv(glGetUniformLocation(m_id, name.c_str()), 1, GL_FALSE, glm::value_ptr(v));
}
void GLShaderProgram::SetVec4(const std::string& name, const glm::vec4& v) const {
    glUniform4fv(glGetUniformLocation(m_id, name.c_str()), 1, glm::value_ptr(v));
}
void GLShaderProgram::SetVec3(const std::string& name, const glm::vec3& v) const {
    glUniform3fv(glGetUniformLocation(m_id, name.c_str()), 1, glm::value_ptr(v));
}
void GLShaderProgram::SetVec2(const std::string& name, const glm::vec2& v) const {
    glUniform2fv(glGetUniformLocation(m_id, name.c_str()), 1, glm::value_ptr(v));
}
void GLShaderProgram::SetFloat(const std::string& name, float v) const {
    glUniform1f(glGetUniformLocation(m_id, name.c_str()), v);
}
void GLShaderProgram::SetInt(const std::string& name, int v) const {
    glUniform1i(glGetUniformLocation(m_id, name.c_str()), v);
}

// ============================================================================
//  GLGeometry
// ============================================================================

namespace {
void ApplyAttributes(const std::vector<VertexAttribute>& attrs, int stride, bool perInstance) {
    for (const VertexAttribute& a : attrs) {
        glEnableVertexAttribArray(a.Location);
        switch (a.Type) {
            case AttribType::Float:
                glVertexAttribPointer(a.Location, a.Components, GL_FLOAT, GL_FALSE,
                                      stride, (const void*)(intptr_t)a.Offset);
                break;
            case AttribType::UByteNorm:
                glVertexAttribPointer(a.Location, a.Components, GL_UNSIGNED_BYTE, GL_TRUE,
                                      stride, (const void*)(intptr_t)a.Offset);
                break;
        }
        if (perInstance) glVertexAttribDivisor(a.Location, 1); // атрибут продвигается раз на инстанс
    }
}
} // namespace

GLGeometry::GLGeometry(const VertexLayout& layout) {
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);

    // Указатели атрибутов можно настраивать до заливки данных — буфер лишь
    // должен быть привязан. Данные придут позже через SetVertexData.
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    ApplyAttributes(layout.Attributes, layout.Stride, /*perInstance=*/false);

    if (!layout.InstanceAttributes.empty()) {
        glGenBuffers(1, &m_instanceVbo);
        glBindBuffer(GL_ARRAY_BUFFER, m_instanceVbo);
        ApplyAttributes(layout.InstanceAttributes, layout.InstanceStride, /*perInstance=*/true);
    }

    glBindVertexArray(0);
}

GLGeometry::~GLGeometry() {
    if (m_instanceVbo) glDeleteBuffers(1, &m_instanceVbo);
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
}

void GLGeometry::SetVertexData(const void* data, size_t bytes, bool dynamic) {
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, data, dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
}

void GLGeometry::SetIndexData(const unsigned int* indices, size_t count, bool dynamic) {
    // Привязка EBO — часть состояния VAO, поэтому заливаем при привязанном VAO.
    glBindVertexArray(m_vao);
    if (!m_ebo) glGenBuffers(1, &m_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(count * sizeof(unsigned int)), indices,
                 dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    glBindVertexArray(0);
}

void GLGeometry::SetInstanceData(const void* data, size_t bytes) {
    glBindBuffer(GL_ARRAY_BUFFER, m_instanceVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, data, GL_STREAM_DRAW); // перезаливается каждый кадр
}

void GLGeometry::DrawIndexed(size_t indexCount) const {
    ++g_renderStats.DrawCalls;
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, (GLsizei)indexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

void GLGeometry::DrawArrays(size_t vertexCount) const {
    ++g_renderStats.DrawCalls;
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)vertexCount);
    glBindVertexArray(0);
}

void GLGeometry::DrawInstanced(size_t vertexCount, size_t instanceCount) const {
    ++g_renderStats.DrawCalls;
    glBindVertexArray(m_vao);
    glDrawArraysInstanced(GL_TRIANGLES, 0, (GLsizei)vertexCount, (GLsizei)instanceCount);
    glBindVertexArray(0);
}

void GLGeometry::DrawLines(size_t vertexCount) const {
    ++g_renderStats.DrawCalls;
    glBindVertexArray(m_vao);
    glDrawArrays(GL_LINES, 0, (GLsizei)vertexCount);
    glBindVertexArray(0);
}

// ============================================================================
//  GLTexture2D
// ============================================================================

// Токены расширения GL_EXT_texture_filter_anisotropic: core только с GL 4.6,
// а glad сгенерирован под 3.3 — определяем сами (функции не нужны, это просто
// enum-значения для glTexParameterf/glGetFloatv).
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

float QueryMaxAnisotropy() {
    static float cached = -1.0f;
    if (cached < 0.0f) {
        GLfloat value = 0.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &value);
        cached = (value >= 1.0f) ? value : 1.0f; // расширение не поддерживается — анизотропия выключена
    }
    return cached;
}

namespace {
GLenum FormatFromChannels(int channels) {
    if (channels == 1) return GL_RED;
    if (channels == 4) return GL_RGBA;
    return GL_RGB;
}

void ApplyFilter2D(Filter filter, bool mipmaps) {
    GLenum magFilter = (filter == Filter::Nearest) ? GL_NEAREST : GL_LINEAR;

    GLenum minFilter;
    if (!mipmaps) {
        minFilter = magFilter; // без мипмапов MIPMAP-варианты недоступны
    } else {
        switch (filter) {
            case Filter::Nearest:  minFilter = GL_NEAREST_MIPMAP_NEAREST; break;
            case Filter::Bilinear: minFilter = GL_LINEAR_MIPMAP_NEAREST; break;
            case Filter::Trilinear:
            case Filter::Anisotropic:
            default: minFilter = GL_LINEAR_MIPMAP_LINEAR; break;
        }
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);

    if (filter == Filter::Anisotropic && mipmaps) {
        float level = std::min(4.0f, QueryMaxAnisotropy()); // 4x — баланс качество/цена
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, level);
    }
}
} // namespace

GLTexture2D::GLTexture2D(const Texture2DDesc& desc, const void* pixels) {
    GLenum format = FormatFromChannels(desc.Channels);

    glGenTextures(1, &m_id);
    glBindTexture(GL_TEXTURE_2D, m_id);
    glTexImage2D(GL_TEXTURE_2D, 0, format, desc.Width, desc.Height, 0, format, GL_UNSIGNED_BYTE, pixels);
    if (desc.GenerateMipmaps) glGenerateMipmap(GL_TEXTURE_2D);

    ApplyFilter2D(desc.FilterMode, desc.GenerateMipmaps);
    GLenum wrap = (desc.WrapMode == Wrap::ClampEdge) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
}

GLTexture2D::~GLTexture2D() {
    if (m_id) glDeleteTextures(1, &m_id);
}

void GLTexture2D::Bind(int unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, m_id);
}

// ============================================================================
//  GLTextureCube
// ============================================================================

GLTextureCube::GLTextureCube(const CubeFacePixels faces[6]) {
    glGenTextures(1, &m_id);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_id);

    for (int i = 0; i < 6; ++i) {
        GLenum format = FormatFromChannels(faces[i].Channels);
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, format,
                     faces[i].Width, faces[i].Height, 0, format, GL_UNSIGNED_BYTE, faces[i].Pixels);
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
}

GLTextureCube::~GLTextureCube() {
    if (m_id) glDeleteTextures(1, &m_id);
}

void GLTextureCube::Bind(int unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_id);
}

// ============================================================================
//  GLRenderTarget
// ============================================================================

GLRenderTarget::GLRenderTarget(const RenderTargetDesc& desc)
    : m_kind(desc.Kind), m_width(desc.Width), m_height(desc.Height) {
    CreateStorage();
}

GLRenderTarget::~GLRenderTarget() {
    DestroyStorage();
}

void GLRenderTarget::CreateStorage() {
    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

    if (m_kind == RenderTargetKind::ColorHDRWithDepth) {
        // Цвет — float-текстура (HDR: значения не обрезаются в [0,1], что
        // нужно тон-маппингу). Глубина — renderbuffer (читать её не нужно).
        glGenTextures(1, &m_colorTex);
        glBindTexture(GL_TEXTURE_2D, m_colorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_width, m_height, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_colorTex, 0);

        glGenRenderbuffers(1, &m_depthRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, m_depthRbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, m_width, m_height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depthRbo);
    } else { // DepthOnly (карта теней)
        glGenTextures(1, &m_depthTex);
        glBindTexture(GL_TEXTURE_2D, m_depthTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, m_width, m_height, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // За пределами таргета считаем «полностью освещено»: граничный тексель
        // глубины = 1.0, сравнение никогда не даст тень вне покрытия.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        float border[] = {1.0f, 1.0f, 1.0f, 1.0f};
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_depthTex, 0);
        // Без color-attachment FBO считается неполным, пока явно не отключить
        // чтение/запись цвета.
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("RHI") << "Рендер-таргет неполный (incomplete framebuffer)";
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GLRenderTarget::DestroyStorage() {
    if (m_colorTex) glDeleteTextures(1, &m_colorTex);
    if (m_depthTex) glDeleteTextures(1, &m_depthTex);
    if (m_depthRbo) glDeleteRenderbuffers(1, &m_depthRbo);
    if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
    m_colorTex = m_depthTex = m_depthRbo = m_fbo = 0;
}

void GLRenderTarget::Bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, m_width, m_height);
}

void GLRenderTarget::Resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;

    // Перевыделяем только хранилища вложений — FBO и параметры сохраняются.
    if (m_kind == RenderTargetKind::ColorHDRWithDepth) {
        glBindTexture(GL_TEXTURE_2D, m_colorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
        glBindRenderbuffer(GL_RENDERBUFFER, m_depthRbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    } else {
        glBindTexture(GL_TEXTURE_2D, m_depthTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    }
}

} // namespace sage::rhi
