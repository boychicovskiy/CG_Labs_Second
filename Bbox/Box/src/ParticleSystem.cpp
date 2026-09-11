// Windows.h min/max macros collide with std::min / std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ParticleSystem.hpp"
#include "GBuffer.hpp"

#include <algorithm>
#include <cstdio>

using namespace DirectX;

namespace {

	// Счётчик Append/Consume должен лежать по смещению, кратному 4096.
	// Мы используем смещение 0, но сам ресурс делаем с запасом.
	const UINT64 kCounterBufferSize = 256;

	ComPtr<ID3D12Resource> CreateDefaultBuffer(ID3D12Device* device, UINT64 size,
	                                           D3D12_RESOURCE_STATES state,
	                                           bool allowUav)
	{
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width            = size;
		desc.Height           = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels        = 1;
		desc.Format           = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags            = allowUav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
		                                 : D3D12_RESOURCE_FLAG_NONE;

		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;

		ComPtr<ID3D12Resource> res;
		ThrowIfFailed(device->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res)));

		return res;
	}

	void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
	                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		if (before == after) return;

		D3D12_RESOURCE_BARRIER b = {};
		b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource   = res;
		b.Transition.StateBefore = before;
		b.Transition.StateAfter  = after;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		cmd->ResourceBarrier(1, &b);
	}

	void UavBarrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		b.UAV.pResource = res;
		cmd->ResourceBarrier(1, &b);
	}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Init
// ═════════════════════════════════════════════════════════════════════════════
void ParticleSystem::Init(ID3D12Device* device,
                          const XMFLOAT3& emitterPos,
                          float sceneScale,
                          DXGI_FORMAT depthStencilFormat)
{
	m_emitterPos = emitterPos;
	m_sceneScale = sceneScale;

	m_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	CreateBuffers(device);
	CreateDescriptors(device);

	m_cb = std::make_unique<UploadBuffer<ParticleConstants>>(device, 1, true);

	m_emitCS   = CompileShader(L"shader\\ParticleCompute.hlsl", nullptr, "EmitCS",   "cs_5_1");
	m_updateCS = CompileShader(L"shader\\ParticleCompute.hlsl", nullptr, "UpdateCS", "cs_5_1");

	m_renderVS = CompileShader(L"shader\\ParticleRender.hlsl", nullptr, "VS", "vs_5_1");
	m_renderGS = CompileShader(L"shader\\ParticleRender.hlsl", nullptr, "GS", "gs_5_1");
	m_renderPS = CompileShader(L"shader\\ParticleRender.hlsl", nullptr, "PS", "ps_5_1");

	BuildRootSignatures(device);
	BuildPipelines(device, depthStencilFormat);

	// Масштабируем размеры и скорости под нормализованную сцену
	m_constants.MaxParticles = MaxParticles;
	m_constants.EmitterPos   = m_emitterPos;
	m_constants.Gravity      = { 0.0f, -0.40f * m_sceneScale, 0.0f };
	m_constants.SpeedMin     = 0.20f * m_sceneScale;
	m_constants.SpeedMax     = 0.55f * m_sceneScale;
	m_constants.SizeMin      = 0.0035f * m_sceneScale;
	m_constants.SizeMax      = 0.0090f * m_sceneScale;

#if defined(_DEBUG)
	char buf[128];
	sprintf_s(buf, "[PARTICLES] pool = %u, emitter = (%.2f %.2f %.2f)\n",
	          MaxParticles, m_emitterPos.x, m_emitterPos.y, m_emitterPos.z);
	OutputDebugStringA(buf);
#endif
}

// ═════════════════════════════════════════════════════════════════════════════
// Ресурсы
// ═════════════════════════════════════════════════════════════════════════════
void ParticleSystem::CreateBuffers(ID3D12Device* device)
{
	const UINT64 particleBytes = static_cast<UINT64>(MaxParticles) * sizeof(GpuParticle);

	for (int i = 0; i < 2; ++i)
	{
		m_particles[i] = CreateDefaultBuffer(device, particleBytes,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, /*allowUav*/ true);

		m_counters[i] = CreateDefaultBuffer(device, kCounterBufferSize,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, /*allowUav*/ true);
	}

	m_countSnapshot = CreateDefaultBuffer(device, kCounterBufferSize,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, /*allowUav*/ true);

	// D3D12_DRAW_ARGUMENTS: VertexCountPerInstance, InstanceCount,
	//                       StartVertexLocation, StartInstanceLocation
	m_drawArgs = CreateDefaultBuffer(device, sizeof(D3D12_DRAW_ARGUMENTS),
		D3D12_RESOURCE_STATE_COPY_DEST, /*allowUav*/ false);

	// Маленький буфер с нулями — источник для обнуления счётчика через
	// CopyBufferRegion. Заодно кладём сюда заготовку аргументов отрисовки.
	{
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width            = 64;
		desc.Height           = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels        = 1;
		desc.Format           = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_UPLOAD;

		ThrowIfFailed(device->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_zeroBuffer)));

		UINT* mapped = nullptr;
		ThrowIfFailed(m_zeroBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));

		mapped[0] = 0;   // ноль для сброса счётчика

		// Заготовка D3D12_DRAW_ARGUMENTS по смещению 16 байт:
		// число вершин перепишется копией счётчика, остальное постоянно.
		mapped[4] = 0;   // VertexCountPerInstance
		mapped[5] = 1;   // InstanceCount
		mapped[6] = 0;   // StartVertexLocation
		mapped[7] = 0;   // StartInstanceLocation

		m_zeroBuffer->Unmap(0, nullptr);
	}
}

