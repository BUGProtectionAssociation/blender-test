
/**
 * Operations to move virtual shadow map pages between heaps and tiles.
 * We reuse the blender::vector class denomination.
 *
 * The needed resources for this lib are:
 * - tilemap_img
 * - pages_free_buf
 * - pages_cached_buf
 * - pages_infos_buf
 *
 * A page is can be in 3 state (free, cached, acquired). Each one correspond to a different owner.
 *
 * - The pages_free_buf works in a regular stack containing only the page coordinates.
 *
 * - The pages_cached_buf is a ring buffer where newly cached pages gets added at the end and the
 *   old cached pages gets defragmented at the start of the used portion.
 *
 * - The tilemap_img only owns a page if it is used. If the page is cached, the tile contains a
 *   reference index inside the pages_cached_buf.
 *
 * IMPORTANT: Do not forget to manually store the tile data after doing operations on them.
 */

#pragma BLENDER_REQUIRE(eevee_shadow_tilemap_lib.glsl)

/* TODO(@fclem): Implement. */
#define assert(check)

/* Remove page ownership from the tile cache and append it to the cache. */
void shadow_page_free_buf_append(inout ShadowTileData tile)
{
  assert(tile.is_allocated);

  int index = atomicAdd(pages_infos_buf.page_free_count, 1);
  assert(index < SHADOW_MAX_PAGE);
  /* Insert in heap. */
  pages_free_buf[index] = packUvec2x16(tile.page);
  /* Remove from tile. */
  tile.page = uvec2(-1);
  tile.is_cached = false;
  tile.is_allocated = false;
}

/* Remove last page from the free heap and give ownership to the tile. */
void shadow_page_free_buf_pop_last(inout ShadowTileData tile)
{
  assert(!tile.is_allocated);

  int index = atomicAdd(pages_infos_buf.page_free_count, -1) - 1;
  /* This can easily happen in really big scene. */
  if (index < 0) {
    return;
  }
  /* Insert in tile. */
  tile.page = unpackUvec2x16(pages_free_buf[index]);
  tile.is_allocated = true;
  tile.do_update = true;
  /* Remove from heap. */
  pages_free_buf[index] = uint(-1);
}

/* Remove page ownership from the tile cache and append it to the cache. */
void shadow_page_cached_buf_append(inout ShadowTileData tile, ivec2 tile_co)
{
  assert(tile.is_allocated);

  /* The page_cached_next is also wrapped in the defrag phase to avoid unsigned overflow. */
  uint index = atomicAdd(pages_infos_buf.page_cached_next, 1u) % uint(SHADOW_MAX_PAGE);
  /* Insert in heap. */
  pages_cached_buf[index] = uvec2(packUvec2x16(tile.page), packUvec2x16(tile_co));
  /* Remove from tile. */
  tile.page = uvec2(-1);
  tile.cache_index = index;
  tile.is_cached = true;
  tile.is_allocated = false;
}

/* Remove page from cache and give ownership to the tile. */
void shadow_page_cached_buf_remove(inout ShadowTileData tile)
{
  assert(!tile.is_allocated);
  assert(tile.is_cached);

  uint index = tile.cache_index;
  /* Insert in tile. */
  tile.page = unpackUvec2x16(pages_cached_buf[index].x);
  tile.cache_index = uint(-1);
  tile.is_cached = false;
  tile.is_allocated = true;
  /* Remove from heap. Leaves hole in the buffer. This is handled by the defrag phase. */
  pages_cached_buf[index] = uvec2(-1);
}

void shadow_page_cache_update_tile_ref(ShadowTileData tile, ivec2 new_tile_co)
{
  assert(!tile.is_allocated);
  assert(tile.is_cached);

  pages_cached_buf[tile.cache_index].y = packUvec2x16(new_tile_co);
}

void shadow_page_cache_update_page_ref(uint page_index, uint new_page_index)
{
  assert(!tile.is_allocated);
  assert(tile.is_cached);

  ivec2 texel = ivec2(unpackUvec2x16(pages_cached_buf[page_index].y));

  ShadowTileData tile = shadow_tile_data_unpack(imageLoad(tilemaps_img, texel).x);
  tile.cache_index = new_page_index;
  imageStore(tilemaps_img, texel, uvec4(shadow_tile_data_pack(tile)));
}
