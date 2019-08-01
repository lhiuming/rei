#include "hybrid.h"

#include <memory>
#include <unordered_map>
#include <vector>

#include "../container_utils.h"
#include "../direct3d/d3d_renderer.h"

using std::vector;

namespace rei {

namespace hybrid {

struct ViewportProxy {
  size_t width = 1;
  size_t height = 1;
  SwapchainHandle swapchain;
  BufferHandle depth_stencil_buffer;
  BufferHandle gbuffer0;
  BufferHandle gbuffer1;
  BufferHandle gbuffer2;

  ShaderArgumentHandle base_shading_inout_arg;
  ShaderArgumentHandle direct_lighting_inout_arg;

  // Multi-bounce GI
  BufferHandle raytracing_output_buffer;

  // Statochastic Shadowed area lighting
  struct AreaLightHandles {
    BufferHandle unshadowed;
    BufferHandle stochastic_ray;
    BufferHandle stochastic_unshadowed;
    BufferHandle stochastic_shadowed;
    // BufferHandle stochastic_variance;
    BufferHandle denoised_0;
    ShaderArgumentHandle unshadowed_pass_arg;
    ShaderArgumentHandle sample_gen_pass_arg;
    ShaderArgumentHandle trace_pass_arg;
    // ShaderArgumentHandle variance_pass_arg;
    // ShaderArgumentHandle variance_filter_pass_arg;
    ShaderArgumentHandle denoise_horizontal_pass_arg;
    ShaderArgumentHandle denoise_final_pass_arg;

    // debug pass
    ShaderArgumentHandle blit_unshadowed;
  } area_light;

  // TAA resources
  BufferHandle taa_cb;
  BufferHandle taa_buffer[2];
  ShaderArgumentHandle taa_argument[2];

  BufferHandle deferred_shading_output;
  ShaderArgumentHandle blit_for_present;

  Vec4 cam_pos = {0, 1, 8, 1};
  Mat4 view_proj = Mat4::I();
  Mat4 view_proj_inv = Mat4::I();
  byte frame_id = 0;
  bool view_proj_dirty = true;

  void advance_frame() {
    frame_id++;
    if (frame_id == 0) frame_id += 2;
    // reset dirty mark
    view_proj_dirty = false;
  }

  ShaderArgumentHandle taa_curr_arg() { return taa_argument[frame_id % 2]; }
  BufferHandle taa_curr_input() { return taa_buffer[frame_id % 2]; }
  BufferHandle taa_curr_output() { return taa_buffer[(frame_id + 1) % 2]; }

  template <int Base>
  static float halton(int index) {
    float a = 0;
    float inv_base = 1.0f / float(Base);
    for (float mult = inv_base; index != 0; index /= Base, mult *= inv_base) {
      a += float(index % Base) * mult;
    }
    return a;
  }

  const Mat4& get_view_proj(bool jiterred) {
    if (jiterred) {
      float rndx = halton<2>(frame_id);
      float rndy = halton<3>(frame_id);
      const Mat4 subpixel_jitter {
        1, 0, 0, (rndx * 2 - 1) / double(width),  //
        0, 1, 0, (rndy * 2 - 1) / double(height), //
        0, 0, 1, 0,                               //
        0, 0, 0, 1                                //
      };
      return subpixel_jitter * view_proj;
    }
    return view_proj;
  }
};

struct SceneProxy {
  // holding for default material
  BufferHandle objects_cb;
  BufferHandle materials_cb;
  struct MaterialData {
    ShaderArgumentHandle arg;
    Vec4 parset0;
    Vec4 parset1;
    size_t cb_index;
    Vec4& albedo() { return parset0; }
    double& smoothness() { return parset1.x; }
    double& metalness() { return parset1.y; }
    double& emissive() { return parset1.z; }
  };
  struct ModelData {
    GeometryBuffers geo_buffers;
    // TODO support root cb offseting
    ShaderArgumentHandle raster_arg;
    ShaderArgumentHandle raytrace_shadertable_arg;
    size_t cb_index;
    size_t tlas_instance_id;
    Mat4 trans;

    // JUST COPY THAT SH*T
    MaterialData mat;
  };
  Hashmap<Scene::GeometryUID, GeometryBuffers> geometries;
  Hashmap<Scene::MaterialUID, MaterialData> materials;
  Hashmap<Scene::ModelUID, ModelData> models;

  // Accelration structure and shader table
  BufferHandle tlas;
  BufferHandle shader_table;

