// Windows.h min/max macros collide with std::min / std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ObjectField.hpp"
#include "GBuffer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

using namespace DirectX;

namespace {

	// Локальный куб со стороной 2 (координаты ±1): в вершинном шейдере
	// умножается на полуразмер экземпляра, поэтому AABB объекта получается
	// ровно тем же, что использует отсечение.
	void MakeCube(std::vector<Vertex>& verts, std::vector<uint16_t>& indices)
	{
		struct Face { XMFLOAT3 n, u, v; };

		const Face faces[6] = {
			{ {  0,  0, -1 }, { 1, 0, 0 }, { 0, 1, 0 } },  // -Z
			{ {  0,  0,  1 }, { -1, 0, 0 }, { 0, 1, 0 } }, // +Z
			{ { -1,  0,  0 }, { 0, 0, -1 }, { 0, 1, 0 } }, // -X
			{ {  1,  0,  0 }, { 0, 0, 1 }, { 0, 1, 0 } },  // +X
			{ {  0, -1,  0 }, { 1, 0, 0 }, { 0, 0, -1 } }, // -Y
			{ {  0,  1,  0 }, { 1, 0, 0 }, { 0, 0, 1 } },  // +Y
		};

		verts.clear();
		indices.clear();

		for (int f = 0; f < 6; ++f)
		{
			const Face& face = faces[f];
			const uint16_t base = static_cast<uint16_t>(verts.size());

			const float su[4] = { -1.0f,  1.0f,  1.0f, -1.0f };
			const float sv[4] = { -1.0f, -1.0f,  1.0f,  1.0f };

			for (int i = 0; i < 4; ++i)
			{
				Vertex v{};
				v.Pos = {
					face.n.x + su[i] * face.u.x + sv[i] * face.v.x,
					face.n.y + su[i] * face.u.y + sv[i] * face.v.y,
					face.n.z + su[i] * face.u.z + sv[i] * face.v.z
				};
				v.Normal   = face.n;
				v.TangentU = { face.u.x, face.u.y, face.u.z, 1.0f };
				v.TexCoord = { 0.5f * (su[i] + 1.0f), 0.5f * (sv[i] + 1.0f) };
				verts.push_back(v);
			}

			indices.push_back(base + 0); indices.push_back(base + 1); indices.push_back(base + 2);
			indices.push_back(base + 0); indices.push_back(base + 2); indices.push_back(base + 3);
		}
	}

	ComPtr<ID3D12Resource> CreateUploadBufferRaw(ID3D12Device* device, UINT64 byteSize)
	{
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width            = byteSize;
		desc.Height           = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels        = 1;
		desc.Format           = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		D3D12_HEAP_PROPERTIES props = {};
		props.Type = D3D12_HEAP_TYPE_UPLOAD;

		ComPtr<ID3D12Resource> res;
		ThrowIfFailed(device->CreateCommittedResource(
			&props, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res)));

		return res;
	}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Init
