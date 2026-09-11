#ifndef RENDERING_SYSTEM_HPP
#define RENDERING_SYSTEM_HPP

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include "Dx12Common.hpp"
#include "UploadBuffer.hpp"
#include "RenderStructs.hpp"
#include "GBuffer.hpp"
#include "ObjectField.hpp"
#include "ShadowMap.hpp"

// Диапазон вершин с одним материалом
struct SubMesh {
	UINT vertexOffset = 0;
	UINT vertexCount  = 0;
	UINT materialSlot = 0;      // индекс в m_materials / в SRV-куче материалов
	bool tessellated  = false;  // рисовать ли через HS/DS с displacement
};

// Материал: константы + до четырёх текстур.
// SRV-куча хранит их четвёрками, порядок совпадает с регистрами t0..t3.
struct Material {
	MaterialConstants        constants;
	ComPtr<ID3D12Resource>   diffuseTex;   // t0, nullptr → белая заглушка
	ComPtr<ID3D12Resource>   alphaTex;     // t1, map_d
	ComPtr<ID3D12Resource>   normalTex;    // t2, *_nrm.dds (сгенерирована из карты высот)
	ComPtr<ID3D12Resource>   dispTex;      // t3, map_bump (карта высот)
	std::string              name;
};

// ─────────────────────────────────────────────────────────────────────────────
// RenderingSystem — владеет всем, что относится к отрисовке сцены:
// сценой (VB + подмеши + материалы + текстуры), G-Buffer'ом, обоими
// root signature/PSO и списком источников света.
//
// Framework оставляет за собой окно, устройство, очередь команд, swap chain,
// depth-буфер и камеру, а всю работу по кадру делегирует сюда.
//
// Схема кадра (deferred rendering, лекция 03 слайд 8):
//   1. Opaque geometry stage → заполняем G-Buffer, освещение не считаем
//   2. Light stage           → для каждого источника рисуем fullscreen-треугольник
//                              с аддитивным блендингом, читая G-Buffer через Load()
//   (3. Transparent stage    → в этой работе не реализован)
// ─────────────────────────────────────────────────────────────────────────────
class RenderingSystem {
public:
	RenderingSystem() = default;
	~RenderingSystem() = default;

	RenderingSystem(const RenderingSystem&) = delete;
	RenderingSystem& operator=(const RenderingSystem&) = delete;

	// Создаёт шейдеры, root signatures, PSO, загружает сцену и текстуры.
	// cmdAlloc/cmdList используются для одноразовой заливки текстур на GPU,
	// после чего RenderingSystem их не держит.
	void Init(ID3D12Device* device,
	          ID3D12CommandQueue* cmdQueue,
	          ID3D12CommandAllocator* cmdAlloc,
	          ID3D12GraphicsCommandList* cmdList,
	          DXGI_FORMAT backBufferFormat,
	          DXGI_FORMAT depthStencilFormat);

	// Вызывается из Framework::OnResize после пересоздания depth-буфера.
	void OnResize(ID3D12Device* device, UINT width, UINT height, ID3D12Resource* depthBuffer);

	// Обновляет константные буферы. Матрицы приходят готовыми из Framework.
	// fovY/aspect/nearZ/farZ нужны для построения каскадов: они режут
	// фрустум камеры, а не берут готовую матрицу проекции.
	void Update(double dt,
	            const DirectX::XMMATRIX& view,
	            const DirectX::XMMATRIX& proj,
	            const DirectX::XMFLOAT3& eyePos,
	            UINT width, UINT height,
	            float fovY, float aspect, float nearZ, float farZ);

	// Пишет команды обоих проходов в cmdList.
	void Render(ID3D12GraphicsCommandList* cmd,
	            D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
	            D3D12_CPU_DESCRIPTOR_HANDLE dsv,
	            const D3D12_VIEWPORT& viewport,
	            const D3D12_RECT& scissor);

	// ── Управление с клавиатуры (вызывает Framework) ────────────────────────
	void ToggleUvAnimation();
	void ScaleTiling(float factor);
	void ResetUv();
	void SetDebugMode(uint32_t mode);
	uint32_t DebugMode() const { return m_debugMode; }
	void ScaleLightIntensity(float factor);

