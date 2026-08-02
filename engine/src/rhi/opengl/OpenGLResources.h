#pragma once
#include "sage/rhi/Resources.h"
#include <unordered_map>

// OpenGL-реализации RHI-ресурсов. Вместе с OpenGLDevice это ЕДИНСТВЕННОЕ место
// движка, где включается glad и живут вызовы GL — остальной код (включая игры)
// работает только с интерфейсами sage/rhi/*.
namespace sage::rhi {

class GLShaderProgram : public ShaderProgram {
public:
    GLShaderProgram(const std::string& vertexSrc, const std::string& fragmentSrc);
    ~GLShaderProgram() override;

    void Use() const override;
    void SetMat4(const std::string& name, const glm::mat4& v) const override;
    void SetMat4Array(const std::string& name, const glm::mat4* v, int count) const override;
    void SetIntArray(const std::string& name, const int* v, int count) const override;
    void SetFloatArray(const std::string& name, const float* v, int count) const override;
    void SetVec4(const std::string& name, const glm::vec4& v) const override;
    void SetVec3(const std::string& name, const glm::vec3& v) const override;
    void SetVec2(const std::string& name, const glm::vec2& v) const override;
    void SetFloat(const std::string& name, float v) const override;
    void SetInt(const std::string& name, int v) const override;

private:
    static unsigned int Compile(unsigned int type, const std::string& source);

    // Локация униформы с кэшированием. glGetUniformLocation — обращение к
    // драйверу со строковым поиском; в горячем пути его звали НА КАЖДУЮ
    // установку униформы (освещение — ~30 установок на проход, редактор — три
    // прохода в кадр). Программа неизменяема после линковки, так что локации
    // можно запомнить навсегда. mutable — Set* методы const.
    int Loc(const std::string& name) const;

    unsigned int m_id = 0;
    mutable std::unordered_map<std::string, int> m_uniformLocations;
};

class GLGeometry : public Geometry {
public:
    explicit GLGeometry(const VertexLayout& layout);
    ~GLGeometry() override;

    void SetVertexData(const void* data, size_t bytes, bool dynamic) override;
    void SetIndexData(const unsigned int* indices, size_t count, bool dynamic) override;
    void SetInstanceData(const void* data, size_t bytes) override;

    void DrawIndexed(size_t indexCount) const override;
    void DrawIndexedRange(size_t firstIndex, size_t indexCount) const override;
    void DrawArrays(size_t vertexCount) const override;
    void DrawInstanced(size_t vertexCount, size_t instanceCount) const override;
    void DrawIndexedInstanced(size_t indexCount, size_t instanceCount) const override;
    void DrawLines(size_t vertexCount) const override;

private:
    unsigned int m_vao = 0, m_vbo = 0, m_ebo = 0, m_instanceVbo = 0;
};

class GLTexture2D : public Texture2D {
public:
    GLTexture2D(const Texture2DDesc& desc, const void* pixels);
    ~GLTexture2D() override;

    void Bind(int unit) const override;
    sage::rhi::TextureHandle Handle() const override { return {m_id}; }
    uint64_t NativeHandle() const override { return m_id; }

private:
    unsigned int m_id = 0;
};

class GLTexture3D : public Texture3D {
public:
    GLTexture3D(const Texture3DDesc& desc, const float* pixels);
    ~GLTexture3D() override;

    void Bind(int unit) const override;

private:
    unsigned int m_id = 0;
};

class GLTextureCube : public TextureCube {
public:
    explicit GLTextureCube(const CubeFacePixels faces[6]);
    ~GLTextureCube() override;

    void Bind(int unit) const override;

private:
    unsigned int m_id = 0;
};

// Цветной cubemap с мипами, в грани которого можно рисовать. Формат — RGB16F:
// отражение обязано нести HDR. Небо ярче единицы, и в LDR-кубе солнце и блик
// на воде превратились бы в плоское белое пятно ещё до тон-маппинга.
class GLCubeRenderTarget : public CubeRenderTarget {
public:
    explicit GLCubeRenderTarget(const CubeRenderTargetDesc& desc);
    ~GLCubeRenderTarget() override;

    void BindFace(int face, int mip) override;
    void GenerateMips() override;
    int Size() const override { return m_size; }
    int MipLevels() const override { return m_mips; }
    void Bind(int unit) const override;
    sage::rhi::TextureHandle Handle() const override { return {m_cube}; }

private:
    int m_size = 0;
    int m_mips = 1;
    unsigned int m_cube = 0;
    unsigned int m_fbo = 0;
    unsigned int m_depthRbo = 0;
    int m_depthSize = 0;   // под какой размер выделена глубина сейчас
};

class GLRenderTarget : public RenderTarget {
public:
    explicit GLRenderTarget(const RenderTargetDesc& desc);
    ~GLRenderTarget() override;

    void Bind() const override;
    void Resize(int width, int height) override;
    int Width() const override { return m_width; }
    int Height() const override { return m_height; }
    sage::rhi::TextureHandle ColorTextureHandle() const override { return {m_colorTex}; }
    sage::rhi::TextureHandle DepthTextureHandle() const override { return {m_depthTex}; }
    uint64_t NativeColorHandle() const override { return m_colorTex; }
    void Resolve() override;
    int Samples() const override { return m_samples; }

private:
    void CreateStorage();
    void DestroyStorage();

    RenderTargetKind m_kind;
    int m_width = 0, m_height = 0;
    int m_samples = 1;
    unsigned int m_fbo = 0;
    unsigned int m_colorTex = 0;  // ColorHDRWithDepth
    unsigned int m_depthRbo = 0;  // ColorHDRWithDepth
    unsigned int m_depthTex = 0;  // DepthOnly
    // MSAA: рисуем в многосэмпловый FBO, а наружу отдаём обычные текстуры выше.
    // Сэмплировать многосэмпловую текстуру обычным sampler2D нельзя, а весь
    // пост-процесс написан именно под него — поэтому нужен явный перенос
    // (glBlitFramebuffer), а не «просто ещё один формат вложения».
    unsigned int m_msFbo = 0;
    unsigned int m_msColor = 0;   // многосэмпловый renderbuffer цвета
    unsigned int m_msDepth = 0;   // многосэмпловый renderbuffer глубины
};

} // namespace sage::rhi
