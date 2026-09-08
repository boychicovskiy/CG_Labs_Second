#ifndef RENDER_STRUCTS_HPP
#define RENDER_STRUCTS_HPP

#include <DirectXMath.h>
#include <cstdint>

namespace dx {
	inline DirectX::XMFLOAT4X4 Identity4x4() {
		DirectX::XMFLOAT4X4 m;
		DirectX::XMStoreFloat4x4(&m, DirectX::XMMatrixIdentity());
		return m;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Вершина. Stride остался 48 байт: неиспользуемый Color (ДЗ №1) заменён на
// касательную для normal mapping (ДЗ №3). w хранит хиральность базиса TBN.
// ─────────────────────────────────────────────────────────────────────────────
struct Vertex {
	DirectX::XMFLOAT3 Pos;                        // offset  0
	DirectX::XMFLOAT3 Normal;                     // offset 12
	DirectX::XMFLOAT4 TangentU;                   // offset 24  (xyz = T, w = ±1)
	DirectX::XMFLOAT2 TexCoord = { 0.0f, 0.0f };  // offset 40
};

// ─────────────────────────────────────────────────────────────────────────────
// GEOMETRY PASS — константные буферы
// ─────────────────────────────────────────────────────────────────────────────

// b0: одна на всю модель
struct alignas(16) ObjectConstants {
	DirectX::XMFLOAT4X4 World             = dx::Identity4x4();
	DirectX::XMFLOAT4X4 WorldInvTranspose = dx::Identity4x4();
};

// b1: одна на кадр. Кроме матрицы содержит параметры тесселяции (ДЗ №3):
// hull shader считает по ним коэффициенты, domain shader — величину смещения.
struct alignas(16) GeoPassConstants {
	DirectX::XMFLOAT4X4 ViewProj = dx::Identity4x4();

	DirectX::XMFLOAT3 EyePosW        = { 0.0f, 0.0f, 0.0f };
	float             TessFactorMax  = 8.0f;     // вблизи

	float             TessFactorMin  = 1.0f;     // вдали
	float             TessDistNear   = 0.25f;    // ближе — максимальный фактор
	float             TessDistFar    = 3.00f;    // дальше — минимальный
	float             DisplacementScale = 0.008f;

	uint32_t          NormalMapEnabled = 1;
	uint32_t          FlipGreenChannel = 0;      // на случай другого соглашения карты
	uint32_t          BackfaceCullHS   = 0;      // отбраковка патчей в hull shader
	uint32_t          _pad0            = 0;
};

// b2: одна на подмеш (материал). Ровно 64 байта.
struct alignas(16) MaterialConstants {
	DirectX::XMFLOAT4 DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };   // Kd из .mtl

	DirectX::XMFLOAT3 SpecularColor = { 0.1f, 0.1f, 0.1f };         // Ks из .mtl
	float             SpecPower     = 32.0f;                        // Ns из .mtl

	DirectX::XMFLOAT2 UvScale       = { 1.0f, 1.0f };               // тайлинг (ДЗ №1)
	DirectX::XMFLOAT2 UvOffset      = { 0.0f, 0.0f };               // UV-анимация (ДЗ №1)

	uint32_t          AlphaTest       = 0;    // есть ли map_d
	uint32_t          HasNormalMap    = 0;    // есть ли *_nrm.dds
	uint32_t          HasDisplacement = 0;    // есть ли map_bump (карта высот)
	float             DispScale       = 1.0f; // множитель к глобальной силе смещения
};

// ─────────────────────────────────────────────────────────────────────────────
// LIGHT PASS — константные буферы
// ─────────────────────────────────────────────────────────────────────────────

enum class LightType : uint32_t {
	Directional = 0,
	Point       = 1,
	Spot        = 2,
	Ambient     = 3   // отдельный "источник" ambient — см. слайд 13 лекции 03
};

// b1: одна на источник света. Ровно 64 байта.
struct alignas(16) LightConstants {
	DirectX::XMFLOAT3 Color      = { 1.0f, 1.0f, 1.0f };
	float             Intensity  = 1.0f;

	DirectX::XMFLOAT3 PositionW  = { 0.0f, 0.0f, 0.0f };   // Point / Spot
	float             Range      = 1.0f;                   // Point / Spot

	DirectX::XMFLOAT3 DirectionW = { 0.0f, -1.0f, 0.0f };  // Directional / Spot
	float             SpotCosOuter = 0.70f;                // cos внешнего угла

	uint32_t          Type       = 0;
	float             SpotCosInner = 0.90f;                // cos внутреннего угла
	float             _pad0 = 0.0f, _pad1 = 0.0f;
};

// b0: одна на кадр
struct alignas(16) LightPassConstants {
	DirectX::XMFLOAT4X4 InvViewProj = dx::Identity4x4();   // для реконструкции posW из глубины

	DirectX::XMFLOAT3 EyePosW = { 0.0f, 0.0f, 0.0f };
	float             _pad0   = 0.0f;

	DirectX::XMFLOAT2 InvScreenSize = { 0.0f, 0.0f };      // 1/width, 1/height
	uint32_t          DebugMode     = 0;                   // 0=off, 1..4 = показать таргет G-Buffer
	float             _pad1         = 0.0f;

	DirectX::XMFLOAT4 AmbientColor = { 0.12f, 0.12f, 0.14f, 1.0f };
};

static_assert(sizeof(Vertex)              == 48,     "Vertex stride must stay 48 bytes.");
static_assert(sizeof(ObjectConstants)     % 16 == 0, "ObjectConstants must be 16-byte aligned.");
static_assert(sizeof(GeoPassConstants)    == 112,    "GeoPassConstants must match HLSL layout.");
static_assert(sizeof(MaterialConstants)   == 64,     "MaterialConstants must match HLSL layout.");
static_assert(sizeof(LightConstants)      == 64,     "LightConstants must match HLSL layout.");
static_assert(sizeof(LightPassConstants)  % 16 == 0, "LightPassConstants must be 16-byte aligned.");

#endif // !RENDER_STRUCTS_HPP
