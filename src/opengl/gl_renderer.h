#ifndef CEL_OPENGL_GL_RENDERER_H
#define CEL_OPENGL_GL_RENDERER_H

#include <cstddef>

#include <GL/glew.h>    // it includes GL; must before glfw
#include <GLFW/glfw3.h>

#include "../camera.h"
#include "../scene.h"
#include "../model.h"
#include "../renderer.h" // base class

/*
 * opengl/renderer.h
 * Implement a OpenGL-based renderer.
 *
 * TODO: implement fantastic CEL shading
 * TODO: design some internal model format for rendering (for more fancy fx)
 */

namespace CEL {

class GLRenderer : public Renderer {

  // a VertexElement class to pass data in VS
  class Vertex {
    // TODO : position, color, normal, (material?)
  };

  // a uniform block data per model object
  // TODO add model transform
  struct ubPerObject {
    float m2w[4 * 4]; // modle to world transform
    float diffuse[4];

    ubPerObject(const Color& diff, const Mat4& w)
    : diffuse {diff.r, diff.g, diff.b, diff.a} {
      for (int i = 0; i < 16; ++i)
        m2w[i] = static_cast<float>(w(i % 4, i / 4));
    }
  };

  // a uniform block data per frame
  struct Light {
    float dir[3];
    float pad; // De we need this in GLSL ?
    float ambient[4];
    float diffuse[4];
  };
  struct ubPerFrame {
    float w2n[4 * 4]; // column-major in GLSL
    Light light;

    ubPerFrame(const Mat4& w2n_col_major) {
      for (int i = 0; i < 16; ++i)
        this->w2n[i] = static_cast<float>(w2n_col_major(i % 4, i / 4));
    }
  };

  // Holds GL objects related to a mesh
  // TODO: make a more automonous class (auto alloc/release GL resouces)
  struct BufferedMesh {
    const Mesh& mesh;
    ubPerObject meshUniformData; // world matrix, diffuse
    GLuint meshVAO;
    GLuint meshIndexBuffer;
    GLuint meshVertexBuffer;

    BufferedMesh(const Mesh& m, const Mat4& trans) : mesh(m),
      meshUniformData(mesh.get_material().diffuse, trans) {};

    Mesh::size_type indices_num() const {
      return mesh.get_triangles().size() * 3; }
  };

public:

  // Type Alias
  using Buffer = unsigned char*;

  // Default constructor
  GLRenderer();

  // Destructor
  ~GLRenderer() override;

  // Configurations
  void set_scene(std::shared_ptr<const Scene> scene) override;

  // Render request
  void set_buffer_size(BufferSize width, BufferSize height) override;
  void render() override;

  // Implementation specific interface
  void set_gl_context(GLFWwindow* w);
  void compile_shader();

private:

  // GL interface object
  GLFWwindow* window; // manged by GLViewer
  GLuint program; // object id for a unified pass-through shader

  // Uniform Buffer slot assignment
  GLuint perFrameBufferIndex = 0;
  GLuint perObjectBufferIndex = 1;

  // Rendering objects
  std::vector<BufferedMesh> meshes;
  GLuint perFrameBuffer;
  GLuint perObjectBuffer;

  // Implementation helpers
  void add_buffered_mesh(const Mesh& mesh, const Mat4& trans);
  void render_meshes();

};

} // namespace CEL

#endif
