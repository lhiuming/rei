#ifndef REI_D3D_COMMON_RESOURCES_H
#define REI_D3D_COMMON_RESOURCES_H

#if DIRECT3D_ENABLED

#include <array>
#include <memory>

#include <DirectXMath.h>
#include <d3d12.h>
#include <d3dx12.h>
#include <windows.h>
#include <wrl.h>

#include "../algebra.h"
#include "../color.h"
#include "../common.h"
#include "../renderer.h"
#include "d3d_utils.h"

namespace rei {

namespace d3d {

using Microsoft::WRL::ComPtr;

// Some contants
constexpr DXGI_FORMAT c_index_format = DXGI_FORMAT_R16_UINT;
constexpr DXGI_FORMAT c_accel_struct_vertex_pos_format = DXGI_FORMAT_R32G32B32_FLOAT;

// Reminder: using right-hand coordinate throughout the pipeline
constexpr bool is_right_handed = true;

inline static DirectX::XMFLOAT4 rei_to_D3D(const Vec4& v) {
  return DirectX::XMFLOAT4(v.x, v.y, v.z, v.h);
}

inline static DirectX::XMMATRIX rei_to_D3D(const Mat4& A) {
  // REMARK: Mat4 is column major, while D3D is row major by default
  return DirectX::XMMatrixSet( // must transpose
    A(0, 0), A(1, 0), A(2, 0), A(3, 0), A(0, 1), A(1, 1), A(2, 1), A(3, 1), A(0, 2), A(1, 2),
    A(2, 2), A(3, 2), A(0, 3), A(1, 3), A(2, 3), A(3, 3));
}

// TODO remove this hardcode class
struct cbPerObject {
  DirectX::XMMATRIX WVP; // DirectXMath fits well with HLSL
  DirectX::XMMATRIX World;

  void update(const Mat4& wvp, const Mat4& world = Mat4::I()) {
    WVP = rei_to_D3D(wvp);
    World = rei_to_D3D(world);
  }
};

struct VertexElement {
  DirectX::XMFLOAT4 pos; // DirectXMath fits well with HLSL
  DirectX::XMFLOAT4 color;
  DirectX::XMFLOAT3 normal;

  VertexElement() {}
  VertexElement(const Vec4& p, const Color& c, const Vec3& n)
      : pos(p.x, p.y, p.z, p.h), color(c.r, c.g, c.b, c.a), normal(n.x, n.y, n.z) {}
  VertexElement(
    float x, float y, float z, float r, float g, float b, float a, float nx, float ny, float nz)
      : pos(x, y, z, 1), color(r, g, b, a), normal(nx, ny, nz) {}
};

// Over simple Light object, to debug
struct Light {
  DirectX::XMFLOAT3 dir;
  float pad; // padding to match with shader's constant buffer packing
  DirectX::XMFLOAT4 ambient;
  DirectX::XMFLOAT4 diffuse;

  Light() { ZeroMemory(this, sizeof(Light)); }
};

// per-frame constant-buffer layout
struct cbPerFrame {
  Light light;
  DirectX::XMMATRIX camera_world_trans;
  DirectX::XMFLOAT3 camera_pos;

  void set_camera_world_trans(const Mat4& m) { camera_world_trans = rei_to_D3D(m); }
  void set_camera_pos(const Vec3& v) { camera_pos = {float(v.x), float(v.y), float(v.z)}; }
};

struct RenderTargetSpec {
  DXGI_SAMPLE_DESC sample_desc; // multi-sampling parameters
  DXGI_FORMAT rt_format;
  DXGI_FORMAT ds_format;
  D3D12_DEPTH_STENCIL_VALUE ds_clear;
  RenderTargetSpec();

  bool operator==(const RenderTargetSpec& other) const {
    return sample_desc.Count == other.sample_desc.Count
           && sample_desc.Quality == other.sample_desc.Quality && rt_format == other.rt_format
           && ds_format == other.ds_format && ds_clear.Depth == other.ds_clear.Depth
           && ds_clear.Stencil == other.ds_clear.Stencil;
  }

  std::size_t simple_hash() const {
    // 5 bits
    std::size_t hash_sample = sample_desc.Count ^ (sample_desc.Quality << 4);
    // 16 bits
    std::size_t hash_rt_ds_format = rt_format ^ (ds_format << 8);
    // ignored
    std::size_t hash_ds_clear = 0;
    return hash_sample ^ (hash_rt_ds_format << 5);
  }
};

struct GeometryData : BaseGeometryData {
  using BaseGeometryData::BaseGeometryData;
};

struct MeshData : GeometryData {
  using GeometryData::GeometryData;
  ComPtr<ID3D12Resource> vert_buffer;
  ComPtr<ID3D12Resource> vert_upload_buffer;
  D3D12_VERTEX_BUFFER_VIEW vbv = {NULL, UINT_MAX, UINT_MAX};
  UINT vertex_num = UINT_MAX;