	// ── ДЗ №3 ───────────────────────────────────────────────────────────────
	void ToggleTessellation();
	void ToggleWireframe();
	void ToggleNormalMapping();
	void ToggleGreenChannelFlip();
	void ToggleHullBackfaceCulling();
	void ScaleDisplacement(float factor);
	void ScaleMaxTessFactor(float delta);

	// ── ДЗ №4 ───────────────────────────────────────────────────────────────
	void     SetCullMode(CullMode mode);
	CullMode GetCullMode() const { return m_cullMode; }
	void     ToggleOctreeBoxes() { m_objectField.ToggleNodeBoxes(); }

	// «Заморозка» пирамиды видимости — единственный способ увидеть отсечение
	// глазами: пирамида остаётся на месте, камера улетает в сторону.
	void ToggleFrustumFreeze();
	bool FrustumFrozen() const { return m_freezeFrustum; }

	// ── ДЗ №5 ───────────────────────────────────────────────────────────────
	void ToggleShadows();
	void ToggleCascadeView();
	void ScaleShadowBias(float factor);
	void ScaleCascadeLambda(float delta);
	bool ShadowsEnabled() const { return m_shadowsEnabled; }

	const CullStats& FieldStats()   const { return m_objectField.Stats(); }
	size_t OctreeNodeCount()        const { return m_objectField.OctreeNodeCount(); }
	int    OctreeDepth()            const { return m_objectField.OctreeDepth(); }

	bool SceneLoaded() const { return !m_subMeshes.empty(); }
	size_t LightCount() const { return m_lights.size(); }

private:
	// ── Построение ──────────────────────────────────────────────────────────
	void BuildShaders();
	void BuildGeometryRootSignature(ID3D12Device* device);
	void BuildLightRootSignature(ID3D12Device* device);
	void BuildGeometryPSO(ID3D12Device* device);
	void BuildTessellationPSO(ID3D12Device* device);
	void BuildLightPSO(ID3D12Device* device);
	void BuildShadowRootSignature(ID3D12Device* device);
	void BuildShadowPSO(ID3D12Device* device);
	void RenderShadowPass(ID3D12GraphicsCommandList* cmd);
	void BuildConstantBuffers(ID3D12Device* device);

	void CreateWhiteTexture(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
	                        std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive);

	void LoadScene(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
	               std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive);

	void BuildMaterialSrvHeap(ID3D12Device* device);
	void BuildLights();

	// Загрузка одной DDS-текстуры с кэшем по имени файла
	ComPtr<ID3D12Resource> LoadTextureCached(ID3D12Device* device,
	                                         ID3D12GraphicsCommandList* cmd,
	                                         const std::wstring& path,
	                                         std::vector<ComPtr<ID3D12Resource>>& uploadKeepAlive);

	void FlushUploads(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cmd);

private:
	DXGI_FORMAT m_backBufferFormat   = DXGI_FORMAT_R8G8B8A8_UNORM;
	DXGI_FORMAT m_depthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

	// ── Шейдеры ─────────────────────────────────────────────────────────────
	ComPtr<ID3DBlob> m_geoVS, m_geoPS;
	ComPtr<ID3DBlob> m_geoVSTess, m_geoHS, m_geoDS;
	ComPtr<ID3DBlob> m_lightVS, m_lightPS, m_debugPS;
	ComPtr<ID3DBlob> m_shadowVS, m_shadowVSInstanced;

	// ── Root signatures / PSO ───────────────────────────────────────────────
	ComPtr<ID3D12RootSignature> m_geoRootSig;
	ComPtr<ID3D12RootSignature> m_lightRootSig;

	ComPtr<ID3D12PipelineState> m_geoPSO;        // без тесселяции, solid
	ComPtr<ID3D12PipelineState> m_geoPSOWire;    // без тесселяции, wireframe
	ComPtr<ID3D12PipelineState> m_tessPSO;       // VS→HS→DS→PS, solid
	ComPtr<ID3D12PipelineState> m_tessPSOWire;   // VS→HS→DS→PS, wireframe
	ComPtr<ID3D12PipelineState> m_lightPSO;
	ComPtr<ID3D12PipelineState> m_debugPSO;