// ═════════════════════════════════════════════════════════════════════════════
void ObjectField::Init(ID3D12Device* device,
                       const AABB& region,
                       uint32_t objectCount,
                       DXGI_FORMAT depthStencilFormat)
{
	BuildCubeMesh(device);
	BuildObjects(region, objectCount);

	// Дерево строится один раз: объекты статичны
	m_octree.Build(m_objectBounds, /*maxDepth*/ 8, /*maxObjectsPerNode*/ 8);

	m_instanceCapacity = objectCount;

	const UINT64 instBytes = static_cast<UINT64>(m_instanceCapacity) * sizeof(InstanceData);
	m_instanceBuffer = CreateUploadBufferRaw(device, instBytes);

	// Буфер держим замапленным на всё время жизни: он в UPLOAD-куче, а кадр
	// синхронизирован через FlushCommandQueue, поэтому гонки с GPU нет.
	ThrowIfFailed(m_instanceBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_instanceMapped)));

	m_instanceVBV.BufferLocation = m_instanceBuffer->GetGPUVirtualAddress();
	m_instanceVBV.StrideInBytes  = sizeof(InstanceData);
	m_instanceVBV.SizeInBytes    = static_cast<UINT>(instBytes);

	// Статическая копия со всеми объектами — используется проходом карты теней
	{
		m_allInstancesBuffer = CreateUploadBufferRaw(device, instBytes);

		InstanceData* mapped = nullptr;
		ThrowIfFailed(m_allInstancesBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));

		for (size_t i = 0; i < m_objectBounds.size(); ++i)
		{
			mapped[i].Center = m_objectBounds[i].Center();
			mapped[i].Extent = m_objectBounds[i].Extent();
			mapped[i].Color  = m_objectColors[i];
		}

		m_allInstancesBuffer->Unmap(0, nullptr);

		m_allInstancesVBV.BufferLocation = m_allInstancesBuffer->GetGPUVirtualAddress();
		m_allInstancesVBV.StrideInBytes  = sizeof(InstanceData);
		m_allInstancesVBV.SizeInBytes    = static_cast<UINT>(instBytes);
	}

	BuildNodeBoxInstances();

	// Коробки узлов статичны — заливаем один раз
	if (m_nodeBoxCount > 0)
	{
		std::vector<AABB> boxes;
		m_octree.CollectNodeBoxes(boxes);

		const UINT64 boxBytes = static_cast<UINT64>(boxes.size()) * sizeof(InstanceData);
		m_nodeBoxBuffer = CreateUploadBufferRaw(device, boxBytes);

		InstanceData* mapped = nullptr;
		ThrowIfFailed(m_nodeBoxBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));

		for (size_t i = 0; i < boxes.size(); ++i)
		{
			mapped[i].Center = boxes[i].Center();
			mapped[i].Extent = boxes[i].Extent();
			mapped[i].Color  = { 0.2f, 1.0f, 0.4f, 1.0f };
		}

		m_nodeBoxBuffer->Unmap(0, nullptr);

		m_nodeBoxVBV.BufferLocation = m_nodeBoxBuffer->GetGPUVirtualAddress();
		m_nodeBoxVBV.StrideInBytes  = sizeof(InstanceData);
		m_nodeBoxVBV.SizeInBytes    = static_cast<UINT>(boxBytes);
	}

	m_passCB = std::make_unique<UploadBuffer<InstancePassConstants>>(device, 1, true);

	m_vs = CompileShader(L"shader\\Instanced.hlsl", nullptr, "VS", "vs_5_1");
	m_ps = CompileShader(L"shader\\Instanced.hlsl", nullptr, "PS", "ps_5_1");

	BuildRootSignature(device);
	BuildPSO(device, depthStencilFormat);

	m_visibleIndices.reserve(objectCount);
	m_stats.totalObjects = objectCount;

#if defined(_DEBUG)
	char buf[160];
	sprintf_s(buf, "[FIELD] %u objects, octree: %zu nodes, depth %d\n",
	          objectCount, m_octree.NodeCount(), m_octree.MaxDepth());
	OutputDebugStringA(buf);
#endif
}

// ═════════════════════════════════════════════════════════════════════════════
// Геометрия куба
// ═════════════════════════════════════════════════════════════════════════════
void ObjectField::BuildCubeMesh(ID3D12Device* device)
{
	std::vector<Vertex>   verts;
	std::vector<uint16_t> indices;
	MakeCube(verts, indices);

	m_cubeIndexCount = static_cast<UINT>(indices.size());

	const UINT vbBytes = static_cast<UINT>(verts.size() * sizeof(Vertex));
	const UINT ibBytes = static_cast<UINT>(indices.size() * sizeof(uint16_t));

	m_cubeVB = CreateUploadBufferRaw(device, vbBytes);
	m_cubeIB = CreateUploadBufferRaw(device, ibBytes);

	void* p = nullptr;
	ThrowIfFailed(m_cubeVB->Map(0, nullptr, &p));
	std::memcpy(p, verts.data(), vbBytes);
	m_cubeVB->Unmap(0, nullptr);

	ThrowIfFailed(m_cubeIB->Map(0, nullptr, &p));
	std::memcpy(p, indices.data(), ibBytes);
	m_cubeIB->Unmap(0, nullptr);

	m_cubeVBV.BufferLocation = m_cubeVB->GetGPUVirtualAddress();
	m_cubeVBV.StrideInBytes  = sizeof(Vertex);
	m_cubeVBV.SizeInBytes    = vbBytes;

	m_cubeIBV.BufferLocation = m_cubeIB->GetGPUVirtualAddress();
	m_cubeIBV.Format         = DXGI_FORMAT_R16_UINT;
	m_cubeIBV.SizeInBytes    = ibBytes;
}

