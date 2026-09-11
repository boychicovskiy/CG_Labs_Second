#ifndef POST_PROCESS_HPP
#define POST_PROCESS_HPP

#include <memory>

#include "Dx12Common.hpp"
#include "UploadBuffer.hpp"
#include "RenderStructs.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// PostProcess — конвейер постобработки (лаба 7, лекции 08.1 и 08.2).
//
// До этой работы световой проход писал прямо в бэк-буфер формата
// R8G8B8A8_UNORM. Значения там обрезаются на единице, поэтому яркие места
// «выгорали» (saturation, слайд 5 лекции 08.2), а гамма вообще не учитывалась.
//
// Теперь между освещением и экраном стоит цепочка:
//
//   Light pass ──> HDR (R16G16B16A16_FLOAT, линейное пространство)
//                    │
//                    ├─> Bright pass  ──> Bloom[0]   (четверть разрешения)
//                    │      Blur H    ──> Bloom[1]
//                    │      Blur V    ──> Bloom[0]
//                    │
//                    └─> Composite ──> бэк-буфер (RTV формата _SRGB)
//                          экспозиция, тональное отображение, bloom,
//                          виньетка, хроматическая аберрация, дизеринг
//
// Гамма-коррекцию делает аппаратура: RTV бэк-буфера создан с форматом
// _SRGB, поэтому шейдер отдаёт линейный цвет, а кодирование в sRGB
// выполняет блок вывода (слайд 43 лекции 08.1).
// ─────────────────────────────────────────────────────────────────────────────
class PostProcess {
public:
	// Промежуточный буфер обязан быть с плавающей точкой: в 8-битном UNORM
	// после тонального отображения значения 1..20 просто не используются
	// и получаются цветовые полосы (слайд 51 лекции 08.1).
	static constexpr DXGI_FORMAT HdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

	// Во сколько раз буфер свечения меньше экрана. Уменьшение само по себе
	// размывает, поэтому ядро размытия можно держать коротким.
	static constexpr UINT BloomDivisor = 4;

	void Init(ID3D12Device* device, DXGI_FORMAT backBufferRtvFormat);
	void Resize(ID3D12Device* device, UINT width, UINT height);

	// Перевести HDR-буфер в состояние render target и очистить.
	// Вызывается перед световым проходом.
	void BeginScene(ID3D12GraphicsCommandList* cmd);

	D3D12_CPU_DESCRIPTOR_HANDLE HdrRtv() const;

	// Вся цепочка: bright pass, размытие, композит в бэк-буфер.
	void Execute(ID3D12GraphicsCommandList* cmd,
	             D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
	             const D3D12_VIEWPORT& viewport,
	             const D3D12_RECT& scissor);

	// ── Переключатели ───────────────────────────────────────────────────────
	void ToggleBloom();
	void ToggleDither();
	void ToggleVignette();
	void CycleToneMap();
	void ScaleExposure(float factor);
	void ScaleBloomThreshold(float factor);

	// Режимы отладки G-Buffer должны показываться как есть, без тонального
	// отображения и свечения.
	void SetPassthrough(bool on) { m_passthrough = on; }

	float    Exposure()    const { return m_exposure; }
	uint32_t ToneMapMode() const { return m_toneMapMode; }
	bool     BloomEnabled() const { return m_bloomEnabled; }

private:
	void BuildRootSignature(ID3D12Device* device);
	void BuildPipelines(ID3D12Device* device);
	void CreateDescriptorHeaps(ID3D12Device* device);
	void CreateViews(ID3D12Device* device);

	// Индексы в кучах
	enum : UINT { Rtv_Hdr = 0, Rtv_Bloom0 = 1, Rtv_Bloom1 = 2, Rtv_Count = 3 };

	// SRV-куча выложена так, чтобы каждому проходу хватало пары подряд
	// идущих дескрипторов (таблица всегда описывает t0 и t1):
	//   [0] HDR      [1] Bloom0     -> bright pass и композит
	//   [1] Bloom0   [2] Bloom1     -> горизонтальное размытие
	//   [2] Bloom1   [3] Bloom0     -> вертикальное размытие
	enum : UINT { Srv_Hdr = 0, Srv_Bloom0 = 1, Srv_Bloom1 = 2, Srv_Bloom0Dup = 3, Srv_Count = 4 };

	// По элементу константного буфера на проход
	enum : UINT { Cb_Bright = 0, Cb_BlurH = 1, Cb_BlurV = 2, Cb_Composite = 3, Cb_Count = 4 };

	D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT index) const;
	D3D12_GPU_DESCRIPTOR_HANDLE SrvTable(UINT index) const;

	ComPtr<ID3D12Resource> m_hdr;
	ComPtr<ID3D12Resource> m_bloom[2];

	ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
	ComPtr<ID3D12DescriptorHeap> m_srvHeap;

	UINT m_rtvSize = 0;
	UINT m_srvSize = 0;

	UINT m_width  = 0;
	UINT m_height = 0;
	UINT m_bloomWidth  = 0;
	UINT m_bloomHeight = 0;

	DXGI_FORMAT m_backBufferRtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

	std::unique_ptr<UploadBuffer<PostConstants>> m_cb;
	UINT m_cbStride = 0;

	ComPtr<ID3DBlob> m_vs, m_brightPS, m_blurPS, m_compositePS;

	ComPtr<ID3D12RootSignature> m_rootSig;
	ComPtr<ID3D12PipelineState> m_bloomPSO;      // bright pass и размытие (формат HDR)
	ComPtr<ID3D12PipelineState> m_blurPSO;
	ComPtr<ID3D12PipelineState> m_compositePSO;  // в бэк-буфер

	// Состояния ресурсов отслеживаем сами, чтобы не выдавать барьер
	// с StateBefore == StateAfter.
	D3D12_RESOURCE_STATES m_hdrState      = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	D3D12_RESOURCE_STATES m_bloomState[2] = {
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
	};

	// ── Настройки ───────────────────────────────────────────────────────────
	bool     m_bloomEnabled    = true;
	bool     m_ditherEnabled   = true;
	bool     m_vignetteEnabled = true;
	bool     m_passthrough     = false;

	uint32_t m_toneMapMode     = 1;        // 0 выкл, 1 Рейнхард, 2 экспозиционный
	float    m_exposure        = 1.6f;
	float    m_bloomThreshold  = 1.0f;
	float    m_bloomIntensity  = 0.60f;
};

#endif // !POST_PROCESS_HPP
