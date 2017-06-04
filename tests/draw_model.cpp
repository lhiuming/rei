// Test loading and drwing mesh

#ifdef USE_MSVC
#pragma comment(linker, "/subsystem:windows /ENTRY:mainCRTStartup")
#endif

#include <iostream>

#include <model.h>
#include <asset_loader.h>
#include <scene.h>
#include <camera.h>
#include <renderer.h>
#include <viewer.h>
#include <console.h>


using namespace std;
using namespace CEL;

int main(int argc, char** argv)
{
  // Read the .dae model
  string fn;
  if (argc > 1) {
    fn = string(argv[1]);
  } else {
    fn = string("color_cube.dae");
    console << "using default input file: " << fn << endl;
  }

  AssetLoader loader;
  auto meshes = loader.load_mesh(fn);
  console << "Model read. Get " << meshes.size() << " meshes. " << endl;

  // Check the model
  for(const auto& t : meshes[0]->get_triangles())
  {
    console << "triangle: " << endl;
    console << "  " << t.a->coord << ", " << t.a->color << endl;
    console << "  " << t.b->coord << ", " << t.b->color << endl;
    console << "  " << t.c->coord << ", " << t.c->color << endl;
  }
  // Set up the scene
  auto s = make_shared<StaticScene>();
  s->add_model(meshes[0], Mat4::I()); // push it as it-is
  console << "Scene set up. " << endl;

  // window size
  const int width = 720;
  const int height = 480;

  // Set up the camera
  auto c = make_shared<Camera>(Vec3{0, 0, 20}, Vec3{0, 0, -1});
  c->set_ratio((float)width / height);
  console << "Camera set up." << endl;

  // Set up the Viewer and Renderer
  auto v = makeViewer(width, height,
    "Three Triangle (testing color and z-buffer)");
  auto r = makeRenderer(); // must put after Viewer construction !
  v->set_camera(c);
  v->set_scene(s);
  v->set_renderer(r);
  console << "Viewer and Renderer set up." << endl;

  // run
  v->run();

  console << "Viewer stopped. Program ends." << endl;

  return 0;
}
