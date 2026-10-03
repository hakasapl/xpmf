#pragma once

#include "PCH.h"

#include <cstdint>

namespace XPMF::Offsets {

/**
 * @brief BSGraphics::TriShape* CreateTriShape(Renderer*, const void* vertexData, uint32_t sizeBytes,
 *          uint64_t vertexDesc, ID3D11Buffer** indexBuffer)
 *
 * Allocates the 48 byte renderer data through the game's MemoryManager, copies the vertex data
 * into a CPU side rawVertexData (same allocator; the caller keeps its own buffer), creates the
 * D3D11 vertex buffer from it, stores the descriptor, starts the reference count at 1, and
 * takes the index buffer by reading one ID3D11Buffer* through the last argument and AddRef-ing
 * it, with no null check (a crash report of 2026-10-02: a DynDOLOD LOD shape without one) - so
 * the address of another TriShape's indexBuffer member shares that shape's indices.
 * rawIndexData is left null. Same IDs SmoothTerrain builds its land meshes with.
 * 1.5.97: 0xD6BD10. 1.6.1170: 0xE46170. 1.7.99: 0x100B710.
 */
constexpr REL::RelocationID K_CREATE_TRISHAPE_DATA(75475,
                                                   77261);

/**
 * @brief Signature of the function K_CREATE_TRISHAPE_DATA points at
 */
using CreateTriShapeData_t = RE::BSGraphics::TriShape*(RE::BSGraphics::Renderer* renderer,
                                                       const void* vertexData,
                                                       std::uint32_t sizeBytes,
                                                       std::uint64_t vertexDesc,
                                                       REX::W32::ID3D11Buffer** indexBuffer);

/**
 * @brief Global pointer to the geometry buffer manager
 *
 * BSTriShape::~BSTriShape releases its renderer data through this manager's vfunc 5, which
 * lands in the renderer's release (1.7.99: 0x100BDB0): an interlocked decrement of refCount
 * and, at zero, Release() on both D3D buffers, MemoryManager::Deallocate on rawVertexData and
 * rawIndexData, and the struct itself freed. Hence anything attached to a renderer data has
 * to come from RE::malloc. 1.5.97: 0x30136C0. 1.6.1170: 0x3274040. 1.7.99: 0x331D7C0.
 */
constexpr REL::RelocationID K_GEOMETRY_BUFFER_MANAGER(523950,
                                                      410530);

/**
 * @brief Signature of vfunc 5 (vtable offset 0x28) of the manager K_GEOMETRY_BUFFER_MANAGER points at
 */
using ReleaseRendererData_t = void (*)(void* manager,
                                       RE::BSGraphics::TriShape* rendererData);

} // namespace XPMF::Offsets