// ═════════════════════════════════════════════════════════════════════════════
// Расстановка объектов
// ═════════════════════════════════════════════════════════════════════════════
void ObjectField::BuildObjects(const AABB& region, uint32_t objectCount)
{
	// Фиксированное зерно: расстановка одинакова от запуска к запуску, иначе
	// сравнивать замеры производительности между режимами бессмысленно.
	std::mt19937 rng(1337u);

	std::uniform_real_distribution<float> ux(region.Min.x, region.Max.x);
	std::uniform_real_distribution<float> uy(region.Min.y, region.Max.y);
	std::uniform_real_distribution<float> uz(region.Min.z, region.Max.z);

	const XMFLOAT3 ext = region.Extent();
	const float baseSize = 0.010f * std::max(ext.x, std::max(ext.y, ext.z));

	std::uniform_real_distribution<float> usize(0.6f * baseSize, 2.0f * baseSize);
	std::uniform_real_distribution<float> uhue(0.0f, 1.0f);

	m_objectBounds.clear();
	m_objectColors.clear();
	m_objectBounds.reserve(objectCount);
	m_objectColors.reserve(objectCount);

	for (uint32_t i = 0; i < objectCount; ++i)
	{
		const XMFLOAT3 c = { ux(rng), uy(rng), uz(rng) };
		const XMFLOAT3 e = { usize(rng), usize(rng), usize(rng) };

		AABB box;
		box.Min = { c.x - e.x, c.y - e.y, c.z - e.z };
		box.Max = { c.x + e.x, c.y + e.y, c.z + e.z };
		m_objectBounds.push_back(box);

		// Простая радуга по оттенку — так глазом видно, что объекты разные
		const float h = uhue(rng) * 6.0f;
		const int   s = static_cast<int>(h) % 6;
		const float f = h - static_cast<float>(static_cast<int>(h));

		XMFLOAT4 col{ 1.0f, 1.0f, 1.0f, 1.0f };
		switch (s)
		{
		case 0: col = { 1.0f,    f,     0.0f, 1.0f }; break;
		case 1: col = { 1.0f - f, 1.0f, 0.0f, 1.0f }; break;
		case 2: col = { 0.0f,    1.0f,  f,    1.0f }; break;
		case 3: col = { 0.0f, 1.0f - f, 1.0f, 1.0f }; break;
		case 4: col = { f,       0.0f,  1.0f, 1.0f }; break;
		default:col = { 1.0f,    0.0f, 1.0f - f, 1.0f }; break;
		}

		m_objectColors.push_back(col);
	}
}

void ObjectField::BuildNodeBoxInstances()
{
	m_nodeBoxCount = static_cast<UINT>(m_octree.NodeCount());
}

// ═════════════════════════════════════════════════════════════════════════════
// Root signature: один root CBV. Держим её маленькой (слайд 34).
// ═════════════════════════════════════════════════════════════════════════════
void ObjectField::BuildRootSignature(ID3D12Device* device)
{
	D3D12_ROOT_PARAMETER param = {};
	param.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
	param.Descriptor.ShaderRegister = 0;
	param.ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

	D3D12_ROOT_SIGNATURE_DESC desc = {};
	desc.NumParameters = 1;
	desc.pParameters   = &param;
	desc.Flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	ComPtr<ID3DBlob> blob, errors;
	HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
	                                         blob.GetAddressOf(), errors.GetAddressOf());
	if (errors) OutputDebugStringA((const char*)errors->GetBufferPointer());
	ThrowIfFailed(hr);

	ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
	                                          IID_PPV_ARGS(&m_rootSig)));
}

