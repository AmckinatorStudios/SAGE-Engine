#include "rhi/opengl/OpenGLDevice.h"
#include "rhi/opengl/OpenGLResources.h"

#include <algorithm>
#include <cstring>
#include <glad/glad.h>

// ---------------------------------------------------------------------------
// OpenGL: то, что нужно своему рендеру сверх штатного, — программы любых
// стадий без исключений, вычислительные шейдеры, буферы шейдеров, запись в
// текстуры.
//
// Вычисления в OpenGL — это 4.3. Контекст движок просит 3.3 core, и драйверы
// Linux/Windows на такую просьбу отдают старший совместимый (4.5/4.6), а macOS
// останавливается на 4.1. Поэтому всё ниже спрашивает версию У КОНТЕКСТА, а
// не у сборки: на одной и той же сборке игры вычисления есть у одного игрока
// и нет у другого, и честный ответ — в Capabilities().
// ---------------------------------------------------------------------------
namespace sage::rhi {
namespace {

// Только по версии ядра: расширения ARB в загрузчик (glad) не входят, а
// контекст, где вычисления есть лишь расширением, — редкость прошлого
// десятилетия.
bool HasCompute() { return GLAD_GL_VERSION_4_3 != 0; }
bool HasStorage() { return GLAD_GL_VERSION_4_3 != 0; }
bool HasImages() { return GLAD_GL_VERSION_4_2 != 0; }

GLenum StageEnum(ShaderStage s) {
    switch (s) {
        case ShaderStage::Vertex: return GL_VERTEX_SHADER;
        case ShaderStage::Fragment: return GL_FRAGMENT_SHADER;
        case ShaderStage::Geometry: return GL_GEOMETRY_SHADER;
        case ShaderStage::Compute: return GL_COMPUTE_SHADER;
    }
    return GL_VERTEX_SHADER;
}

std::string ShaderLog(GLuint id) {
    GLint len = 0;
    glGetShaderiv(id, GL_INFO_LOG_LENGTH, &len);
    std::string out((size_t)std::max(len, 1), '\0');
    glGetShaderInfoLog(id, len, nullptr, out.data());
    while (!out.empty() && (out.back() == '\0' || out.back() == '\n')) out.pop_back();
    return out;
}

std::string ProgramLog(GLuint id) {
    GLint len = 0;
    glGetProgramiv(id, GL_INFO_LOG_LENGTH, &len);
    std::string out((size_t)std::max(len, 1), '\0');
    glGetProgramInfoLog(id, len, nullptr, out.data());
    while (!out.empty() && (out.back() == '\0' || out.back() == '\n')) out.pop_back();
    return out;
}

GLenum BufferTarget(BufferUsage u) {
    return u == BufferUsage::Uniform ? GL_UNIFORM_BUFFER : GL_SHADER_STORAGE_BUFFER;
}

} // namespace

std::unique_ptr<ShaderProgram> BuildGLProgram(const ShaderProgramDesc& desc, ShaderBuildLog& log) {
    log = {};
    bool compute = false, graphics = false;
    for (const ShaderStageSource& st : desc.Stages) {
        if (st.Stage == ShaderStage::Compute) compute = true;
        else graphics = true;
    }
    if (compute && graphics) {
        log.Entries.push_back({ShaderStage::Compute, true,
                               "вычислительную стадию нельзя смешивать с графическими"});
        return nullptr;
    }
    if (desc.Stages.empty()) {
        log.Entries.push_back({ShaderStage::Vertex, true, "у программы нет ни одной стадии"});
        return nullptr;
    }
    if (compute && !HasCompute()) {
        log.Entries.push_back({ShaderStage::Compute, false,
                               "вычислительные шейдеры требуют OpenGL 4.3, у контекста их нет"});
        return nullptr;
    }

    std::vector<GLuint> shaders;
    bool ok = true;
    for (const ShaderStageSource& st : desc.Stages) {
        const GLuint id = glCreateShader(StageEnum(st.Stage));
        const char* src = st.Code.c_str();
        glShaderSource(id, 1, &src, nullptr);
        glCompileShader(id);
        GLint success = 0;
        glGetShaderiv(id, GL_COMPILE_STATUS, &success);
        if (!success) {
            ok = false;
            log.Entries.push_back({st.Stage, false, ShaderLog(id)});
        } else if (std::string warn = ShaderLog(id); !warn.empty()) {
            // Предупреждения тоже в журнал: «переменная не используется» —
            // частая причина униформы, которая «не доходит».
            log.Entries.push_back({st.Stage, false, warn});
        }
        shaders.push_back(id);
    }
    if (!ok) {
        for (GLuint id : shaders) glDeleteShader(id);
        return nullptr;
    }

    const GLuint program = glCreateProgram();
    for (GLuint id : shaders) glAttachShader(program, id);
    glLinkProgram(program);
    for (GLuint id : shaders) {
        glDetachShader(program, id);
        glDeleteShader(id);
    }
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        log.Entries.push_back({desc.Stages.front().Stage, true, ProgramLog(program)});
        glDeleteProgram(program);
        return nullptr;
    }
    log.Ok = true;
    return std::make_unique<GLShaderProgram>(GLShaderProgram::AdoptTag{}, program);
}

// ============================================================================
//  GLGpuBuffer
// ============================================================================