  CD3DX12_GPU_DESCRIPTOR_HANDLE vert_srv_gpu = {D3D12_DEFAULT};
  CD3DX12_CPU_DESCRIPTOR_HANDLE vert_srv_cpu = {D3D12_DEFAULT};
  DXGI_FORMAT vertex_pos_format;

  ComPtr<ID3D12Resource> ind_buffer;
  ComPtr<ID3D12Resource> ind_upload_buffer;
  D3D12_INDEX_BUFFER_VIEW ibv = {NULL, UINT_MAX, DXGI_FORMAT_UNKNOWN};
  UINT index_num = UINT_MAX;

  CD3DX12_GPU_DESCRIPTOR_HANDLE ind_srv_gpu = {D3D12_DEFAULT};
  CD3DX12_CPU_DESCRIPTOR_HANDLE ind_srv_cpu = {D3D12_DEFAULT};
  DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN;

  ComPtr<ID3D12Resource> blas_buffer;
  ComPtr<ID3D12Resource> scratch_buffer;
};

class ViewportResources;

struct ViewportData : BaseViewportData {
  using BaseViewportData::BaseViewportData;
  Vec3 pos = {};
  Mat4 view = Mat4::I();
  Mat4 view_proj = Mat4::I();
  std::array<FLOAT, 4> clear_color;
  D3D12_VIEWPORT d3d_viewport;
  D3D12_RECT scissor;

  std::weak_ptr<ViewportResources> viewport_resources;

  D3D12_CPU_DESCRIPTOR_HANDLE depth_buffer_srv_cpu;
  D3D12_GPU_DESCRIPTOR_HANDLE depth_buffer_srv_gpu;

  void update_camera_transform(const Camera& cam) {
    REI_ASSERT(is_right_handed);
    pos = cam.position();
    view = cam.view();
    view_proj = cam.view_proj_halfz();
  }
};

struct ShaderMetaInfo {
  CD3DX12_RASTERIZER_DESC raster_state = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
  CD3DX12_DEPTH_STENCIL_DESC depth_stencil = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
  bool is_depth_stencil_null = false;
  CD3DX12_BLEND_DESC blend_state = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
  CD3DX12_ROOT_SIGNATURE_DESC root_desc = CD3DX12_ROOT_SIGNATURE_DESC(D3D12_DEFAULT);
  D3D12_INPUT_LAYOUT_DESC input_layout = {nullptr, 0};

  ShaderMetaInfo() {
    REI_ASSERT(is_right_handed);
    raster_state.FrontCounterClockwise = true;
    depth_stencil.DepthFunc
      = D3D12_COMPARISON_FUNC_GREATER; // we use right-hand coordiante throughout the pipeline
  }
};

struct ShaderCompileResult {
  ComPtr<ID3DBlob> vs_bytecode;
  ComPtr<ID3DBlob> ps_bytecode;
};

struct ShaderConstBuffers {
  std::unique_ptr<UploadBuffer<cbPerFrame>> per_frame_CB;
  std::shared_ptr<UploadBuffer<cbPerObject>> per_object_CBs;
  // TODO more delicated management
  UINT next_object_index = UINT_MAX;
};

struct ShaderData : BaseShaderData {
  using BaseShaderData::BaseShaderData;
  std::unique_ptr<ShaderMetaInfo> meta;
  ShaderCompileResult compiled_data;
  ShaderConstBuffers const_buffers;
  ComPtr<ID3D12RootSignature> root_signature;
};

struct MaterialData : BaseMaterialData {
  using BaseMaterialData::BaseMaterialData;
  std::shared_ptr<ShaderData> shader;
};

struct ModelData : BaseModelData {
  using BaseModelData::BaseModelData;
  // Bound aabb;
  std::shared_ptr<GeometryData> geometry;
  std::shared_ptr<MaterialData> material;
  UINT const_buffer_index = UINT_MAX; // in shader cb buffer
  UINT cbv_index = UINT_MAX;          // in device shared heap

  UINT tlas_instance_id = UINT_MAX;

  void update_transform(Model& model) {
    // NOTE: check how ViewportData store the transforms
    REI_ASSERT(is_right_handed);
    this->transform = model.get_transform();
  }
};

// Data proxy for all obejct in a scene
struct SceneData : BaseSceneData {
  using BaseSceneData::BaseSceneData;
};

struct CullingData : BaseCullingData {
  using BaseCullingData::BaseCullingData;
  std::vector<ModelData> models;
  std::shared_ptr<SceneData> scene;
};

} // namespace d3d

} // namespace rei

#endif

#endif
