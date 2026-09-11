// Windows.h min/max macros collide with std::min / std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ShadowMap.hpp"

#include <algorithm>
#include <cmath>

using namespace DirectX;

void ShadowMap::Init(ID3D12Device* device, UINT size, UINT cascadeCount)
{
	m_size         = size;
	m_cascadeCount = std::min(cascadeCount, MaxCascades);

	// ── Текстура: массив срезов глубины ─────────────────────────────────────
	// Формат TYPELESS по той же причине, что и основной depth-буфер: нужен
	// и DSV (D32_FLOAT, запись), и SRV (R32_FLOAT, чтение в световом проходе).
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width            = m_size;
	desc.Height           = m_size;
	desc.DepthOrArraySize = static_cast<UINT16>(m_cascadeCount);
	desc.MipLevels        = 1;
	desc.Format           = DXGI_FORMAT_R32_TYPELESS;
	desc.SampleDesc.Count = 1;
	desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE clear = {};
	clear.Format               = DXGI_FORMAT_D32_FLOAT;
	clear.DepthStencil.Depth   = 1.0f;
	clear.DepthStencil.Stencil = 0;

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	ThrowIfFailed(device->CreateCommittedResource(
		&heap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
		IID_PPV_ARGS(&m_texture)));

	m_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	// ── По DSV на каскад ────────────────────────────────────────────────────
	m_dsvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.NumDescriptors = m_cascadeCount;
	heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_dsvHeap)));

	D3D12_CPU_DESCRIPTOR_HANDLE handle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

	for (UINT i = 0; i < m_cascadeCount; ++i)
	{
		D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
		dsv.Format                         = DXGI_FORMAT_D32_FLOAT;
		dsv.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsv.Flags                          = D3D12_DSV_FLAG_NONE;
		dsv.Texture2DArray.MipSlice        = 0;
		dsv.Texture2DArray.FirstArraySlice = i;   // ровно один срез на каскад
		dsv.Texture2DArray.ArraySize       = 1;

		device->CreateDepthStencilView(m_texture.Get(), &dsv, handle);
		handle.ptr += m_dsvSize;
	}

	// Viewport карты теней — размеры КАРТЫ, а не окна (слайд 32)
	m_viewport = { 0.0f, 0.0f, static_cast<float>(m_size), static_cast<float>(m_size), 0.0f, 1.0f };
	m_scissor  = { 0, 0, static_cast<LONG>(m_size), static_cast<LONG>(m_size) };
}

D3D12_CPU_DESCRIPTOR_HANDLE ShadowMap::Dsv(UINT cascade) const
{
	D3D12_CPU_DESCRIPTOR_HANDLE h = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<SIZE_T>(cascade) * m_dsvSize;
	return h;
}

void ShadowMap::SetLambda(float lambda)
{
	m_lambda = std::max(0.0f, std::min(lambda, 1.0f));
}