// ═════════════════════════════════════════════════════════════════════════════
// PSO: пишет в тот же G-Buffer, что и основная геометрия
// ═════════════════════════════════════════════════════════════════════════════
void ObjectField::BuildPSO(ID3D12Device* device, DXGI_FORMAT depthStencilFormat)
{
	// Слот 0 — вершины куба, слот 1 — данные экземпляра.
	// InstanceDataStepRate = 1: новые значения раз в экземпляр, а не в вершину.
	D3D12_INPUT_ELEMENT_DESC inputLayout[] =
	{
		{ "POSITION",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
		{ "NORMAL",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },

		{ "INSTCENTER", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0,
		  D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
		{ "INSTEXTENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16,
		  D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
		{ "INSTCOLOR",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32,
		  D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
	};

	D3D12_RASTERIZER_DESC raster = {};
	raster.FillMode              = D3D12_FILL_MODE_SOLID;
	raster.CullMode              = D3D12_CULL_MODE_BACK;
	raster.FrontCounterClockwise = FALSE;
	raster.DepthBias             = D3D12_DEFAULT_DEPTH_BIAS;
	raster.DepthBiasClamp        = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
	raster.SlopeScaledDepthBias  = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	raster.DepthClipEnable       = TRUE;
	raster.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	D3D12_BLEND_DESC blend = {};
	for (UINT i = 0; i < GBuffer::RT_Count; ++i)
	{
		blend.RenderTarget[i].BlendEnable           = FALSE;
		blend.RenderTarget[i].SrcBlend              = D3D12_BLEND_ONE;
		blend.RenderTarget[i].DestBlend             = D3D12_BLEND_ZERO;
		blend.RenderTarget[i].BlendOp               = D3D12_BLEND_OP_ADD;
		blend.RenderTarget[i].SrcBlendAlpha         = D3D12_BLEND_ONE;
		blend.RenderTarget[i].DestBlendAlpha        = D3D12_BLEND_ZERO;
		blend.RenderTarget[i].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
		blend.RenderTarget[i].LogicOp               = D3D12_LOGIC_OP_NOOP;
		blend.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	}

	D3D12_DEPTH_STENCIL_DESC ds = {};
	ds.DepthEnable      = TRUE;
	ds.DepthWriteMask   = D3D12_DEPTH_WRITE_MASK_ALL;
	ds.DepthFunc        = D3D12_COMPARISON_FUNC_LESS;
	ds.StencilEnable    = FALSE;
	ds.StencilReadMask  = D3D12_DEFAULT_STENCIL_READ_MASK;
	ds.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
	ds.FrontFace.StencilFailOp      = D3D12_STENCIL_OP_KEEP;
	ds.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	ds.FrontFace.StencilPassOp      = D3D12_STENCIL_OP_KEEP;
	ds.FrontFace.StencilFunc        = D3D12_COMPARISON_FUNC_ALWAYS;
	ds.BackFace = ds.FrontFace;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
	pso.InputLayout           = { inputLayout, _countof(inputLayout) };
	pso.pRootSignature        = m_rootSig.Get();
	pso.VS                    = { m_vs->GetBufferPointer(), m_vs->GetBufferSize() };
	pso.PS                    = { m_ps->GetBufferPointer(), m_ps->GetBufferSize() };
	pso.RasterizerState       = raster;
	pso.BlendState            = blend;
	pso.DepthStencilState     = ds;
	pso.SampleMask            = D3D12_DEFAULT_SAMPLE_MASK;
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets      = GBuffer::RT_Count;
	for (UINT i = 0; i < GBuffer::RT_Count; ++i)
		pso.RTVFormats[i] = GBuffer::Format(i);
	pso.DSVFormat             = depthStencilFormat;
	pso.SampleDesc.Count      = 1;

	ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));

	// Каркасный вариант — для коробок узлов дерева
	pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoWire)));
}