void ParticleSystem::CreateDescriptors(ID3D12Device* device)
{
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.NumDescriptors = 2 * DescriptorsPerSet;
	heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_heap)));

	D3D12_CPU_DESCRIPTOR_HANDLE base = m_heap->GetCPUDescriptorHandleForHeapStart();

	auto HandleAt = [&](UINT index) {
		D3D12_CPU_DESCRIPTOR_HANDLE h = base;
		h.ptr += static_cast<SIZE_T>(index) * m_descriptorSize;
		return h;
	};

	// UAV на структурированный буфер со счётчиком: именно так работают
	// Append/Consume — счётчик живёт в отдельном ресурсе и привязан к view.
	auto MakeParticleUav = [&](ID3D12Resource* buffer, ID3D12Resource* counter,
	                           D3D12_CPU_DESCRIPTOR_HANDLE handle)
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
		uav.Format                      = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension               = D3D12_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement         = 0;
		uav.Buffer.NumElements          = MaxParticles;
		uav.Buffer.StructureByteStride  = sizeof(GpuParticle);
		uav.Buffer.CounterOffsetInBytes = 0;   // должно быть кратно 4096
		uav.Buffer.Flags                = D3D12_BUFFER_UAV_FLAG_NONE;

		device->CreateUnorderedAccessView(buffer, counter, &uav, handle);
	};

	// Сырой (raw) UAV — чтобы читать счётчик как обычные 4 байта
	auto MakeRawUav = [&](ID3D12Resource* buffer, D3D12_CPU_DESCRIPTOR_HANDLE handle)
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
		uav.Format                      = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension               = D3D12_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement         = 0;
		uav.Buffer.NumElements          = static_cast<UINT>(kCounterBufferSize / 4);
		uav.Buffer.StructureByteStride  = 0;
		uav.Buffer.CounterOffsetInBytes = 0;
		uav.Buffer.Flags                = D3D12_BUFFER_UAV_FLAG_RAW;

		device->CreateUnorderedAccessView(buffer, nullptr, &uav, handle);
	};

	auto MakeParticleSrv = [&](ID3D12Resource* buffer, D3D12_CPU_DESCRIPTOR_HANDLE handle)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
		srv.Format                     = DXGI_FORMAT_UNKNOWN;
		srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
		srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Buffer.FirstElement        = 0;
		srv.Buffer.NumElements         = MaxParticles;
		srv.Buffer.StructureByteStride = sizeof(GpuParticle);
		srv.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;

		device->CreateShaderResourceView(buffer, &srv, handle);
	};

	// Два набора дескрипторов — по одному на каждое значение чётности кадра.
	// set = индекс буфера-приёмника.
	for (UINT set = 0; set < 2; ++set)
	{
		const UINT src = 1 - set;
		const UINT b = set * DescriptorsPerSet;

		MakeParticleUav(m_particles[set].Get(), m_counters[set].Get(), HandleAt(b + 0)); // u0 append
		MakeParticleUav(m_particles[src].Get(), m_counters[src].Get(), HandleAt(b + 1)); // u1 consume
		MakeRawUav(m_countSnapshot.Get(),                              HandleAt(b + 2)); // u2 snapshot
		MakeRawUav(m_counters[set].Get(),                              HandleAt(b + 3)); // u3 dst counter
		MakeParticleSrv(m_particles[set].Get(),                        HandleAt(b + Srv_DstParticles));
	}
}

D3D12_GPU_DESCRIPTOR_HANDLE ParticleSystem::SetHandle(UINT set, UINT indexInSet) const
{
	D3D12_GPU_DESCRIPTOR_HANDLE h = m_heap->GetGPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<UINT64>(set * DescriptorsPerSet + indexInSet) * m_descriptorSize;
	return h;
}

