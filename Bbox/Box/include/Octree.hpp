#ifndef OCTREE_HPP
#define OCTREE_HPP

#include <cstdint>
#include <vector>

#include "Frustum.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Окто-дерево над ограничивающими объёмами объектов сцены (ДЗ №4, лекция 05).
//
// Смысл структуры: при переборе «в лоб» frustum culling стоит O(N) тестов
// на кадр. Дерево позволяет отсечь целое поддерево одним тестом узла:
//   узел Outside  -> ни один объект внутри не виден, обход прекращается
//   узел Inside   -> ВСЕ объекты поддерева видимы, дальше не проверяем вообще
//   узел Intersect-> проверяем объекты узла поштучно и спускаемся к детям
//
// Правило размещения: объект кладётся в самый глубокий узел, чей объём
// ПОЛНОСТЬЮ его содержит. Объект, пересекающий границу деления, остаётся
// в родителе. Так границы узлов не нужно расширять, и тест узла остаётся
// корректным для всего поддерева.
// ─────────────────────────────────────────────────────────────────────────────
class Octree {
public:
	// objectBounds — ограничивающие коробки объектов; индекс = идентификатор
	// объекта, который вернёт Query().
	void Build(const std::vector<AABB>& objectBounds,
	           int maxDepth = 8,
	           size_t maxObjectsPerNode = 8);

	// Собирает индексы потенциально видимых объектов.
	// aabbTests — счётчик выполненных тестов (для сравнения с перебором).
	void Query(const Frustum& frustum,
	           std::vector<uint32_t>& outVisible,
	           uint32_t& aabbTests) const;

	// Коробки узлов для отладочной отрисовки
	void CollectNodeBoxes(std::vector<AABB>& out) const;

	size_t NodeCount()  const { return m_nodes.size(); }
	int    MaxDepth()   const { return m_reachedDepth; }
	bool   IsBuilt()    const { return !m_nodes.empty(); }

private:
	struct Node {
		AABB   bounds;
		int    children[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
		bool   leaf = true;
		std::vector<uint32_t> objects;   // объекты, лежащие ровно в этом узле
	};

	int  BuildNode(const AABB& bounds, std::vector<uint32_t>& objects, int depth);

	void QueryNode(int nodeIndex, const Frustum& frustum,
	               std::vector<uint32_t>& outVisible, uint32_t& aabbTests) const;

	// Все объекты поддерева без единого теста — узел уже признан Inside
	void CollectSubtree(int nodeIndex, std::vector<uint32_t>& outVisible) const;

	std::vector<Node> m_nodes;
	std::vector<AABB> m_objectBounds;

	int    m_maxDepth          = 8;
	size_t m_maxObjectsPerNode = 8;
	int    m_reachedDepth      = 0;
};

#endif // !OCTREE_HPP