// ═════════════════════════════════════════════════════════════════════════════
// UpdateCascades — вся математика каскадов (слайды 24–29)
// ═════════════════════════════════════════════════════════════════════════════
void ShadowMap::UpdateCascades(const XMMATRIX& cameraView,
                               float fovY, float aspect,
                               float nearZ, float farZ,
                               const XMFLOAT3& lightDirW)
{
	// ── 1. Нелинейное распределение каскадов ────────────────────────────────
	//
	// Practical split scheme: смесь логарифмического и равномерного деления.
	//   логарифмическое  d_i = n * (f/n)^(i/N)     — идеально по плотности
	//                                                текселей, но первый каскад
	//                                                получается крошечным
	//   равномерное      d_i = n + (f-n)*(i/N)     — грубое вблизи камеры
	// Смешиваем с коэффициентом lambda: он и даёт требуемую нелинейность.
	float splits[MaxCascades + 1] = {};
	splits[0] = nearZ;

	for (UINT i = 1; i <= m_cascadeCount; ++i)
	{
		const float p = static_cast<float>(i) / static_cast<float>(m_cascadeCount);

		const float logSplit     = nearZ * std::pow(farZ / nearZ, p);
		const float uniformSplit = nearZ + (farZ - nearZ) * p;

		splits[i] = m_lambda * logSplit + (1.0f - m_lambda) * uniformSplit;
	}

	// Дальние границы в пространстве камеры — по ним шейдер выберет каскад
	m_splitDistances = { 0.0f, 0.0f, 0.0f, 0.0f };
	float* splitOut = &m_splitDistances.x;
	for (UINT i = 0; i < m_cascadeCount; ++i)
		splitOut[i] = splits[i + 1];
	// Незадействованные каскады отправляем за дальнюю плоскость
	for (UINT i = m_cascadeCount; i < MaxCascades; ++i)
		splitOut[i] = farZ;

	const XMMATRIX invView = XMMatrixInverse(nullptr, cameraView);

	XMVECTOR lightDir = XMVector3Normalize(XMLoadFloat3(&lightDirW));

	// Вектор «вверх» не должен быть коллинеарен направлению света
	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	if (std::fabs(XMVectorGetY(lightDir)) > 0.98f)
		up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);

	const float tanHalfV = std::tan(0.5f * fovY);
	const float tanHalfH = tanHalfV * aspect;

	for (UINT c = 0; c < m_cascadeCount; ++c)
	{
		const float n = splits[c];
		const float f = splits[c + 1];

		// ── 2. Углы под-фрустума в мировом пространстве (слайд 26) ──────────
		XMVECTOR corners[8];
		int k = 0;

		const float zs[2] = { n, f };
		for (int zi = 0; zi < 2; ++zi)
		{
			const float z = zs[zi];
			for (int sy = -1; sy <= 1; sy += 2)
			{
				for (int sx = -1; sx <= 1; sx += 2)
				{
					// точка на ближней/дальней плоскости в пространстве камеры
					XMVECTOR pView = XMVectorSet(sx * tanHalfH * z, sy * tanHalfV * z, z, 1.0f);
					corners[k++] = XMVector3TransformCoord(pView, invView);
				}
			}
		}

		// ── 3. Центр и радиус описанной сферы ───────────────────────────────
		// Берём именно сферу, а не AABB: размер каскада тогда не зависит от
		// поворота камеры, и тени не «кипят» при вращении.
		XMVECTOR center = XMVectorZero();
		for (int i = 0; i < 8; ++i)
			center = XMVectorAdd(center, corners[i]);
		center = XMVectorScale(center, 1.0f / 8.0f);

		float radius = 0.0f;
		for (int i = 0; i < 8; ++i)
			radius = std::max(radius, XMVectorGetX(XMVector3Length(XMVectorSubtract(corners[i], center))));

		// Квантование радиуса — он тоже не должен дрожать от кадра к кадру
		radius = std::ceil(radius * 16.0f) / 16.0f;

		// ── 4. Матрица вида света (слайд 28) ────────────────────────────────
		// Отодвигаем «камеру света» назад так, чтобы вся сфера каскада попала
		// в объём, плюс запас на объекты позади каскада: они тоже отбрасывают
		// в него тень.
		const float backOff = radius * 2.0f;

		const XMVECTOR eye = XMVectorSubtract(center, XMVectorScale(lightDir, radius + backOff));
		XMMATRIX lightView = XMMatrixLookAtLH(eye, center, up);

		// ── 5. Ортогональная проекция по размеру каскада (слайд 29) ─────────
		XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(
			-radius, radius, -radius, radius, 0.0f, 2.0f * radius + backOff);

		// ── 6. Привязка к сетке текселей ────────────────────────────────────
		// Без этого при движении камеры карта теней сдвигается на доли текселя
		// и края теней мерцают. Смещаем проекцию так, чтобы начало координат
		// каскада попадало ровно в центр текселя.
		{
			const XMMATRIX viewProj = XMMatrixMultiply(lightView, lightProj);

			XMVECTOR origin = XMVector3TransformCoord(XMVectorZero(), viewProj);
			origin = XMVectorScale(origin, 0.5f * static_cast<float>(m_size));   // NDC -> тексели

			XMVECTOR rounded = XMVectorRound(origin);
			XMVECTOR offset  = XMVectorSubtract(rounded, origin);
			offset = XMVectorScale(offset, 2.0f / static_cast<float>(m_size));   // тексели -> NDC

			// Соглашение вектор-строка: сдвиг живёт в последней строке
			lightProj.r[3] = XMVectorAdd(
				lightProj.r[3],
				XMVectorSet(XMVectorGetX(offset), XMVectorGetY(offset), 0.0f, 0.0f));
		}

		XMStoreFloat4x4(&m_lightViewProj[c], XMMatrixMultiply(lightView, lightProj));
	}

	// Незадействованные каскады — единичные матрицы
	for (UINT c = m_cascadeCount; c < MaxCascades; ++c)
		XMStoreFloat4x4(&m_lightViewProj[c], XMMatrixIdentity());
}

// ═════════════════════════════════════════════════════════════════════════════
// Переходы состояний
// ═════════════════════════════════════════════════════════════════════════════
void ShadowMap::TransitionToWrite(ID3D12GraphicsCommandList* cmd)
{
	if (!m_texture || m_state == D3D12_RESOURCE_STATE_DEPTH_WRITE)
		return;

	D3D12_RESOURCE_BARRIER b = {};
	b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource   = m_texture.Get();
	b.Transition.StateBefore = m_state;
	b.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd->ResourceBarrier(1, &b);

	m_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
}

void ShadowMap::TransitionToRead(ID3D12GraphicsCommandList* cmd)
{
	if (!m_texture || m_state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
		return;

	D3D12_RESOURCE_BARRIER b = {};
	b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource   = m_texture.Get();
	b.Transition.StateBefore = m_state;
	b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd->ResourceBarrier(1, &b);

	m_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}