// ═════════════════════════════════════════════════════════════════════════════
// Root signatures
// ═════════════════════════════════════════════════════════════════════════════
void ParticleSystem::BuildRootSignatures(ID3D12Device* device)
{
	// ── Compute: b0 + таблица из четырёх UAV ────────────────────────────────
	{
		D3D12_DESCRIPTOR_RANGE range = {};
		range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		range.NumDescriptors                    = 4;   // u0..u3
		range.BaseShaderRegister                = 0;
		range.RegisterSpace                     = 0;
		range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER params[2] = {};

		params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[0].Descriptor.ShaderRegister = 0;
		params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

		params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable.NumDescriptorRanges = 1;
		params[1].DescriptorTable.pDescriptorRanges   = &range;
		params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC desc = {};
		desc.NumParameters = _countof(params);
		desc.pParameters   = params;
		desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		ComPtr<ID3DBlob> blob, errors;
		HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
		                                         blob.GetAddressOf(), errors.GetAddressOf());
		if (errors) OutputDebugStringA((const char*)errors->GetBufferPointer());
		ThrowIfFailed(hr);

		ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
		                                          IID_PPV_ARGS(&m_computeRootSig)));
	}

	// ── Render: b0 + таблица с одним SRV ────────────────────────────────────
	{
		D3D12_DESCRIPTOR_RANGE range = {};
		range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors                    = 1;   // t0
		range.BaseShaderRegister                = 0;
		range.RegisterSpace                     = 0;
		range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER params[2] = {};

		params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[0].Descriptor.ShaderRegister = 0;
		params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

		params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable.NumDescriptorRanges = 1;
		params[1].DescriptorTable.pDescriptorRanges   = &range;
		// Буфер частиц читает вершинный шейдер, а не пиксельный
		params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_VERTEX;

		D3D12_ROOT_SIGNATURE_DESC desc = {};
		desc.NumParameters = _countof(params);
		desc.pParameters   = params;
		// Входной раскладки нет: вершины берутся по SV_VertexID
		desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		ComPtr<ID3DBlob> blob, errors;
		HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
		                                         blob.GetAddressOf(), errors.GetAddressOf());
		if (errors) OutputDebugStringA((const char*)errors->GetBufferPointer());
		ThrowIfFailed(hr);

		ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
		                                          IID_PPV_ARGS(&m_renderRootSig)));
	}
}

// ═════════════════════════════════════════════════════════════════════════════
// Конвейеры
// ═════════════════════════════════════════════════════════════════════════════
void ParticleSystem::BuildPipelines(ID3D12Device* device, DXGI_FORMAT depthStencilFormat)
{
	// ── Два вычислительных PSO ──────────────────────────────────────────────
	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
		pso.pRootSignature = m_computeRootSig.Get();

		pso.CS = { m_updateCS->GetBufferPointer(), m_updateCS->GetBufferSize() };
		ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_updatePSO)));

		pso.CS = { m_emitCS->GetBufferPointer(), m_emitCS->GetBufferSize() };
		ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_emitPSO)));
	}

	// ── Графический PSO: point list -> GS -> билборды в G-Buffer ────────────
	{
		D3D12_RASTERIZER_DESC raster = {};
		raster.FillMode              = D3D12_FILL_MODE_SOLID;
		raster.CullMode              = D3D12_CULL_MODE_NONE;
		raster.FrontCounterClockwise = FALSE;
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
		pso.InputLayout           = { nullptr, 0 };   // вершинного буфера нет
		pso.pRootSignature        = m_renderRootSig.Get();
		pso.VS                    = { m_renderVS->GetBufferPointer(), m_renderVS->GetBufferSize() };
		pso.GS                    = { m_renderGS->GetBufferPointer(), m_renderGS->GetBufferSize() };
		pso.PS                    = { m_renderPS->GetBufferPointer(), m_renderPS->GetBufferSize() };
		pso.RasterizerState       = raster;
		pso.BlendState            = blend;
		pso.DepthStencilState     = ds;
		pso.SampleMask            = D3D12_DEFAULT_SAMPLE_MASK;
		// На вход конвейера идут точки; треугольники появятся уже из GS
		pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
		pso.NumRenderTargets      = GBuffer::RT_Count;
		for (UINT i = 0; i < GBuffer::RT_Count; ++i)
			pso.RTVFormats[i] = GBuffer::Format(i);
		pso.DSVFormat             = depthStencilFormat;
		pso.SampleDesc.Count      = 1;

		ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_renderPSO)));
	}

	// ── Сигнатура команды для DrawInstancedIndirect ─────────────────────────
	// Число живых частиц известно только GPU (слайд 18), поэтому вызов
	// отрисовки собирается из буфера, а не задаётся с CPU.
	{
		D3D12_INDIRECT_ARGUMENT_DESC arg = {};
		arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

		D3D12_COMMAND_SIGNATURE_DESC desc = {};
		desc.ByteStride       = sizeof(D3D12_DRAW_ARGUMENTS);
		desc.NumArgumentDescs = 1;
		desc.pArgumentDescs   = &arg;

		// Root signature нужна только если команда меняет корневые аргументы
		ThrowIfFailed(device->CreateCommandSignature(&desc, nullptr,
		                                             IID_PPV_ARGS(&m_drawSignature)));
	}
}