  // Direct lighting const buffer
  BufferHandle punctual_lights;
  vector<ShaderArgumentHandle> punctual_light_arg_cache;
  BufferHandle area_lights;
  vector<ShaderArgumentHandle> area_light_arg_cache;
};

} // namespace hybrid

using namespace hybrid;

struct HybridGPassShaderDesc : RasterizationShaderMetaInfo {
  HybridGPassShaderDesc() {
    ShaderParameter space0 {};
    space0.const_buffers = {ConstantBuffer()};
    ShaderParameter space1 {};
    space1.const_buffers = {ConstantBuffer()};
    ShaderParameter space2 {};
    space2.const_buffers = {ConstantBuffer()};

    RenderTargetDesc rt_normal {ResourceFormat::R32G32B32A32_FLOAT};
    RenderTargetDesc rt_albedo {ResourceFormat::B8G8R8A8_UNORM};
    RenderTargetDesc rt_emissive {ResourceFormat::R32G32B32A32_FLOAT};
    render_target_descs = {rt_normal, rt_albedo, rt_emissive};

    signature.param_table = {space0, space1};
  }
};

struct HybridRaytracingShaderDesc : RaytracingShaderMetaInfo {
  HybridRaytracingShaderDesc() {
    ShaderParameter space0 {};
    space0.const_buffers = {ConstantBuffer()}; // Per-render CN
    space0.shader_resources = {ShaderResource(), ShaderResource(), ShaderResource(),
      ShaderResource(), ShaderResource()};           // TLAS ans G-Buffer
    space0.unordered_accesses = {UnorderedAccess()}; // output buffer
    global_signature.param_table = {space0};

    ShaderParameter space1 {};
    space1.shader_resources = {ShaderResource(), ShaderResource()}; // indices and vertices
    space1.const_buffers = {ConstantBuffer()};                      // material
    hitgroup_signature.param_table = {{}, space1};

    hitgroup_name = L"hit_group0";
    raygen_name = L"raygen_shader";
    // raygen_name = L"raygen_plain_color";
    closest_hit_name = L"closest_hit_shader";
    miss_name = L"miss_shader";
  }
};

struct HybridBaseShadingShaderDesc : ComputeShaderMetaInfo {
  HybridBaseShadingShaderDesc() {
    ShaderParameter space1 {};
    space1.unordered_accesses = {
      UnorderedAccess(), // output color
    };
    signature.param_table = {{}, space1};
  }
};

struct HybridDirectLightingShaderDesc : ComputeShaderMetaInfo {
  HybridDirectLightingShaderDesc() {
    ShaderParameter space0 {};
    space0.const_buffers = {
      ConstantBuffer(), // light data
    };
    ShaderParameter space1 {};
    space1.shader_resources = {
      ShaderResource(), // depth
      ShaderResource(), // gbuffers
      ShaderResource(),
      ShaderResource(),
    };
    space1.unordered_accesses = {
      UnorderedAccess(), // output color
    };
    space1.const_buffers = {
      ConstantBuffer(), // per-render data
    };
    signature.param_table = {space0, space1};
  }
};

struct HybridDirectAreaLightingShaderDesc : HybridDirectLightingShaderDesc {
  HybridDirectAreaLightingShaderDesc() {}
};

struct BlitShaderDesc : RasterizationShaderMetaInfo {
  BlitShaderDesc() {
    ShaderParameter space0 {};
    space0.shader_resources = {ShaderResource()};
    space0.static_samplers = {StaticSampler()};
    signature.param_table = {space0};

    this->is_depth_stencil_disabled = true;

    RenderTargetDesc dest {ResourceFormat::B8G8R8A8_UNORM};
    this->render_target_descs = {dest};
  }
};

struct TaaShaderDesc : ComputeShaderMetaInfo {
  TaaShaderDesc() {
    ShaderParameter space0;
    space0.const_buffers = {ConstantBuffer()};
    space0.shader_resources = {ShaderResource(), ShaderResource()};     // input and history
    space0.unordered_accesses = {UnorderedAccess(), UnorderedAccess()}; // two outputs
    signature.param_table = {space0};
  }
};

HybridPipeline::HybridPipeline(RendererPtr renderer)
    : SimplexPipeline(renderer), m_enable_multibounce(false) {
  Renderer* r = get_renderer();

  {
    m_gpass_shader
      = r->create_shader(L"CoreData/shader/hybrid_gpass.hlsl", HybridGPassShaderDesc());
  }

  {
    m_base_shading_shader = r->create_shader(
      L"CoreData/shader/hybrid_base_shading.hlsl", HybridBaseShadingShaderDesc());
    m_punctual_lighting_shader = r->create_shader(
      L"CoreData/shader/hybrid_direct_lighting.hlsl", HybridDirectLightingShaderDesc());
    m_area_lighting_shader = r->create_shader(
      L"CoreData/shader/hybrid_direct_area_lighting.hlsl", HybridDirectAreaLightingShaderDesc());
  }

  {
    m_multibounce_shader
      = r->create_shader(L"CoreData/shader/hybrid_multibounce.hlsl", HybridRaytracingShaderDesc());
  }

  {
    m_blit_shader = r->create_shader(L"CoreData/shader/blit.hlsl", BlitShaderDesc());
    m_taa_shader = r->create_shader(L"CoreData/shader/taa.hlsl", TaaShaderDesc());
  }

  {
    ConstBufferLayout lo = {
      ShaderDataType::Float4,   // screen size
      ShaderDataType::Float4x4, // world_to_proj := camera view_proj
      ShaderDataType::Float4x4, // proj_to_world := camera view_proj inverse
      ShaderDataType::Float4,   // camera pos
      ShaderDataType::Float4,   // frame id
    };
    m_per_render_buffer = r->create_const_buffer(lo, 1, L"PerRenderCB");
  }
}

HybridPipeline::ViewportHandle HybridPipeline::register_viewport(ViewportConfig conf) {
  Renderer* r = get_renderer();

  ViewportProxy proxy {};

  proxy.width = conf.width;
  proxy.height = conf.height;
  proxy.swapchain = r->create_swapchain(conf.window_id, conf.width, conf.height, 2);

  proxy.depth_stencil_buffer
    = r->create_texture_2d(TextureDesc::depth_stencil(conf.width, conf.height),
      ResourceState::DeptpWrite, L"Depth Stencil");
  proxy.gbuffer0 = r->create_texture_2d(
    TextureDesc::render_target(conf.width, conf.height, ResourceFormat::R32G32B32A32_FLOAT),
    ResourceState::RenderTarget, L"Normal Buffer");
  proxy.gbuffer1 = r->create_texture_2d(
    TextureDesc::render_target(conf.width, conf.height, ResourceFormat::B8G8R8A8_UNORM),
    ResourceState::RenderTarget, L"Albedo Buffer");
  proxy.gbuffer2 = r->create_texture_2d(
    TextureDesc::render_target(conf.width, conf.height, ResourceFormat::R32G32B32A32_FLOAT),
    ResourceState::RenderTarget, L"Emissive Buffer");

  proxy.raytracing_output_buffer = r->create_unordered_access_buffer_2d(
    conf.width, conf.height, ResourceFormat::R32G32B32A32_FLOAT, L"Raytracing Output Buffer");

  proxy.deferred_shading_output = r->create_texture_2d(
    TextureDesc::unorder_access(conf.width, conf.height, ResourceFormat::R32G32B32A32_FLOAT),
    ResourceState::UnorderedAccess, L"Deferred Shading Output");

  // area light resources
  proxy.area_light.unshadowed = r->create_texture_2d(
    TextureDesc::unorder_access(conf.width, conf.height, ResourceFormat::R32G32B32A32_FLOAT),
    ResourceState::UnorderedAccess, L"Area Light Unshadowed");

  // helper routine
  auto make_blit_arg = [&](BufferHandle source) {
    ShaderArgumentValue val {};
    val.shader_resources = {source};
    return r->create_shader_argument(val);
  };

  // Base shading pass argument
  {
    ShaderArgumentValue val {};
    val.unordered_accesses = {proxy.deferred_shading_output};
    proxy.base_shading_inout_arg = r->create_shader_argument(val);
    REI_ASSERT(proxy.base_shading_inout_arg);
  }

  // punctual light argumnets
  {
    ShaderArgumentValue val {};
    val.shader_resources
      = {proxy.depth_stencil_buffer, proxy.gbuffer0, proxy.gbuffer1, proxy.gbuffer2};
    val.unordered_accesses = {proxy.deferred_shading_output};
    val.const_buffers = {m_per_render_buffer};
    val.const_buffer_offsets
      = {0}; // FIXME current all viewport using the same part of per-render buffer
    proxy.direct_lighting_inout_arg = r->create_shader_argument(val);
    REI_ASSERT(proxy.direct_lighting_inout_arg);
  }

  // area light arguments
  {
    ShaderArgumentValue val {};
    val.shader_resources
      = {proxy.depth_stencil_buffer, proxy.gbuffer0, proxy.gbuffer1, proxy.gbuffer2};
    val.unordered_accesses = {proxy.area_light.unshadowed};
    val.const_buffers = {m_per_render_buffer};
    val.const_buffer_offsets
      = {0}; // FIXME current all viewport using the same part of per-render buffer
    proxy.area_light.unshadowed_pass_arg = r->create_shader_argument(val);
    REI_ASSERT(proxy.area_light.unshadowed_pass_arg);
    proxy.area_light.blit_unshadowed = make_blit_arg(proxy.area_light.unshadowed);
  }

  {
    ConstBufferLayout lo {};
    lo[0] = ShaderDataType::Float4;
    proxy.taa_cb = r->create_const_buffer(lo, 1, L"TAA CB");
  }
  {
    static const wchar_t* taa_names[2] = {L"TAA_Buffer[0]", L"TAA_Buffer[1]"};
    auto make_taa_buffer = [&](int idx) {
      return r->create_texture_2d(
        TextureDesc::unorder_access(conf.width, conf.height, ResourceFormat::R32G32B32A32_FLOAT),
        ResourceState::UnorderedAccess, taa_names[idx]);
    };
    for (int i = 0; i < 2; i++)
      proxy.taa_buffer[i] = make_taa_buffer(i);
  }

  {
    auto make_taa_argument = [&](int input, int output) {
      ShaderArgumentValue v {};
      // TODO we feed deferred shading raw result to TAA, but better is to do a tonemapping first
      v.const_buffers = {proxy.taa_cb};
      v.const_buffer_offsets = {0};
      v.shader_resources = {proxy.taa_buffer[input], proxy.deferred_shading_output};
      v.unordered_accesses = {proxy.taa_buffer[output], proxy.deferred_shading_output};
      return r->create_shader_argument(v);
    };
    for (int i = 0; i < 2; i++)
      proxy.taa_argument[i] = make_taa_argument(i, (i + 1) % 2);
  }

  proxy.blit_for_present = make_blit_arg(proxy.deferred_shading_output);

  return add_viewport(std::move(proxy));
}

void HybridPipeline::transform_viewport(ViewportHandle handle, const Camera& camera) {
  ViewportProxy* viewport = get_viewport(handle);
  REI_ASSERT(viewport);
  Renderer* r = get_renderer();
  REI_ASSERT(r);

  viewport->cam_pos = camera.position();
  Mat4 new_vp = r->is_depth_range_01() ? camera.view_proj_halfz() : camera.view_proj();
  Mat4 diff_mat = new_vp - viewport->view_proj;
  if (!m_enabled_accumulated_rtrt || diff_mat.norm2() > 0) {
    viewport->view_proj_dirty = true;
    viewport->view_proj = new_vp;
    viewport->view_proj_inv = new_vp.inv();
  }
}

HybridPipeline::SceneHandle HybridPipeline::register_scene(SceneConfig conf) {
  const Scene* scene = conf.scene;
  REI_ASSERT(scene);
  Renderer* r = get_renderer();
  REI_ASSERT(r);

  const size_t model_count = scene->get_models().size();
  const size_t material_count = scene->materials().size();

  SceneProxy proxy = {};
  // Initialize Geometries
  {
    for (const GeometryPtr& geo : scene->geometries()) {
      GeometryBuffers buffers = r->create_geometry({geo});
      Scene::GeometryUID id = conf.scene->get_id(geo);
      proxy.geometries.insert({id, buffers});
    }
  }
  // Initialize Materials
  {
    ConstBufferLayout mat_lo = {
      ShaderDataType::Float4, // albedo
      ShaderDataType::Float4, // metalness and smoothness
    };
    proxy.materials_cb = r->create_const_buffer(mat_lo, material_count, L"Scene Material CB");

    proxy.materials.reserve(material_count);
    size_t index = 0;
    ShaderArgumentValue v {};
    v.const_buffers = {proxy.materials_cb}; // currenlty, materials are just somt value config
    v.const_buffer_offsets = {0};
    for (const MaterialPtr& mat : scene->materials()) {
      v.const_buffer_offsets[0] = index;
      ShaderArgumentHandle mat_arg = r->create_shader_argument(v);
      REI_ASSERT(mat_arg);
      SceneProxy::MaterialData data {};
      data.arg = mat_arg;
      data.cb_index = index;
      data.albedo() = Vec4(mat->get<Color>(L"albedo").value_or(Colors::magenta));
      data.smoothness() = mat->get<double>(L"smoothness").value_or(0);
      data.metalness() = mat->get<double>(L"metalness").value_or(0);
      data.emissive() = mat->get<double>(L"emissive").value_or(0);
      proxy.materials.insert({conf.scene->get_id(mat), data});
      index++;
    }
  }

  // Initialize Geometry, Material, and assigning raster/ray-tracing shader arguments
  {
    ConstBufferLayout cb_lo = {
      ShaderDataType::Float4x4, // WVP
      ShaderDataType::Float4x4, // World
    };
    proxy.objects_cb = r->create_const_buffer(cb_lo, model_count, L"Scene-Objects CB");

    proxy.models.reserve(model_count);
    ShaderArgumentValue raster_arg_value = {};
    { // init
      raster_arg_value.const_buffers = {proxy.objects_cb};
      raster_arg_value.const_buffer_offsets = {SIZE_T_ERROR};
    }
    ShaderArgumentValue raytrace_arg_value = {};
    {
      raytrace_arg_value.const_buffers = {proxy.materials_cb};
      raytrace_arg_value.const_buffer_offsets = {SIZE_T_ERROR};
    }
    for (size_t model_index = 0; model_index < model_count; model_index++) {
      auto model = scene->get_models()[model_index];
      auto mat = proxy.materials.try_get(conf.scene->get_id(model->get_material()));
      REI_ASSERT(mat);
      auto geo = proxy.geometries.try_get(conf.scene->get_id(model->get_geometry()));
      REI_ASSERT(geo);
      const size_t material_index = mat->cb_index;
      // raster
      raster_arg_value.const_buffer_offsets[0] = model_index;
      ShaderArgumentHandle raster_arg = r->create_shader_argument(raster_arg_value);
      // raytrace
      raytrace_arg_value.shader_resources = {geo->index_buffer, geo->vertex_buffer};
      raytrace_arg_value.const_buffer_offsets[0] = material_index;
      ShaderArgumentHandle raytrace_arg = r->create_shader_argument(raytrace_arg_value);
      // record
      size_t tlas_instance_id = model_index;
      SceneProxy::ModelData data {*geo, raster_arg, raytrace_arg, model_index, tlas_instance_id,
        model->get_transform(), *mat};
      proxy.models.insert({conf.scene->get_id(model), std::move(data)});
    }
  }

  // Build Acceleration structure and shader table
  {
    RaytraceSceneDesc desc {};
    for (auto& kv : proxy.models) {
      auto& m = kv.second;
      desc.instance_id.push_back(m.tlas_instance_id);
      desc.blas_buffer.push_back(m.geo_buffers.blas_buffer);
      desc.transform.push_back(m.trans);
    }
    proxy.tlas = r->create_raytracing_accel_struct(desc);
    proxy.shader_table = r->create_shader_table(*scene, m_multibounce_shader);
  }

  // Allocate analitic light buffer
  {
    ConstBufferLayout lo {};
    lo[0] = ShaderDataType::Float4; // pos_dir
    lo[1] = ShaderDataType::Float4; // color
    proxy.punctual_lights = r->create_const_buffer(lo, 128, L"Punctual Lights Buffer");
  }
  {
    ConstBufferLayout lo {};
    lo[0] = ShaderDataType::Float4; // shaper
    lo[1] = ShaderDataType::Float4; // color
    proxy.area_lights = r->create_const_buffer(lo, 128, L"Area Lights Buffer");
  }

  return add_scene(std::move(proxy));
}

void HybridPipeline::update_model(
  SceneHandle scene_handle, const Model& model, Scene::ModelUID model_id) {
  SceneProxy* scene = get_scene(scene_handle);
  REI_ASSERT(scene);
  auto* data = scene->models.try_get(model_id);
  data->trans = model.get_transform();
}

void HybridPipeline::render(ViewportHandle viewport_h, SceneHandle scene_h) {
  ViewportProxy* viewport = get_viewport(viewport_h);
  SceneProxy* scene = get_scene(scene_h);
  REI_ASSERT(viewport);
  REI_ASSERT(scene);

  Renderer* const renderer = get_renderer();
  const auto cmd_list = renderer->prepare();

  // render info
  Mat4 view_proj = viewport->get_view_proj(viewport->view_proj_dirty && m_enable_jittering);
  double taa_blend_factor
    = viewport->view_proj_dirty ? 1.0 : (m_enabled_accumulated_rtrt ? 0.01 : 0.5);

  // Update scene-wide const buffer (per-frame CB)
  {
    Vec4 screen = Vec4(viewport->width, viewport->height, 0, 0);
    cmd_list->update_const_buffer(m_per_render_buffer, 0, 0, screen);
    cmd_list->update_const_buffer(m_per_render_buffer, 0, 1, viewport->view_proj);
    cmd_list->update_const_buffer(m_per_render_buffer, 0, 2, viewport->view_proj_inv);
    cmd_list->update_const_buffer(m_per_render_buffer, 0, 3, viewport->cam_pos);
    Vec4 render_info = Vec4(viewport->frame_id, -1, -1, -1);
    cmd_list->update_const_buffer(m_per_render_buffer, 0, 4, render_info);
  }

  // Update material buffer
  {
    for (auto& pair : scene->materials) {
      auto& mat = pair.second;
      renderer->update_const_buffer(scene->materials_cb, mat.cb_index, 0, mat.parset0);
      renderer->update_const_buffer(scene->materials_cb, mat.cb_index, 1, mat.parset1);
    }
  }

  //-------
  // Pass: Create G-Buffer

  // Update per-object const buffer
  {
    for (auto& pair : scene->models) {
      auto& model = pair.second;
      size_t index = model.cb_index;
      Mat4& m = model.trans;
      Mat4 mvp = view_proj * m;
      renderer->update_const_buffer(scene->objects_cb, index, 0, mvp);
      renderer->update_const_buffer(scene->objects_cb, index, 1, m);
    }
  }

  // Draw to G-Buffer
  cmd_list->transition(viewport->gbuffer0, ResourceState::RenderTarget);
  cmd_list->transition(viewport->gbuffer1, ResourceState::RenderTarget);
  cmd_list->transition(viewport->gbuffer2, ResourceState::RenderTarget);
  cmd_list->transition(viewport->depth_stencil_buffer, ResourceState::DeptpWrite);
  RenderPassCommand gpass = {};
  {
    gpass.render_targets = {viewport->gbuffer0, viewport->gbuffer1, viewport->gbuffer2};
    gpass.depth_stencil = viewport->depth_stencil_buffer;
    gpass.clear_ds = true;
    gpass.clear_rt = true;
    gpass.viewport = RenderViewaport::full(viewport->width, viewport->height);
    gpass.area = RenderArea::full(viewport->width, viewport->height);
  }
  cmd_list->begin_render_pass(gpass);
  {
    DrawCommand draw_cmd = {};
    draw_cmd.shader = m_gpass_shader;
    for (auto pair : scene->models) {
      auto& model = pair.second;
      draw_cmd.index_buffer = model.geo_buffers.index_buffer;
      draw_cmd.vertex_buffer = model.geo_buffers.vertex_buffer;
      draw_cmd.arguments = {model.raster_arg, model.mat.arg};
      cmd_list->draw(draw_cmd);
    }
  }
  cmd_list->end_render_pass();

  //--- End G-Buffer pass

  if (m_enable_multibounce) {
    //-------
    // Pass: Raytraced multi-bounced GI

    // TODO move this to a seperated command queue

    // Update shader Tables
    {
      UpdateShaderTable desc = UpdateShaderTable::hitgroup();
      desc.shader = m_multibounce_shader;
      desc.shader_table = scene->shader_table;
      for (auto& pair : scene->models) {
        auto& m = pair.second;
        desc.index = m.tlas_instance_id;
        desc.arguments = {m.raytrace_shadertable_arg};
        cmd_list->update_shader_table(desc);
      }
    }

    // Trace
    cmd_list->transition(viewport->gbuffer0, ResourceState::ComputeShaderResource);
    cmd_list->transition(viewport->gbuffer1, ResourceState::ComputeShaderResource);
    cmd_list->transition(viewport->gbuffer2, ResourceState::ComputeShaderResource);
    cmd_list->transition(viewport->depth_stencil_buffer, ResourceState::ComputeShaderResource);
    cmd_list->transition(viewport->raytracing_output_buffer, ResourceState::UnorderedAccess);
    {
      RaytraceCommand cmd {};
      cmd.raytrace_shader = m_multibounce_shader;
      cmd.arguments = {fetch_raytracing_arg(viewport_h, scene_h)};
      cmd.shader_table = scene->shader_table;
      cmd.width = viewport->width;
      cmd.height = viewport->height;
      cmd_list->raytrace(cmd);
    }

    //--- End multi-bounce GI pass
  }

  //-------
  // Pass: Deferred direct lightings, both punctual and area lights

  // Base Shading
  cmd_list->transition(viewport->deferred_shading_output, ResourceState::UnorderedAccess);
  {
    DispatchCommand dispatch {};
    dispatch.compute_shader = m_base_shading_shader;
    dispatch.arguments = {viewport->base_shading_inout_arg};
    dispatch.dispatch_x = viewport->width / 8;
    dispatch.dispatch_y = viewport->height / 8;
    dispatch.dispatch_z = 1;
    cmd_list->dispatch(dispatch);
  }

  cmd_list->barrier(viewport->deferred_shading_output);

  // Punctual lights
  // TODO: same texture input for multibounce GI
  cmd_list->transition(viewport->gbuffer0, ResourceState::ComputeShaderResource);
  cmd_list->transition(viewport->gbuffer1, ResourceState::ComputeShaderResource);
  cmd_list->transition(viewport->gbuffer2, ResourceState::ComputeShaderResource);
  cmd_list->transition(viewport->depth_stencil_buffer, ResourceState::ComputeShaderResource);
  cmd_list->transition(viewport->deferred_shading_output, ResourceState::UnorderedAccess);
  // currently both taa and direct-lighting write to the same UA buffer
  cmd_list->barrier(viewport->deferred_shading_output);
  int light_count = 2;
  static int light_indices[] = {0, 1};
  static Color light_colors[] = {Colors::white * 1.3, Colors::white * 1};
  static Vec4 light_pos_dirs[] = {{Vec3(1, 2, 1).normalized(), 0}, {0, 2, 0, 1}};
  for (int i = 0; i < light_count; i++) {
    int light_index = light_indices[i];
    Vec4 light_color = Vec4(light_colors[i]);
    Vec4 light_pos_dir = light_pos_dirs[i];

    // update light data
    cmd_list->update_const_buffer(scene->punctual_lights, light_index, 0, light_pos_dir);
    cmd_list->update_const_buffer(scene->punctual_lights, light_index, 1, light_color);

    // dispatch
    DispatchCommand dispatch {};
    dispatch.compute_shader = m_punctual_lighting_shader;
    dispatch.arguments = {
      fetch_direct_punctual_lighting_arg(*scene, light_index), viewport->direct_lighting_inout_arg};
    dispatch.dispatch_x = viewport->width / 8;
    dispatch.dispatch_y = viewport->height / 8;
    dispatch.dispatch_z = 1;
    cmd_list->dispatch(dispatch);
  }

  // Area lights
  // TODO: same texture input for multibounce GI
  cmd_list->transition(viewport->gbuffer0, ResourceState::ComputeShaderResource);
  cmd_list->transition(viewport->gbuffer1, ResourceState::ComputeShaderResource);
  cmd_list->transition(viewport->gbuffer2, ResourceState::ComputeShaderResource);
  cmd_list->transition(viewport->depth_stencil_buffer, ResourceState::ComputeShaderResource);
  cmd_list->transition(viewport->area_light.unshadowed, ResourceState::UnorderedAccess);
  cmd_list->clear_texture(viewport->area_light.unshadowed, {0, 0, 0, 0},
    RenderArea::full(viewport->width, viewport->height));
  cmd_list->barrier(viewport->area_light.unshadowed);
  static std::array<int, 1> area_light_indices = {0};
  static std::array<Vec4, 1> area_light_shapes = {
    Vec4(0, 5, 0, 0.5),
  };
  static std::array<Color, 1> area_light_color {
    Colors::white * 10,
  };
  for (int i = 0; i < area_light_indices.size(); i++) {
    int light_index = area_light_indices[i];
    Vec4 light_color = Vec4(area_light_color[i]);
    Vec4 light_shape = area_light_shapes[i];

    // update light data
    cmd_list->update_const_buffer(scene->area_lights, light_index, 0, light_shape);
    cmd_list->update_const_buffer(scene->area_lights, light_index, 1, light_color);

    // dispatch
    DispatchCommand dispatch {};
    dispatch.compute_shader = m_area_lighting_shader;
    dispatch.arguments = {fetch_direct_area_lighting_arg(*scene, light_index),
      viewport->area_light.unshadowed_pass_arg};
    dispatch.dispatch_x = viewport->width / 8;
    dispatch.dispatch_y = viewport->height / 8;
    dispatch.dispatch_z = 1;
    cmd_list->dispatch(dispatch);
  }

  // --- End Deferred directy lighting

  // -------
  // Pass: Stochastic Shadow

  //--- End Stochastic Shadow

  // -------
  // Pass: TAA on final shading result
  {
    Vec4 parset0 {double(viewport->frame_id), taa_blend_factor, -1, -1};
    cmd_list->update_const_buffer(viewport->taa_cb, 0, 0, parset0);
  }
  cmd_list->transition(viewport->taa_curr_input(), ResourceState::ComputeShaderResource);
  cmd_list->transition(viewport->taa_curr_output(), ResourceState::UnorderedAccess);
  cmd_list->transition(viewport->deferred_shading_output, ResourceState::UnorderedAccess);
  {
    DispatchCommand dispatch {};
    dispatch.compute_shader = m_taa_shader;
    dispatch.arguments = {viewport->taa_curr_arg()};
    dispatch.dispatch_x = viewport->width / 8;
    dispatch.dispatch_y = viewport->height / 8;
    dispatch.dispatch_z = 1;
    cmd_list->dispatch(dispatch);
  }

  // --- End TAA pass

  // -------
  // Pass: Blit Post-processing reults to render target

  BufferHandle render_target = renderer->fetch_swapchain_render_target_buffer(viewport->swapchain);
  cmd_list->transition(render_target, ResourceState::RenderTarget);
  cmd_list->transition(viewport->deferred_shading_output, ResourceState::PixelShaderResource);
  {
    RenderPassCommand render_pass {};
    render_pass.render_targets = {render_target};
    render_pass.depth_stencil = c_empty_handle;
    render_pass.clear_ds = false;
    render_pass.clear_rt = true;
    render_pass.viewport = RenderViewaport::full(viewport->width, viewport->height);
    render_pass.area = RenderArea::full(viewport->width, viewport->height);
    cmd_list->begin_render_pass(render_pass);

    DrawCommand draw {};
    draw.arguments = {viewport->blit_for_present};
    draw.shader = m_blit_shader;
    cmd_list->draw(draw);
  
    cmd_list->end_render_pass();
  }

  // --- End blitting

  // some debug blits
  if (true) {
    const size_t blit_width = 256;
    const size_t blit_height = blit_width * viewport->height / viewport->width;
    int debug_blit_count = 0;
    const RenderViewaport full_vp = RenderViewaport::full(viewport->width, viewport->height);
    const RenderArea full_area = RenderArea::full(viewport->width, viewport->height);
    auto drawer = [&](BufferHandle texture, ShaderArgumentHandle blit_arg) {
      cmd_list->transition(texture, ResourceState::PixelShaderResource);
      RenderPassCommand render_pass {};
      render_pass.render_targets = {render_target};
      render_pass.depth_stencil = c_empty_handle;
      render_pass.clear_ds = false;
      render_pass.clear_rt = false;
      render_pass.viewport = full_vp.shrink_to_upper_left(blit_width, blit_height, 0, blit_height * debug_blit_count);
      render_pass.area = full_area.shrink_to_upper_left(blit_width, blit_height, 0, blit_height * debug_blit_count);
      cmd_list->begin_render_pass(render_pass);

      DrawCommand draw {};
      draw.arguments = {blit_arg};
      draw.shader = m_blit_shader;
      cmd_list->draw(draw);

      cmd_list->end_render_pass();

      debug_blit_count++;
    };

    drawer(viewport->area_light.unshadowed, viewport->area_light.blit_unshadowed);
  }

  // Update frame-counting in the end
  viewport->advance_frame();

  cmd_list->transition(render_target, ResourceState::Present);
  cmd_list->present(viewport->swapchain, false);
}

ShaderArgumentHandle HybridPipeline::fetch_raytracing_arg(
  ViewportHandle viewport_h, SceneHandle scene_h) {
  const CombinedArgumentKey cache_key {viewport_h, scene_h};

  // check cache
  ShaderArgumentHandle* cached_arg = m_raytracing_args.try_get(cache_key);
  if (cached_arg) { return *cached_arg; }

  // create new one
  Renderer* r = get_renderer();
  REI_ASSERT(r);
  ViewportProxy* viewport = get_viewport(viewport_h);
  SceneProxy* scene = get_scene(scene_h);
  REI_ASSERT(viewport && scene);

  ShaderArgumentValue val {};
  val.const_buffers = {m_per_render_buffer};
  val.const_buffer_offsets = {0};
  val.shader_resources = {
    scene->tlas,                    // t0 TLAS
    viewport->depth_stencil_buffer, // t1 G-buffer depth
    viewport->gbuffer0,             // t2 G-Buffer::normal
    viewport->gbuffer1,             // t3 G-Buffer::albedo
    viewport->gbuffer2,             // t4 G-Buffer::emissive
  };
  val.unordered_accesses = {viewport->raytracing_output_buffer};
  ShaderArgumentHandle arg = r->create_shader_argument(val);
  REI_ASSERT(arg);

  // add to cache
  m_raytracing_args.insert({cache_key, arg});

  return arg;
}

ShaderArgumentHandle HybridPipeline::fetch_direct_punctual_lighting_arg(
  SceneProxy& scene, int cb_index) {
  REI_ASSERT(cb_index >= 0);
  if (cb_index >= scene.punctual_light_arg_cache.size()) {
    Renderer* r = get_renderer();
    REI_ASSERT(r);

    ShaderArgumentValue val {};
    val.const_buffers = {scene.punctual_lights};
    val.const_buffer_offsets = {size_t(cb_index)};
    ShaderArgumentHandle h = r->create_shader_argument(val);
    REI_ASSERT(h);
    scene.punctual_light_arg_cache.reserve(cb_index + 1);
    scene.punctual_light_arg_cache.resize(cb_index + 1);
    scene.punctual_light_arg_cache[cb_index] = h;
  }

  return scene.punctual_light_arg_cache[cb_index];
}

ShaderArgumentHandle HybridPipeline::fetch_direct_area_lighting_arg(
  SceneProxy& scene, int cb_index) {
  REI_ASSERT(cb_index >= 0);
  if (cb_index >= scene.area_light_arg_cache.size()) {
    Renderer* r = get_renderer();
    REI_ASSERT(r);

    ShaderArgumentValue val {};
    val.const_buffers = {scene.area_lights};
    val.const_buffer_offsets = {size_t(cb_index)};
    ShaderArgumentHandle h = r->create_shader_argument(val);
    REI_ASSERT(h);
    scene.area_light_arg_cache.reserve(cb_index + 1);
    scene.area_light_arg_cache.resize(cb_index + 1);
    scene.area_light_arg_cache[cb_index] = h;
  }

  return scene.area_light_arg_cache[cb_index];
}

} // namespace rei