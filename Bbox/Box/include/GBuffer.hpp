#ifndef GBUFFER_HPP
#define GBUFFER_HPP

#include "Dx12Common.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// GBuffer — набор render target'ов, куда geometry pass складывает
// свойства поверхности БЕЗ освещения (лекция 03, слайды 9–10, 17).
//
// Раскладка (3 таргета + глубина):
//   t0  Albedo    R8G8B8A8_UNORM        rgb = диффузный цвет, a = 1
//   t1  Normal    R16G16B16A16_FLOAT    xyz = мировая нормаль (без упаковки)
//   t2  Specular  R8G8B8A8_UNORM        rgb = Ks, a = Ns / 255
//   t3  Depth     R24_UNORM_X8_TYPELESS (SRV на depth-buffer из Framework)
//
// Мировые координаты НЕ храним — они избыточны (слайд 17): в light pass
// восстанавливаем их из глубины и InvViewProj. Это экономит целый таргет
// формата R32G32B32A32_FLOAT (16 байт на пиксель).
//
// Класс не владеет depth-буфером: он живёт в Framework (там же, где swap
// chain, и пересоздаётся в OnResize). GBuffer только создаёт на него SRV
// и следит за его состоянием ресурса.
// ─────────────────────────────────────────────────────────────────────────────
class GBuffer {
public:
	enum RtIndex : UINT {
		RT_Albedo   = 0,
		RT_Normal   = 1,
		RT_Specular = 2,
		RT_Count    = 3
	};

	// Число SRV в куче: 3 таргета + глубина + массив каскадов теней (t4).
	// Тени лежат здесь же, потому что одновременно можно привязать только одну
	// кучу типа CBV_SRV_UAV — держим все ресурсы светового прохода в ней.
	static constexpr UINT SrvCount = RT_Count + 2;

	// Записывает дескриптор карты теней в слот t4. Вызывается один раз после
	// создания ShadowMap; Resize() трогает только слоты t0..t3.
	void SetShadowSrv(ID3D12Device* device, ID3D12Resource* shadowArray, UINT cascadeCount);

	static DXGI_FORMAT Format(UINT index);

	// Создаёт RTV/SRV кучи. Размеров ещё не знает — их даёт Resize().
	void Init(ID3D12Device* device);

	// Пересоздаёт текстуры под новый размер окна и (пере)создаёт все views.
	// depthBuffer должен быть создан с TYPELESS-форматом, иначе SRV на него
	// создать нельзя.
	void Resize(ID3D12Device* device, UINT width, UINT height, ID3D12Resource* depthBuffer);

	// Начало непрерывного диапазона из RT_Count RTV-дескрипторов.
	D3D12_CPU_DESCRIPTOR_HANDLE RtvStart() const;

	ID3D12DescriptorHeap*       SrvHeap()     const { return m_srvHeap.Get(); }
	D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuStart() const;

	// PIXEL_SHADER_RESOURCE → RENDER_TARGET / DEPTH_WRITE (перед geometry pass)
	void TransitionToWrite(ID3D12GraphicsCommandList* cmd);

	// RENDER_TARGET / DEPTH_WRITE → PIXEL_SHADER_RESOURCE (перед light pass)
	void TransitionToRead(ID3D12GraphicsCommandList* cmd);

	// Очищает все таргеты и depth/stencil.
	void Clear(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE dsv);

	bool IsReady() const { return m_rt[0] != nullptr; }

private:
	ComPtr<ID3D12Resource>       m_rt[RT_Count];
	ID3D12Resource*              m_depth = nullptr;      // не владеем

	ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
	ComPtr<ID3D12DescriptorHeap> m_srvHeap;

	UINT m_rtvSize = 0;
	UINT m_srvSize = 0;

	UINT m_width  = 0;
	UINT m_height = 0;

	// Текущее состояние ресурсов — чтобы не выдавать барьер с
	// StateBefore == StateAfter (debug layer считает это ошибкой).
	D3D12_RESOURCE_STATES m_colorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	D3D12_RESOURCE_STATES m_depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
};

#endif // !GBUFFER_HPP