	ComPtr<ID3D12RootSignature> m_shadowRootSig;
	ComPtr<ID3D12PipelineState> m_shadowPSO;           // статическая геометрия
	ComPtr<ID3D12PipelineState> m_shadowPSOInstanced;  // поле коробок

	// ── Константные буферы ──────────────────────────────────────────────────
	std::unique_ptr<UploadBuffer<ObjectConstants>>     m_objectCB;
	std::unique_ptr<UploadBuffer<GeoPassConstants>>    m_geoPassCB;
	std::unique_ptr<UploadBuffer<MaterialConstants>>   m_materialCB;   // по элементу на материал
	std::unique_ptr<UploadBuffer<LightPassConstants>>  m_lightPassCB;
	std::unique_ptr<UploadBuffer<LightConstants>>      m_lightCB;      // по элементу на источник
	std::unique_ptr<UploadBuffer<ShadowPassConstants>> m_shadowCB;     // по элементу на каскад

	UINT m_materialCBStride = 0;   // выровненный размер элемента (256)
	UINT m_lightCBStride    = 0;
	UINT m_shadowCBStride   = 0;

	// ── Сцена ───────────────────────────────────────────────────────────────
	ComPtr<ID3D12Resource>   m_modelVB;
	D3D12_VERTEX_BUFFER_VIEW m_modelVBV{};
	UINT                     m_modelVertexCount = 0;

	std::vector<SubMesh>  m_subMeshes;
	std::vector<Material> m_materials;    // [0] = материал по умолчанию

	std::unordered_map<std::wstring, ComPtr<ID3D12Resource>> m_textureCache;
	ComPtr<ID3D12Resource> m_whiteTex;

	// SRV-куча материалов: по 2 дескриптора на материал (diffuse, alpha)
	ComPtr<ID3D12DescriptorHeap> m_matSrvHeap;
	UINT                         m_srvDescSize = 0;

	DirectX::XMFLOAT3 m_modelCenter = { 0.0f, 0.0f, 0.0f };
	float             m_modelScale  = 1.0f;
	DirectX::XMFLOAT3 m_boundsMin   = { 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 m_boundsMax   = { 0.0f, 0.0f, 0.0f };

	// ── G-Buffer ────────────────────────────────────────────────────────────
	GBuffer m_gbuffer;

	// ── Источники света ─────────────────────────────────────────────────────
	std::vector<LightConstants> m_lights;
	float m_lightIntensityScale = 1.0f;

	// ── Состояние UV (перенесено из ДЗ №1) ──────────────────────────────────
	DirectX::XMFLOAT2 m_uvOffset      = { 0.0f, 0.0f };
	DirectX::XMFLOAT2 m_uvTile        = { 1.0f, 1.0f };
	float             m_uvAnimSpeed   = 0.3f;
	bool              m_uvAnimEnabled = false;

	uint32_t m_debugMode = 0;

	// ── Тесселяция и normal mapping (ДЗ №3) ─────────────────────────────────
	bool  m_tessEnabled       = true;
	bool  m_wireframe         = false;
	bool  m_normalMapEnabled  = true;
	bool  m_flipGreenChannel  = false;
	bool  m_backfaceCullHS    = false;

	float m_displacementScale = 0.008f;
	float m_tessFactorMax     = 8.0f;
	float m_tessDistNear      = 0.25f;
	float m_tessDistFar       = 3.00f;

	// ── Поле объектов и отсечение (ДЗ №4) ───────────────────────────────────
	ObjectField m_objectField;
	CullMode    m_cullMode = CullMode::Octree;

	// ── Каскадные тени (ДЗ №5) ──────────────────────────────────────────────
	ShadowMap m_shadowMap;
	bool      m_shadowsEnabled = true;
	bool      m_showCascades   = false;
	float     m_shadowBias     = 0.0018f;

	bool              m_freezeFrustum  = false;
	DirectX::XMFLOAT4X4 m_frozenViewProj = dx::Identity4x4();
	DirectX::XMFLOAT4X4 m_lastViewProj   = dx::Identity4x4();

	// Синхронизация для одноразовых загрузок
	ComPtr<ID3D12Fence> m_uploadFence;
	UINT64              m_uploadFenceValue = 0;
};

#endif // !RENDERING_SYSTEM_HPP