GLGpuBuffer::GLGpuBuffer(const BufferDesc& desc, const void* data)
    : m_size(desc.Bytes), m_usage(desc.Usage) {
    const GLenum target = BufferTarget(m_usage);
    glGenBuffers(1, &m_id);
    glBindBuffer(target, m_id);
    glBufferData(target, (GLsizeiptr)m_size, data, desc.Dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    glBindBuffer(target, 0);
}

GLGpuBuffer::~GLGpuBuffer() {
    if (m_id) glDeleteBuffers(1, &m_id);
}

bool GLGpuBuffer::Update(const void* data, size_t bytes, size_t offset) {
    if (offset + bytes > m_size) return false;
    const GLenum target = BufferTarget(m_usage);
    glBindBuffer(target, m_id);
    glBufferSubData(target, (GLintptr)offset, (GLsizeiptr)bytes, data);
    glBindBuffer(target, 0);
    return true;
}

bool GLGpuBuffer::Read(void* out, size_t bytes, size_t offset) const {
    if (offset + bytes > m_size) return false;
    const GLenum target = BufferTarget(m_usage);
    glBindBuffer(target, m_id);
    const void* p = glMapBufferRange(target, (GLintptr)offset, (GLsizeiptr)bytes, GL_MAP_READ_BIT);
    if (p) std::memcpy(out, p, bytes);
    glUnmapBuffer(target);
    glBindBuffer(target, 0);
    return p != nullptr;
}

// ============================================================================
//  OpenGLDevice: свой рендер
// ============================================================================

GraphicsDevice::Caps OpenGLDevice::Capabilities() const {
    Caps c;
    c.ComputeShaders = HasCompute();
    c.StorageBuffers = HasStorage();
    c.ImageLoadStore = HasImages();
    c.GeometryShaders = GLAD_GL_VERSION_3_2 != 0;
    auto get = [](GLenum what) {
        GLint v = 0;
        glGetIntegerv(what, &v);
        return (int)v;
    };
    if (c.ComputeShaders) {
        for (int i = 0; i < 3; ++i)
            glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, (GLuint)i, &c.MaxComputeGroupSize[i]);
        c.MaxComputeInvocations = get(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS);
    }
    if (c.StorageBuffers) c.MaxStorageBindings = get(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS);
    c.MaxUniformBindings = get(GL_MAX_UNIFORM_BUFFER_BINDINGS);
    c.MaxTextureUnits = get(GL_MAX_TEXTURE_IMAGE_UNITS);
    return c;
}

std::unique_ptr<ShaderProgram> OpenGLDevice::CreateProgram(const ShaderProgramDesc& desc,
                                                           ShaderBuildLog& log) {
    return BuildGLProgram(desc, log);
}

std::unique_ptr<GpuBuffer> OpenGLDevice::CreateBuffer(const BufferDesc& desc, const void* data) {
    if (desc.Bytes == 0) return nullptr;
    if (desc.Usage == BufferUsage::Storage && !HasStorage()) return nullptr;
    return std::make_unique<GLGpuBuffer>(desc, data);
}

void OpenGLDevice::BindBuffer(int slot, const GpuBuffer* buffer) {
    const auto* gl = static_cast<const GLGpuBuffer*>(buffer);
    const GLenum target = gl ? BufferTarget(gl->Usage()) : GL_SHADER_STORAGE_BUFFER;
    if (target == GL_SHADER_STORAGE_BUFFER && !HasStorage()) return;
    glBindBufferBase(target, (GLuint)slot, gl ? gl->Id() : 0);
}

void OpenGLDevice::BindImage(int unit, TextureHandle texture, ImageAccess access,
                             ImageFormat format, int mip) {
    if (!HasImages()) return;
    GLenum acc = access == ImageAccess::Read ? GL_READ_ONLY
                 : access == ImageAccess::Write ? GL_WRITE_ONLY : GL_READ_WRITE;
    GLenum fmt = GL_RGBA8;
    switch (format) {
        case ImageFormat::RGBA8: fmt = GL_RGBA8; break;
        case ImageFormat::RGBA16F: fmt = GL_RGBA16F; break;
        case ImageFormat::RGBA32F: fmt = GL_RGBA32F; break;
        case ImageFormat::R32F: fmt = GL_R32F; break;
        case ImageFormat::R32UI: fmt = GL_R32UI; break;
    }
    glBindImageTexture((GLuint)unit, (GLuint)texture.Value, mip, GL_FALSE, 0, acc, fmt);
}

void OpenGLDevice::Dispatch(int groupsX, int groupsY, int groupsZ) {
    if (!HasCompute() || groupsX < 1 || groupsY < 1 || groupsZ < 1) return;
    glDispatchCompute((GLuint)groupsX, (GLuint)groupsY, (GLuint)groupsZ);
}

void OpenGLDevice::ShaderBarrier(BarrierBits bits) {
    if (!HasImages() && !HasStorage()) return;
    const uint32_t b = (uint32_t)bits;
    GLbitfield mask = 0;
    if (b == (uint32_t)BarrierBits::All) {
        mask = GL_ALL_BARRIER_BITS;
    } else {
        if (b & (uint32_t)BarrierBits::Storage) mask |= GL_SHADER_STORAGE_BARRIER_BIT;
        if (b & (uint32_t)BarrierBits::Image) mask |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
        if (b & (uint32_t)BarrierBits::Texture) mask |= GL_TEXTURE_FETCH_BARRIER_BIT;
        if (b & (uint32_t)BarrierBits::VertexInput)
            mask |= GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT;
    }
    if (mask) glMemoryBarrier(mask);
}

} // namespace sage::rhi
