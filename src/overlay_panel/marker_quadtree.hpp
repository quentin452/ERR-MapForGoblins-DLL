#pragma once
// Spatial index for the Virtual World Map markers (world XZ). Replaces the O(n)-every-frame draw loop:
// - VIEWPORT CULL: a query returns only the markers whose node intersects the visible rect, so a
//   zoomed-in map pays for the handful on screen, not all ~6837.
// - LOD CLUSTERING: any node whose on-screen size falls below a pixel threshold collapses to ONE pile
//   (centroid + subtree count), so a zoomed-out map draws a few hundred "×N" piles, not 6837 glyphs.
// Both fall out of one traversal. Built once per marker set (rebuilt on group/world change / refresh);
// the query is const (no realloc) so it's cheap and thread-obvious (present thread only).
//
// Index-based (no Node* held across a push_back → realloc-safe). Header-only; vmap-local for now, but
// the shape is reusable by the native map's clustering later.

#include "worldmap/marker_layer.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace goblin::overlay::panel
{
class MarkerQuadtree
{
public:
    using Marker = goblin::worldmap::Marker;
    // node = the collapsing subtree's root index → gather_pile() can enumerate its members on demand (for
    // hover spiderfy). -1 for piles not produced by collect (none currently).
    struct Pile { float cx, cz; int count; int node = -1; };

    // Rebuild over `items` (marker pointers must outlive the tree — they point into the layers).
    void build(const std::vector<const Marker *> &items, int capacity = 8, int max_depth = 8)
    {
        m_nodes.clear();
        m_cap = capacity;
        m_maxDepth = max_depth;
        if (items.empty()) return;
        float minx = 1e30f, minz = 1e30f, maxx = -1e30f, maxz = -1e30f;
        for (const Marker *m : items)
        {
            minx = (std::min)(minx, m->worldX); maxx = (std::max)(maxx, m->worldX);
            minz = (std::min)(minz, m->worldZ); maxz = (std::max)(maxz, m->worldZ);
        }
        const float pad = (std::max)(1.0f, (maxx - minx + maxz - minz) * 0.01f);
        m_nodes.push_back(Node{minx - pad, minz - pad, maxx + pad, maxz + pad});
        for (const Marker *m : items) place(0, m, 0);
    }

    bool empty() const { return m_nodes.empty(); }

    // Root bounds = the marker bbox (for Fit). Returns false if empty.
    bool bounds(float &minx, float &minz, float &maxx, float &maxz) const
    {
        if (m_nodes.empty()) return false;
        const Node &r = m_nodes[0];
        minx = r.minx; minz = r.minz; maxx = r.maxx; maxz = r.maxz;
        return true;
    }

    // Query the visible world rect. Nodes smaller than clusterWorld collapse into a Pile; sparse leaf
    // markers in view are appended to `singles`. clusterWorld = min-pile-px / zoom (px per world unit).
    void query(float vMinX, float vMinZ, float vMaxX, float vMaxZ, float clusterWorld,
               std::vector<Pile> &piles, std::vector<const Marker *> &singles) const
    {
        if (!m_nodes.empty())
            collect(0, vMinX, vMinZ, vMaxX, vMaxZ, clusterWorld, piles, singles);
    }

    // Enumerate a pile's member markers (subtree walk from pl.node). On-demand — call only for the ONE
    // hovered pile so the per-frame cluster draw stays allocation-free. Appends to `out`.
    void gather_pile(const Pile &pl, std::vector<const Marker *> &out) const
    {
        if (pl.node >= 0 && pl.node < (int)m_nodes.size())
            gather(pl.node, out);
    }

private:
    struct Node
    {
        float minx, minz, maxx, maxz;
        int firstChild = -1;                 // index of the first of 4 children; -1 = leaf
        int count = 0;                        // subtree marker total
        double sumx = 0.0, sumz = 0.0;        // subtree centroid accumulation
        std::vector<const Marker *> items;    // leaf contents
    };

    std::vector<Node> m_nodes;
    int m_cap = 8, m_maxDepth = 8;

    int quadrant(int ni, float x, float z) const
    {
        const Node &n = m_nodes[ni];
        const float mx = (n.minx + n.maxx) * 0.5f, mz = (n.minz + n.maxz) * 0.5f;
        return (x >= mx ? 1 : 0) + (z >= mz ? 2 : 0);   // 0 SW,1 SE,2 NW,3 NE
    }

    // Insert m into subtree ni (updates count/centroid). Re-fetches nodes[] after any push_back.
    void place(int ni, const Marker *m, int depth)
    {
        {
            Node &n = m_nodes[ni];
            n.count++; n.sumx += m->worldX; n.sumz += m->worldZ;
            if (n.firstChild >= 0)
            {
                const int c = n.firstChild + quadrant(ni, m->worldX, m->worldZ);
                place(c, m, depth + 1);
                return;
            }
            n.items.push_back(m);
            if ((int)n.items.size() <= m_cap || depth >= m_maxDepth)
                return;
        }
        subdivide(ni, depth);   // node reference dropped before this (subdivide reallocs m_nodes)
    }

    void subdivide(int ni, int depth)
    {
        float minx, minz, maxx, maxz;
        {
            const Node &n = m_nodes[ni];
            minx = n.minx; minz = n.minz; maxx = n.maxx; maxz = n.maxz;
        }
        const float mx = (minx + maxx) * 0.5f, mz = (minz + maxz) * 0.5f;
        const int base = (int)m_nodes.size();
        m_nodes.push_back(Node{minx, minz, mx, mz});   // 0 SW
        m_nodes.push_back(Node{mx, minz, maxx, mz});   // 1 SE
        m_nodes.push_back(Node{minx, mz, mx, maxz});   // 2 NW
        m_nodes.push_back(Node{mx, mz, maxx, maxz});   // 3 NE
        std::vector<const Marker *> moved;
        {
            Node &n = m_nodes[ni];
            moved.swap(n.items);
            n.firstChild = base;
        }
        // Re-distribute existing items into the fresh children (their count starts at 0, so this
        // rebuilds the children's subtree stats correctly; the parent count already includes them).
        for (const Marker *m : moved)
            place(base + quadrant(ni, m->worldX, m->worldZ), m, depth + 1);
    }

    // Concatenate every marker in the subtree rooted at `ni` (leaves hold the items).
    void gather(int ni, std::vector<const Marker *> &out) const
    {
        const Node &n = m_nodes[ni];
        if (n.firstChild < 0)
        {
            out.insert(out.end(), n.items.begin(), n.items.end());
            return;
        }
        for (int i = 0; i < 4; ++i) gather(n.firstChild + i, out);
    }

    void collect(int ni, float vMinX, float vMinZ, float vMaxX, float vMaxZ, float clusterWorld,
                 std::vector<Pile> &piles, std::vector<const Marker *> &singles) const
    {
        const Node &n = m_nodes[ni];
        if (n.count == 0) return;
        if (n.maxx < vMinX || n.minx > vMaxX || n.maxz < vMinZ || n.minz > vMaxZ) return;   // cull
        const float sz = (std::max)(n.maxx - n.minx, n.maxz - n.minz);
        if (n.count > 1 && sz <= clusterWorld)   // node too small on screen → one pile
        {
            piles.push_back(Pile{(float)(n.sumx / n.count), (float)(n.sumz / n.count), n.count, ni});
            return;
        }
        if (n.firstChild < 0)                    // leaf → emit the in-view markers individually
        {
            for (const Marker *m : n.items)
                if (m->worldX >= vMinX && m->worldX <= vMaxX && m->worldZ >= vMinZ && m->worldZ <= vMaxZ)
                    singles.push_back(m);
            return;
        }
        for (int i = 0; i < 4; ++i)
            collect(n.firstChild + i, vMinX, vMinZ, vMaxX, vMaxZ, clusterWorld, piles, singles);
    }
};
} // namespace goblin::overlay::panel