// ═════════════════════════════════════════════════════════════════════════════
// Update — отсечение и заполнение буфера экземпляров
// ═════════════════════════════════════════════════════════════════════════════
void ObjectField::Update(const XMMATRIX& renderViewProj, const XMMATRIX& cullViewProj, CullMode mode)
{
	// Рисуем всегда с живой камеры
	InstancePassConstants pc;
	XMStoreFloat4x4(&pc.ViewProj, XMMatrixTranspose(renderViewProj));
	m_passCB->CopyData(0, pc);

	if (!m_instanceMapped)
		return;

	const auto t0 = std::chrono::high_resolution_clock::now();

	// А отсекаем по той матрице, которую передали для отсечения
	m_frustum.ExtractFromViewProj(cullViewProj);

	m_visibleIndices.clear();
	uint32_t tests = 0;

	switch (mode)
	{
	case CullMode::Disabled:
		// Ничего не отсекаем — базовая точка отсчёта для сравнения
		m_visibleIndices.resize(m_objectBounds.size());
		for (uint32_t i = 0; i < m_objectBounds.size(); ++i)
			m_visibleIndices[i] = i;
		break;

	case CullMode::BruteForce:
		// Перебор: ровно N тестов на кадр
		for (uint32_t i = 0; i < m_objectBounds.size(); ++i)
		{
			++tests;
			if (m_frustum.TestAABB(m_objectBounds[i]) != Containment::Outside)
				m_visibleIndices.push_back(i);
		}
		break;

	case CullMode::Octree:
		m_octree.Query(m_frustum, m_visibleIndices, tests);
		break;
	}

	// Заполняем буфер экземпляров только видимыми объектами
	m_visibleCount = static_cast<UINT>(std::min<size_t>(m_visibleIndices.size(), m_instanceCapacity));

	for (UINT i = 0; i < m_visibleCount; ++i)
	{
		const uint32_t idx = m_visibleIndices[i];
		const AABB& b = m_objectBounds[idx];

		m_instanceMapped[i].Center = b.Center();
		m_instanceMapped[i].Extent = b.Extent();
		m_instanceMapped[i].Color  = m_objectColors[idx];
	}

	const auto t1 = std::chrono::high_resolution_clock::now();

	m_stats.totalObjects = static_cast<uint32_t>(m_objectBounds.size());
	m_stats.drawnObjects = m_visibleCount;
	m_stats.aabbTests    = tests;
	m_stats.cullMs       = std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// ═════════════════════════════════════════════════════════════════════════════
// Render — один вызов отрисовки на всё поле
// ═════════════════════════════════════════════════════════════════════════════
void ObjectField::Render(ID3D12GraphicsCommandList* cmd)
{
	if (!m_pso || m_cubeIndexCount == 0)
		return;

	cmd->SetGraphicsRootSignature(m_rootSig.Get());
	cmd->SetGraphicsRootConstantBufferView(0, m_passCB->Resource()->GetGPUVirtualAddress());
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->IASetIndexBuffer(&m_cubeIBV);

	if (m_visibleCount > 0)
	{
		cmd->SetPipelineState(m_pso.Get());

		D3D12_VERTEX_BUFFER_VIEW views[2] = { m_cubeVBV, m_instanceVBV };
		cmd->IASetVertexBuffers(0, 2, views);

		// Instancing: одна геометрия, m_visibleCount копий (слайд 5)
		cmd->DrawIndexedInstanced(m_cubeIndexCount, m_visibleCount, 0, 0, 0);
	}

	if (m_showNodeBoxes && m_nodeBoxCount > 0)
	{
		cmd->SetPipelineState(m_psoWire.Get());

		D3D12_VERTEX_BUFFER_VIEW views[2] = { m_cubeVBV, m_nodeBoxVBV };
		cmd->IASetVertexBuffers(0, 2, views);

		cmd->DrawIndexedInstanced(m_cubeIndexCount, m_nodeBoxCount, 0, 0, 0);
	}
}

void ObjectField::ToggleNodeBoxes()
{
	m_showNodeBoxes = !m_showNodeBoxes;
	OutputDebugStringA(m_showNodeBoxes ? "[FIELD] Octree boxes ON\n" : "[FIELD] Octree boxes OFF\n");
}
