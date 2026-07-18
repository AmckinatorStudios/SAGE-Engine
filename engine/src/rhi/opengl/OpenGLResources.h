#pragma once
#include "sage/rhi/Resources.h"

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
    void SetVec4(const std::string& name, const glm::vec4& v) const override;
    void SetVec3(const std::string& name, const glm::vec3& v) const override;
    void SetVec2(const std::string& name, const glm::vec2& v) const override;
    void SetFloat(const std::string& name, float v) const override;
    void SetInt(const std::string& name, int v) const override;

private:
    static unsigned int Compile(unsigned int type, const std::string& source);
    unsigned int m_id = 0;
};

class GLGeometry : public Geometry {
public:
    explicit GLGeometry(const VertexLayout& layout);
    ~GLGeometry() override;

    void SetVertexData(const void* data, size_t bytes, bool dynamic) override;
    void SetIndexData(const unsigned int* indices, size_t count, bool dynamic) override;
    void SetInstanceData(const void* data, size_t bytes) override;

    void DrawIndexed(size_t indexCount) const override;
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
    unsigned int NativeHandle() const override { return m_id; }

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

class GLRenderTarget : public RenderTarget {
public:
    explicit GLRenderTarget(const RenderTargetDesc& desc);
    ~GLRenderTarget() override;

    void Bind() const override;
    void Resize(int width, int height) override;
    int Width() const override { return m_width; }
    int Height() const override { return m_height; }
    unsigned int ColorTextureHandle() const override { return m_colorTex; }
    unsigned int DepthTextureHandle() const override { return m_depthTex; }

private:
    void CreateStorage();
    void DestroyStorage();

    RenderTargetKind m_kind;
    int m_width = 0, m_height = 0;
    unsigned int m_fbo = 0;
    unsigned int m_colorTex = 0;  // ColorHDRWithDepth
    unsigned int m_depthRbo = 0;  // ColorHDRWithDepth
    unsigned int m_depthTex = 0;  // DepthOnly
};

} // namespace sage::rhi
