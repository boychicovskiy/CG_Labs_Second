#ifndef SHADOW_MAP_HPP
#define SHADOW_MAP_HPP

#include <array>
#include <DirectXMath.h>

#include "Dx12Common.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// ShadowMap — каскадные карты теней для направленного источника (ДЗ №5).
//
// Хранилище: одна текстура Texture2DArray формата R32_TYPELESS, по срезу
// на каскад. Для записи на каждый срез заводится свой DSV (формат D32_FLOAT),
// для чтения — один SRV на весь массив (R32_FLOAT), он живёт в SRV-куче
// G-Buffer'а, чтобы световой проход обходился одной descriptor table.
//
// Идея каскадов (слайды 22–24): близким объектам нужно больше текселей карты,
// чем далёким. Фрустум камеры режется на несколько под-фрустумов, для каждого
// строится своя плотно подогнанная ортогональная проекция, а в пиксельном
// шейдере каскад выбирается по глубине пикселя.
// ─────────────────────────────────────────────────────────────────────────────
class ShadowMap {
public:
	static constexpr UINT MaxCascades = 4;

	void Init(ID3D12Device* device, UINT size, UINT cascadeCount);

	// Пересчитывает границы каскадов и матрицы света. Вызывается каждый кадр:
	// каскады привязаны к текущему положению камеры.
	void UpdateCascades(const DirectX::XMMATRIX& cameraView,
	                    float fovY, float aspect,
	                    float nearZ, float farZ,
	                    const DirectX::XMFLOAT3& lightDirW);

	D3D12_CPU_DESCRIPTOR_HANDLE Dsv(UINT cascade) const;
	ID3D12Resource*             Resource() const { return m_texture.Get(); }

	UINT Size()          const { return m_size; }
	UINT CascadeCount()  const { return m_cascadeCount; }
	float TexelSize()    const { return (m_size > 0) ? (1.0f / static_cast<float>(m_size)) : 0.0f; }

	const DirectX::XMFLOAT4X4& LightViewProj(UINT cascade) const { return m_lightViewProj[cascade]; }

	// Дальние границы каскадов в пространстве камеры — по ним шейдер выбирает каскад
	const DirectX::XMFLOAT4& SplitDistances() const { return m_splitDistances; }

	const D3D12_VIEWPORT& Viewport() const { return m_viewport; }
	const D3D12_RECT&     Scissor()  const { return m_scissor; }

	// DEPTH_WRITE <-> PIXEL_SHADER_RESOURCE
	void TransitionToWrite(ID3D12GraphicsCommandList* cmd);
	void TransitionToRead(ID3D12GraphicsCommandList* cmd);

	// Коэффициент смешивания логарифмического и равномерного распределения:
	// 0 — равномерное, 1 — чисто логарифмическое (слайды 24–25)
	void  SetLambda(float lambda);
	float Lambda() const { return m_lambda; }

private:
	ComPtr<ID3D12Resource>       m_texture;
	ComPtr<ID3D12DescriptorHeap> m_dsvHeap;

	UINT m_dsvSize       = 0;
	UINT m_size          = 2048;
	UINT m_cascadeCount  = MaxCascades;

	float m_lambda = 0.75f;

	std::array<DirectX::XMFLOAT4X4, MaxCascades> m_lightViewProj{};
	DirectX::XMFLOAT4 m_splitDistances = { 0.0f, 0.0f, 0.0f, 0.0f };

	D3D12_VIEWPORT m_viewport = {};
	D3D12_RECT     m_scissor  = {};

	D3D12_RESOURCE_STATES m_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
};

#endif // !SHADOW_MAP_HPP
