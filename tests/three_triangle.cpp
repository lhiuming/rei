// Test color and z-buffer

#ifdef USE_MSVC
#pragma comment(linker, "/subsystem:windows /ENTRY:mainCRTStartup")
#endif

#include <memory>

#include <model.h>
#include <scene.h>
#include <camera.h>
#include <renderer.h>
#include <viewer.h>
#include <console.h>

using namespace std;
using namespace CEL;

int main()
{
  using Vertex = typename Mesh::Vertex;

  // Create the triangles
  Vertex v0({   10, 0, 0 }, {0.f, 0.f, 1.f}, { 0.9f, 0.6f, 0.1f, 0.0f });
  Vertex v1({   8, 10, 0 }, {0.f, 0.f, 1.f}, { 0.9f, 0.1f, 0.5f, 1.0f });
  Vertex v2({ -12,  0, 2 }, {0.2f, 0.f, .8f}, { 0.8f, 0.4f, 0.1f, 1.0f });
  Vertex v3({  0,   7, 0 }, {0.f, 0.f, 1.f}, { 0.4f, 0.8f, 0.0f, 0.0f });
  Vertex v4({ -8,   6, 0 }, {0.f, 0.f, 1.f}, { 0.7f, 0.8f, 0.1f, 1.0f });
  Vertex v5({  0, -13, 2 }, {0.f, 0.2f, .8f}, { 0.1f, 0.8f, 0.6f, 1.0f });
  Vertex v6({  5, -9, 0 }, {0.f, 0.f, 1.f}, { 0.4f, 0.1f, 0.7f, 1.0f });
  Vertex v7({  8,  4, 2 }, {0.f, 0.f, 1.f}, { 0.0f, 0.7f, 0.7f, 1.0f });
  Vertex v8({ -6, -6, 0 }, {0.f, 0.f, 1.f}, { 0.2f, 0.2f, 0.7f, 0.0f });

  Mesh mesh;
  mesh.set( {v0, v1, v2, v3, v4, v5, v6, v7, v8},
            { 0,  1,  2,  3,  4,  5,  6,  7,  8} );
  console << "Mesh model set up." << endl;

  // Set up the scene
  auto s = make_shared<StaticScene>();
  s->add_model(make_shared<Mesh>(std::move(mesh)), Mat4::I());
  console << "Scene set up. " << endl;

  // Set up the camera
  auto c = make_shared<Camera>(Vec3{0.0, 2.0, 20.0});
  c->set_aspect(720.0 / 480.0);
  console << "Camera set up." << endl;

  // Set up the Viewer and Renderer
  const int width = 720, height = 480;
  auto viewer = makeViewer(width, height, "Three Triangles");
  auto r = makeRenderer();  // FIXME: have to make viewer first for pixels.h implementation
  console << "Renderer created." << endl;
  viewer->set_camera(c);
  viewer->set_scene(s);
  viewer->set_renderer(r);
  console << "Viewer and Renderer set up." << endl;

  // run
  viewer->run();

  console << "Viewer stopped. Program ends." << endl;

  return 0;
}
