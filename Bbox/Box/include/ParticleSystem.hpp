#ifndef PARTICLE_SYSTEM_HPP
#define PARTICLE_SYSTEM_HPP

#include <memory>

#include "Dx12Common.hpp"
#include "UploadBuffer.hpp"
#include "RenderStructs.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// ParticleSystem — система непрозрачных частиц целиком на GPU (ДЗ №6).
//
// Схема кадра (лекция 07, слайды 6, 19–20):
//   1. обнуляем счётчик буфера-приёмника
//   2. UpdateCS  — Consume() из буфера прошлого кадра, интегрирование сил,
//                  выжившие частицы Append() в приёмник
//   3. EmitCS    — Append() новых частиц от эмиттера
//   4. копируем счётчик приёмника в аргументы DrawInstancedIndirect
//   5. рисуем point list, геометрический шейдер разворачивает каждую точку
//      в билборд из двух треугольников
//   6. буферы меняются местами
//
// Два буфера частиц меняются ролями каждый кадр: тот, что был приёмником,
// на следующем кадре становится источником. Число живых частиц живёт только
// на GPU, поэтому отрисовка идёт через ExecuteIndirect (слайд 18).
//
// Частицы непрозрачные и пишутся в тот же G-Buffer, что и остальная
// геометрия: сортировка не нужна (её делает буфер глубины), а освещение
// достаётся бесплатно от светового прохода ДЗ №2.
// ─────────────────────────────────────────────────────────────────────────────
class ParticleSystem {
public:
	static constexpr UINT MaxParticles   = 65536;
	static constexpr UINT ThreadGroupSize = 64;

	void Init(ID3D12Device* device,
	          const DirectX::XMFLOAT3& emitterPos,
	          float sceneScale,
	          DXGI_FORMAT depthStencilFormat);

	// Обновляет константы. Базис билборда берётся из матрицы вида.
	void Update(double dt,
	            const DirectX::XMMATRIX& view,
	            const DirectX::XMMATRIX& viewProj);

	// Проходы compute: эмиссия и интегрирование. Вызывать до прохода геометрии.
	void Simulate(ID3D12GraphicsCommandList* cmd);

	// Отрисовка билбордов в G-Buffer. Вызывать внутри прохода геометрии.
	void Render(ID3D12GraphicsCommandList* cmd);

	void  ToggleEnabled();
	void  Reset();
	bool  Enabled() const { return m_enabled; }
	void  ScaleEmissionRate(float factor);
	float EmissionRate() const { return m_emissionRate; }

private:
	void CreateBuffers(ID3D12Device* device);
	void CreateDescriptors(ID3D12Device* device);
	void BuildRootSignatures(ID3D12Device* device);
	void BuildPipelines(ID3D12Device* device, DXGI_FORMAT depthStencilFormat);

	// Индексы дескрипторов внутри одного «набора» (на каждое значение чётности кадра)
	enum : UINT {
		Srv_DstParticles = 4,   // SRV на буфер-приёмник, для вершинного шейдера
		DescriptorsPerSet = 5
	};

	D3D12_GPU_DESCRIPTOR_HANDLE SetHandle(UINT set, UINT indexInSet) const;

	// ── Ресурсы ─────────────────────────────────────────────────────────────
	ComPtr<ID3D12Resource> m_particles[2];      // структурированные буферы частиц
	ComPtr<ID3D12Resource> m_counters[2];       // счётчики Append/Consume
	ComPtr<ID3D12Resource> m_countSnapshot;     // копия счётчика прошлого кадра
	ComPtr<ID3D12Resource> m_drawArgs;          // аргументы DrawInstancedIndirect
	ComPtr<ID3D12Resource> m_zeroBuffer;        // источник нулей для обнуления счётчика

	ComPtr<ID3D12DescriptorHeap> m_heap;
	UINT m_descriptorSize = 0;

	std::unique_ptr<UploadBuffer<ParticleConstants>> m_cb;

	// ── Конвейеры ───────────────────────────────────────────────────────────
	ComPtr<ID3DBlob> m_emitCS, m_updateCS;
	ComPtr<ID3DBlob> m_renderVS, m_renderGS, m_renderPS;

	ComPtr<ID3D12RootSignature> m_computeRootSig;
	ComPtr<ID3D12RootSignature> m_renderRootSig;

	ComPtr<ID3D12PipelineState> m_emitPSO;
	ComPtr<ID3D12PipelineState> m_updatePSO;
	ComPtr<ID3D12PipelineState> m_renderPSO;

	ComPtr<ID3D12CommandSignature> m_drawSignature;

	// ── Состояние ───────────────────────────────────────────────────────────
	UINT m_dstIndex = 0;        // какой из двух буферов приёмник в этом кадре
	bool m_enabled  = true;
	bool m_needsReset = true;   // обнулить счётчики на ближайшем кадре

	float m_emissionRate = 12000.0f;   // частиц в секунду
	float m_emitRemainder = 0.0f;      // дробный остаток эмиссии между кадрами

	double m_totalTime = 0.0;
	UINT   m_frameIndex = 0;

	DirectX::XMFLOAT3 m_emitterPos = { 0.0f, 0.0f, 0.0f };
	float             m_sceneScale = 1.0f;

	ParticleConstants m_constants;
};

#endif // !PARTICLE_SYSTEM_HPP