// ═════════════════════════════════════════════════════════════════════════════
// Update — константы кадра
// ═════════════════════════════════════════════════════════════════════════════
void ParticleSystem::Update(double dt, const XMMATRIX& view, const XMMATRIX& viewProj)
{
	if (!m_cb) return;

	m_totalTime += dt;
	++m_frameIndex;

	const float deltaTime = std::min(static_cast<float>(dt), 0.05f);   // защита от рывков

	// Эмиссия на кадр (слайд 7): среднее число частиц в секунду, дробный
	// остаток переносится на следующий кадр, иначе при 144 Гц эмиссия
	// округлится вниз и поток обеднеет.
	UINT emitCount = 0;
	if (m_enabled)
	{
		const float wanted = m_emissionRate * deltaTime + m_emitRemainder;
		emitCount = static_cast<UINT>(wanted);
		m_emitRemainder = wanted - static_cast<float>(emitCount);
		emitCount = std::min<UINT>(emitCount, MaxParticles / 4);
	}
	else
	{
		m_emitRemainder = 0.0f;
	}

	// Базис билборда: строки матрицы вида — это оси камеры в мировом
	// пространстве (для ортонормированной матрицы вида обратная = транспонированная).
	XMFLOAT4X4 v;
	XMStoreFloat4x4(&v, view);

	XMStoreFloat4x4(&m_constants.ViewProj, XMMatrixTranspose(viewProj));

	m_constants.CameraRight = { v._11, v._21, v._31 };
	m_constants.CameraUp    = { v._12, v._22, v._32 };

	m_constants.DeltaTime   = deltaTime;
	m_constants.TotalTime   = static_cast<float>(m_totalTime);
	m_constants.EmitCount   = emitCount;
	m_constants.FrameIndex  = m_frameIndex;
	m_constants.EmitterPos  = m_emitterPos;

	m_cb->CopyData(0, m_constants);
}

