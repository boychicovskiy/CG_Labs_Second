#ifndef FRUSTUM_HPP
#define FRUSTUM_HPP

#include <DirectXMath.h>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// Ограничивающий объём — выровненный по осям параллелепипед.
// Используется и для объектов сцены, и для узлов окто-дерева, поэтому один
// и тот же тест frustum-vs-AABB работает на обоих уровнях иерархии.
// ─────────────────────────────────────────────────────────────────────────────
struct AABB {
	DirectX::XMFLOAT3 Min = { 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 Max = { 0.0f, 0.0f, 0.0f };

	DirectX::XMFLOAT3 Center() const {
		return { 0.5f * (Min.x + Max.x), 0.5f * (Min.y + Max.y), 0.5f * (Min.z + Max.z) };
	}

	DirectX::XMFLOAT3 Extent() const {
		return { 0.5f * (Max.x - Min.x), 0.5f * (Max.y - Min.y), 0.5f * (Max.z - Min.z) };
	}

	bool Contains(const AABB& other) const {
		return other.Min.x >= Min.x && other.Max.x <= Max.x
		    && other.Min.y >= Min.y && other.Max.y <= Max.y
		    && other.Min.z >= Min.z && other.Max.z <= Max.z;
	}
};

// Результат теста. Отдельное значение Inside важно для окто-дерева: если узел
// целиком внутри пирамиды, все его потомки видимы и их можно не проверять.
enum class Containment {
	Outside = 0,
	Intersects,
	Inside
};

// ─────────────────────────────────────────────────────────────────────────────
// Пирамида видимости: шесть плоскостей, извлечённых прямо из матрицы ViewProj
// (метод Gribb–Hartmann). Никаких обратных матриц и восстановления углов
// камеры — плоскости получаются сложением/вычитанием столбцов.
//
// Соглашение DirectXMath — вектор-строка: clip = p * M, поэтому clip.x
// собирается из НУЛЕВОГО СТОЛБЦА матрицы, clip.w — из третьего.
// Условие попадания в пирамиду: -w <= x,y <= w и 0 <= z <= w.
//   левая   плоскость:  clip.x + clip.w >= 0  ->  столбец0 + столбец3
//   правая:             clip.w - clip.x >= 0  ->  столбец3 - столбец0
//   ближняя:            clip.z          >= 0  ->  столбец2
// ─────────────────────────────────────────────────────────────────────────────
struct Frustum {
	// xyz — нормаль плоскости, w — свободный член. Внутри: dot(n, p) + w >= 0
	DirectX::XMFLOAT4 Planes[6] = {};

	void ExtractFromViewProj(const DirectX::XMMATRIX& viewProj)
	{
		DirectX::XMFLOAT4X4 m;
		DirectX::XMStoreFloat4x4(&m, viewProj);   // без транспонирования!

		// left  = col3 + col0
		Planes[0] = { m.m[0][3] + m.m[0][0], m.m[1][3] + m.m[1][0],
		              m.m[2][3] + m.m[2][0], m.m[3][3] + m.m[3][0] };
		// right = col3 - col0
		Planes[1] = { m.m[0][3] - m.m[0][0], m.m[1][3] - m.m[1][0],
		              m.m[2][3] - m.m[2][0], m.m[3][3] - m.m[3][0] };
		// bottom = col3 + col1
		Planes[2] = { m.m[0][3] + m.m[0][1], m.m[1][3] + m.m[1][1],
		              m.m[2][3] + m.m[2][1], m.m[3][3] + m.m[3][1] };
		// top    = col3 - col1
		Planes[3] = { m.m[0][3] - m.m[0][1], m.m[1][3] - m.m[1][1],
		              m.m[2][3] - m.m[2][1], m.m[3][3] - m.m[3][1] };
		// near   = col2   (в DirectX z из [0..w], а не [-w..w] как в OpenGL)
		Planes[4] = { m.m[0][2], m.m[1][2], m.m[2][2], m.m[3][2] };
		// far    = col3 - col2
		Planes[5] = { m.m[0][3] - m.m[0][2], m.m[1][3] - m.m[1][2],
		              m.m[2][3] - m.m[2][2], m.m[3][3] - m.m[3][2] };

		// Нормируем: иначе «расстояние» до плоскости в единицах, зависящих
		// от матрицы, и сравнение с радиусом объекта теряет смысл.
		for (int i = 0; i < 6; ++i)
		{
			DirectX::XMFLOAT4& p = Planes[i];
			const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
			if (len > 1e-8f)
			{
				p.x /= len; p.y /= len; p.z /= len; p.w /= len;
			}
		}
	}

	// Консервативный тест AABB против шести плоскостей.
	//
	// Для каждой плоскости считается проекция полуразмера коробки на нормаль:
	//   r = |e.x*n.x| + |e.y*n.y| + |e.z*n.z|
	// Это расстояние от центра до самой «дальней» вершины вдоль нормали, то
	// есть не нужно перебирать все восемь вершин.
	//   d + r < 0  -> коробка целиком по отрицательную сторону -> Outside
	//   d - r < 0  -> плоскость режет коробку                  -> Intersects
	Containment TestAABB(const AABB& box) const
	{
		const DirectX::XMFLOAT3 c = box.Center();
		const DirectX::XMFLOAT3 e = box.Extent();

		bool intersects = false;

		for (int i = 0; i < 6; ++i)
		{
			const DirectX::XMFLOAT4& p = Planes[i];

			const float d = p.x * c.x + p.y * c.y + p.z * c.z + p.w;
			const float r = std::fabs(e.x * p.x) + std::fabs(e.y * p.y) + std::fabs(e.z * p.z);

			if (d + r < 0.0f) return Containment::Outside;
			if (d - r < 0.0f) intersects = true;
		}

		return intersects ? Containment::Intersects : Containment::Inside;
	}
};

#endif // !FRUSTUM_HPP
