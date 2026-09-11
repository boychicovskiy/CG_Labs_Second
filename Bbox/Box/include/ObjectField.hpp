#ifndef OBJECT_FIELD_HPP
#define OBJECT_FIELD_HPP

#include <cstdint>
#include <memory>
#include <vector>

#include "Dx12Common.hpp"
#include "UploadBuffer.hpp"
#include "RenderStructs.hpp"
#include "Frustum.hpp"
#include "Octree.hpp"

// Режим отсечения (ДЗ №4, лекция 05)
enum class CullMode : uint32_t {
	Disabled   = 0,   // рисуем всё — базовая точка отсчёта
	BruteForce = 1,   // перебор всех объектов, O(N) тестов
	Octree     = 2    // обход окто-дерева
};

// Статистика за кадр — выводится в заголовок окна
struct CullStats {
	uint32_t totalObjects = 0;
	uint32_t drawnObjects = 0;
	uint32_t aabbTests    = 0;   // сколько раз выполнен тест AABB-vs-frustum
	double   cullMs       = 0.0; // время отсечения на CPU
};

// Данные одного экземпляра. Ровно 48 байт, поступают вторым вершинным
// потоком с D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA.
struct InstanceData {
	DirectX::XMFLOAT3 Center;  float _pad0 = 0.0f;
	DirectX::XMFLOAT3 Extent;  float _pad1 = 0.0f;
	DirectX::XMFLOAT4 Color;
};

// ─────────────────────────────────────────────────────────────────────────────
// ObjectField — поле из нескольких тысяч коробок, разбросанных вокруг сцены.
//
// Кадр выглядит так:
//   1. Update(): извлекаем плоскости пирамиды из ViewProj, отбираем видимые
//      объекты выбранным способом и пишем их в буфер экземпляров
//   2. Render(): один DrawIndexedInstanced на всё поле
//
// Объекты рисуются тем же geometry pass'ом, что и Sponza, и попадают в тот же
// G-Buffer, поэтому освещаются общими источниками света из ДЗ №2.
// ─────────────────────────────────────────────────────────────────────────────
class ObjectField {
public:
	void Init(ID3D12Device* device,
	          const AABB& region,
	          uint32_t objectCount,
	          DXGI_FORMAT depthStencilFormat);

	// Отсечение + заполнение буфера экземпляров.
	//
	// Две матрицы намеренно разные:
	//   renderViewProj — та, с которой сцена рисуется (живая камера)
	//   cullViewProj   — та, из которой берутся плоскости пирамиды
	// В обычном режиме они совпадают. Если пирамиду «заморозить», отсечение
	// продолжает считаться от старого положения камеры, и отлетев в сторону
	// видно ровно то, что culling выбросил.
	void Update(const DirectX::XMMATRIX& renderViewProj,
	            const DirectX::XMMATRIX& cullViewProj,
	            CullMode mode);

	void Render(ID3D12GraphicsCommandList* cmd);

	const CullStats& Stats() const { return m_stats; }

	// ── Доступ для прохода карты теней (ДЗ №5) ──────────────────────────────
	// В карту теней рисуются ВСЕ объекты, а не отобранные отсечением: объект
	// вне пирамиды камеры вполне может отбрасывать тень внутрь неё.
	// PSO и root signature ставит вызывающая сторона.
	const D3D12_VERTEX_BUFFER_VIEW& CubeVBV()        const { return m_cubeVBV; }
	const D3D12_VERTEX_BUFFER_VIEW& AllInstancesVBV() const { return m_allInstancesVBV; }
	const D3D12_INDEX_BUFFER_VIEW&  CubeIBV()        const { return m_cubeIBV; }
	UINT CubeIndexCount() const { return m_cubeIndexCount; }
	UINT TotalCount()     const { return static_cast<UINT>(m_objectBounds.size()); }

	void ToggleNodeBoxes();
	bool NodeBoxesVisible() const { return m_showNodeBoxes; }

	size_t OctreeNodeCount() const { return m_octree.NodeCount(); }
	int    OctreeDepth()     const { return m_octree.MaxDepth(); }

private:
	void BuildCubeMesh(ID3D12Device* device);
	void BuildObjects(const AABB& region, uint32_t objectCount);
	void BuildRootSignature(ID3D12Device* device);
	void BuildPSO(ID3D12Device* device, DXGI_FORMAT depthStencilFormat);
	void BuildNodeBoxInstances();

	// Геометрия куба: 24 вершины, 36 индексов, локальные координаты ±1
	ComPtr<ID3D12Resource>   m_cubeVB;
	ComPtr<ID3D12Resource>   m_cubeIB;
	D3D12_VERTEX_BUFFER_VIEW m_cubeVBV{};
	D3D12_INDEX_BUFFER_VIEW  m_cubeIBV{};
	UINT                     m_cubeIndexCount = 0;

	// Буфер экземпляров: перезаписывается каждый кадр видимыми объектами
	ComPtr<ID3D12Resource>   m_instanceBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_instanceVBV{};
	InstanceData*            m_instanceMapped = nullptr;
	UINT                     m_instanceCapacity = 0;
	UINT                     m_visibleCount = 0;

	// Статический буфер со ВСЕМИ объектами — для прохода карты теней
	ComPtr<ID3D12Resource>   m_allInstancesBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_allInstancesVBV{};

	// Отдельный буфер под отладочные коробки узлов дерева
	ComPtr<ID3D12Resource>    m_nodeBoxBuffer;
	D3D12_VERTEX_BUFFER_VIEW  m_nodeBoxVBV{};
	UINT                      m_nodeBoxCount = 0;
	bool                      m_showNodeBoxes = false;

	std::unique_ptr<UploadBuffer<InstancePassConstants>> m_passCB;

	ComPtr<ID3DBlob>            m_vs, m_ps;
	ComPtr<ID3D12RootSignature> m_rootSig;
	ComPtr<ID3D12PipelineState> m_pso;
	ComPtr<ID3D12PipelineState> m_psoWire;

	// Сцена
	std::vector<AABB>              m_objectBounds;
	std::vector<DirectX::XMFLOAT4> m_objectColors;
	Octree                         m_octree;

	// Переиспользуемые буферы, чтобы не аллоцировать каждый кадр
	std::vector<uint32_t> m_visibleIndices;

	Frustum   m_frustum;
	CullStats m_stats;
};

#endif // !OBJECT_FIELD_HPP