// ═════════════════════════════════════════════════════════════════════════════
// Simulate — эмиссия и интегрирование на GPU
// ═════════════════════════════════════════════════════════════════════════════
void ParticleSystem::Simulate(ID3D12GraphicsCommandList* cmd)
{
	if (!m_updatePSO) return;

	const UINT dst = m_dstIndex;

	ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
	cmd->SetDescriptorHeaps(1, heaps);

	// Первый кадр: счётчики и снимок содержат мусор. Пока их не обнулить,
	// UpdateCS решит, что живых частиц полный буфер, и Consume() уйдёт
	// в отрицательные значения счётчика.
	if (m_needsReset)
	{
		ID3D12Resource* toZero[3] = {
			m_counters[0].Get(), m_counters[1].Get(), m_countSnapshot.Get()
		};

		for (ID3D12Resource* res : toZero)
		{
			Transition(cmd, res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			                     D3D12_RESOURCE_STATE_COPY_DEST);
			cmd->CopyBufferRegion(res, 0, m_zeroBuffer.Get(), 0, sizeof(UINT));
			Transition(cmd, res, D3D12_RESOURCE_STATE_COPY_DEST,
			                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}

		m_needsReset = false;
	}

	cmd->SetComputeRootSignature(m_computeRootSig.Get());
	cmd->SetComputeRootConstantBufferView(0, m_cb->Resource()->GetGPUVirtualAddress());
	cmd->SetComputeRootDescriptorTable(1, SetHandle(dst, 0));

	// ── 1. Обнуляем счётчик приёмника ───────────────────────────────────────
	Transition(cmd, m_counters[dst].Get(),
	           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
	cmd->CopyBufferRegion(m_counters[dst].Get(), 0, m_zeroBuffer.Get(), 0, sizeof(UINT));
	Transition(cmd, m_counters[dst].Get(),
	           D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// ── 2. Update: Consume -> интегрирование -> Append ──────────────────────
	// Диспатчим на весь пул; лишние потоки выходят по проверке снимка счётчика.
	cmd->SetPipelineState(m_updatePSO.Get());
	cmd->Dispatch(MaxParticles / ThreadGroupSize, 1, 1);

	UavBarrier(cmd, m_particles[dst].Get());
	UavBarrier(cmd, m_counters[dst].Get());

	// ── 3. Emit: новые частицы ──────────────────────────────────────────────
	if (m_constants.EmitCount > 0)
	{
		const UINT groups = (m_constants.EmitCount + ThreadGroupSize - 1) / ThreadGroupSize;

		cmd->SetPipelineState(m_emitPSO.Get());
		cmd->Dispatch(groups, 1, 1);

		UavBarrier(cmd, m_particles[dst].Get());
		UavBarrier(cmd, m_counters[dst].Get());
	}

	// ── 4. Счётчик -> снимок и аргументы отрисовки ──────────────────────────
	Transition(cmd, m_counters[dst].Get(),
	           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Transition(cmd, m_countSnapshot.Get(),
	           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);

	// Снимок читает UpdateCS следующего кадра: живой счётчик читать нельзя,
	// его в этот момент дёргают Consume() соседних потоков.
	cmd->CopyBufferRegion(m_countSnapshot.Get(), 0, m_counters[dst].Get(), 0, sizeof(UINT));

	// Постоянная часть аргументов (InstanceCount = 1 и нули) лежит в zeroBuffer
	// по смещению 16, число вершин перезапишем счётчиком.
	cmd->CopyBufferRegion(m_drawArgs.Get(), 0, m_zeroBuffer.Get(), 16, sizeof(D3D12_DRAW_ARGUMENTS));
	cmd->CopyBufferRegion(m_drawArgs.Get(), 0, m_counters[dst].Get(), 0, sizeof(UINT));

	Transition(cmd, m_countSnapshot.Get(),
	           D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Transition(cmd, m_counters[dst].Get(),
	           D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Transition(cmd, m_drawArgs.Get(),
	           D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

	// Буфер частиц читает вершинный шейдер — переводим его в состояние чтения
	Transition(cmd, m_particles[dst].Get(),
	           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

// ═════════════════════════════════════════════════════════════════════════════
// Render — билборды в G-Buffer
// ═════════════════════════════════════════════════════════════════════════════
void ParticleSystem::Render(ID3D12GraphicsCommandList* cmd)
{
	if (!m_renderPSO) return;

	const UINT dst = m_dstIndex;

	ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
	cmd->SetDescriptorHeaps(1, heaps);

	cmd->SetPipelineState(m_renderPSO.Get());
	cmd->SetGraphicsRootSignature(m_renderRootSig.Get());
	cmd->SetGraphicsRootConstantBufferView(0, m_cb->Resource()->GetGPUVirtualAddress());
	cmd->SetGraphicsRootDescriptorTable(1, SetHandle(dst, Srv_DstParticles));

	// Ни вершинного, ни индексного буфера: одна точка = одна частица
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
	cmd->IASetVertexBuffers(0, 1, nullptr);
	cmd->IASetIndexBuffer(nullptr);

	// Число вершин лежит в буфере аргументов, а не известно CPU
	cmd->ExecuteIndirect(m_drawSignature.Get(), 1, m_drawArgs.Get(), 0, nullptr, 0);

	// Возвращаем состояния к началу следующего кадра
	Transition(cmd, m_particles[dst].Get(),
	           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
	           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Transition(cmd, m_drawArgs.Get(),
	           D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST);

	// Буферы меняются ролями: приёмник этого кадра станет источником следующего
	m_dstIndex = 1 - m_dstIndex;
}

// ═════════════════════════════════════════════════════════════════════════════
// Управление
// ═════════════════════════════════════════════════════════════════════════════
void ParticleSystem::ToggleEnabled()
{
	m_enabled = !m_enabled;
	OutputDebugStringA(m_enabled ? "[PARTICLES] Emission ON\n" : "[PARTICLES] Emission OFF\n");
}

void ParticleSystem::Reset()
{
	m_needsReset = true;
	OutputDebugStringA("[PARTICLES] Reset\n");
}

void ParticleSystem::ScaleEmissionRate(float factor)
{
	m_emissionRate = std::max(100.0f, std::min(m_emissionRate * factor, 60000.0f));

#if defined(_DEBUG)
	char buf[64];
	sprintf_s(buf, "[PARTICLES] Emission rate = %.0f/sec\n", m_emissionRate);
	OutputDebugStringA(buf);
#endif
}
