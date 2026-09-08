// Windows.h min/max macros collide with std::min / std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Octree.hpp"

#include <algorithm>

namespace {

	// Восемь дочерних объёмов: бит 0 — ось X, бит 1 — Y, бит 2 — Z.
	AABB ChildBounds(const AABB& b, int index)
	{
		const DirectX::XMFLOAT3 c = b.Center();

		AABB out;
		out.Min.x = (index & 1) ? c.x : b.Min.x;
		out.Max.x = (index & 1) ? b.Max.x : c.x;
		out.Min.y = (index & 2) ? c.y : b.Min.y;
		out.Max.y = (index & 2) ? b.Max.y : c.y;
		out.Min.z = (index & 4) ? c.z : b.Min.z;
		out.Max.z = (index & 4) ? b.Max.z : c.z;

		return out;
	}

} // namespace

void Octree::Build(const std::vector<AABB>& objectBounds, int maxDepth, size_t maxObjectsPerNode)
{
	m_nodes.clear();
	m_objectBounds = objectBounds;
	m_maxDepth = maxDepth;
	m_maxObjectsPerNode = maxObjectsPerNode;
	m_reachedDepth = 0;

	if (m_objectBounds.empty())
		return;

	// Корневой объём — объединение всех коробок объектов
	AABB root = m_objectBounds[0];
	for (const AABB& b : m_objectBounds)
	{
		root.Min.x = std::min(root.Min.x, b.Min.x);
		root.Min.y = std::min(root.Min.y, b.Min.y);
		root.Min.z = std::min(root.Min.z, b.Min.z);
		root.Max.x = std::max(root.Max.x, b.Max.x);
		root.Max.y = std::max(root.Max.y, b.Max.y);
		root.Max.z = std::max(root.Max.z, b.Max.z);
	}

	// Небольшой запас, чтобы объекты на самой границе гарантированно попадали
	// внутрь корня (Contains использует нестрогие сравнения).
	const float pad = 1e-4f;
	root.Min.x -= pad; root.Min.y -= pad; root.Min.z -= pad;
	root.Max.x += pad; root.Max.y += pad; root.Max.z += pad;

	std::vector<uint32_t> all(m_objectBounds.size());
	for (uint32_t i = 0; i < m_objectBounds.size(); ++i)
		all[i] = i;

	BuildNode(root, all, 0);
}

int Octree::BuildNode(const AABB& bounds, std::vector<uint32_t>& objects, int depth)
{
	const int index = static_cast<int>(m_nodes.size());
	m_nodes.emplace_back();
	m_nodes[index].bounds = bounds;

	m_reachedDepth = std::max(m_reachedDepth, depth);

	// Дальше дробить незачем: либо достигли предела глубины, либо объектов мало
	if (depth >= m_maxDepth || objects.size() <= m_maxObjectsPerNode)
	{
		m_nodes[index].objects = std::move(objects);
		m_nodes[index].leaf = true;
		return index;
	}

	// Раскидываем объекты по восьми потомкам. Объект, который не помещается
	// целиком ни в одного потомка, остаётся в текущем узле.
	std::vector<uint32_t> childLists[8];
	std::vector<uint32_t> stay;

	AABB childBoxes[8];
	for (int i = 0; i < 8; ++i)
		childBoxes[i] = ChildBounds(bounds, i);

	for (uint32_t objIndex : objects)
	{
		const AABB& ob = m_objectBounds[objIndex];

		int target = -1;
		for (int i = 0; i < 8; ++i)
		{
			if (childBoxes[i].Contains(ob))
			{
				target = i;
				break;
			}
		}

		if (target >= 0) childLists[target].push_back(objIndex);
		else             stay.push_back(objIndex);
	}

	// Если ничего не удалось спустить вниз — деление бессмысленно, делаем лист.
	bool anyChild = false;
	for (int i = 0; i < 8; ++i)
		anyChild = anyChild || !childLists[i].empty();

	if (!anyChild)
	{
		m_nodes[index].objects = std::move(objects);
		m_nodes[index].leaf = true;
		return index;
	}

	m_nodes[index].objects = std::move(stay);
	m_nodes[index].leaf = false;

	for (int i = 0; i < 8; ++i)
	{
		if (childLists[i].empty())
			continue;

		const int child = BuildNode(childBoxes[i], childLists[i], depth + 1);

		// ВАЖНО: ссылку на m_nodes[index] брать нельзя — вектор мог
		// перевыделиться внутри рекурсивного вызова.
		m_nodes[index].children[i] = child;
	}

	return index;
}

void Octree::Query(const Frustum& frustum, std::vector<uint32_t>& outVisible, uint32_t& aabbTests) const
{
	outVisible.clear();
	aabbTests = 0;

	if (m_nodes.empty())
		return;

	QueryNode(0, frustum, outVisible, aabbTests);
}

void Octree::QueryNode(int nodeIndex, const Frustum& frustum,
                       std::vector<uint32_t>& outVisible, uint32_t& aabbTests) const
{
	const Node& node = m_nodes[nodeIndex];

	++aabbTests;
	const Containment result = frustum.TestAABB(node.bounds);

	// Весь узел за пределами пирамиды — поддерево не обходим вообще.
	if (result == Containment::Outside)
		return;

	// Узел целиком внутри — все объекты поддерева видимы, тесты не нужны.
	// Это и есть основной выигрыш дерева над перебором.
	if (result == Containment::Inside)
	{
		CollectSubtree(nodeIndex, outVisible);
		return;
	}

	// Узел пересекает границу пирамиды — объекты проверяем поштучно
	for (uint32_t objIndex : node.objects)
	{
		++aabbTests;
		if (frustum.TestAABB(m_objectBounds[objIndex]) != Containment::Outside)
			outVisible.push_back(objIndex);
	}

	for (int i = 0; i < 8; ++i)
	{
		if (node.children[i] >= 0)
			QueryNode(node.children[i], frustum, outVisible, aabbTests);
	}
}

void Octree::CollectSubtree(int nodeIndex, std::vector<uint32_t>& outVisible) const
{
	const Node& node = m_nodes[nodeIndex];

	outVisible.insert(outVisible.end(), node.objects.begin(), node.objects.end());

	for (int i = 0; i < 8; ++i)
	{
		if (node.children[i] >= 0)
			CollectSubtree(node.children[i], outVisible);
	}
}

void Octree::CollectNodeBoxes(std::vector<AABB>& out) const
{
	out.clear();
	out.reserve(m_nodes.size());

	for (const Node& n : m_nodes)
		out.push_back(n.bounds);
}
