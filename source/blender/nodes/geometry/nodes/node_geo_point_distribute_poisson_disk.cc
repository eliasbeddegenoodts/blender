/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/*
 * Based on Cem Yuksel. 2015. Sample Elimination for Generating Poisson Disk Sample
 * ! Sets. Computer Graphics Forum 34, 2 (May 2015), 25-32.
 * ! http://www.cemyuksel.com/research/sampleelimination/
 * Copyright (c) 2016, Cem Yuksel <cem@cemyuksel.com>
 * All rights reserved.
 */
#define CYHEAP 1

#include "BLI_kdtree.h"

#ifndef CYHEAP
#  include "BLI_heap.h"
#else
#  include "cyHeap.h"
#endif

#include "node_geometry_util.hh"

#include <iostream>

namespace blender::nodes {

static void tile_point(Vector<float3> *tiled_points,
                       Vector<size_t> *indices,
                       const float maximum_distance,
                       const float3 boundbox,
                       float3 const &point,
                       size_t index,
                       int dim = 0)
{
  for (int d = dim; d < 3; d++) {
    if (boundbox[d] - point[d] < maximum_distance) {
      float3 p = point;
      p[d] -= boundbox[d];

      tiled_points->append(p);
      indices->append(index);

      tile_point(tiled_points, indices, maximum_distance, boundbox, p, index, d + 1);
    }

    if (point[d] < maximum_distance) {
      float3 p = point;
      p[d] += boundbox[d];

      tiled_points->append(p);
      indices->append(index);

      tile_point(tiled_points, indices, maximum_distance, boundbox, p, index, d + 1);
    }
  }
}

/**
 * Returns the weight the point gets based on the distance to another point.
 */
static float point_weight_influence_get(const float maximum_distance,
                                        const float minimum_distance,
                                        float distance)
{
  const float alpha = 8.0f;

  if (distance < minimum_distance) {
    distance = minimum_distance;
  }

  return std::pow(1.0f - distance / maximum_distance, alpha);
}

/**
 * Weight each point based on their proximity to its neighbors
 *
 * For each index in the weight array add a weight based on the proximity the
 * corresponding point has with its neighboors.
 **/
static void points_distance_weight_calculate(Vector<float> *weights,
                                             const size_t point_id,
                                             const float3 *input_points,
                                             const void *kd_tree,
                                             const float minimum_distance,
                                             const float maximum_distance,
#ifndef CYHEAP
                                             Heap *heap,
                                             Vector<HeapNode *> *nodes)
#else
                                             cy::Heap *heap,
                                             void *UNUSED(nodes))
#endif
{
  KDTreeNearest_3d *nearest_point = nullptr;
  int neighbors = BLI_kdtree_3d_range_search(
      (KDTree_3d *)kd_tree, input_points[point_id], &nearest_point, maximum_distance);

  for (int i = 0; i < neighbors; i++) {
    size_t neighbor_point_id = nearest_point[i].index;

    if (neighbor_point_id >= weights->size()) {
      continue;
    }

    /* The point should not influence itself. */
    if (neighbor_point_id == point_id) {
      continue;
    }

    const float weight_influence = point_weight_influence_get(
        maximum_distance, minimum_distance, nearest_point[i].dist);

    /* In the first pass we just the weights. */
    if (heap == nullptr) {
      (*weights)[point_id] += weight_influence;
    }
    /* When we run again we need to update the weights and the heap. */
    else {
      (*weights)[neighbor_point_id] -= weight_influence;
#ifndef CYHEAP
      HeapNode *node = (*nodes)[neighbor_point_id];
      if (node != nullptr) {
        BLI_heap_node_value_update(heap, node, -((*weights)[neighbor_point_id]));
      }
#else
      heap->MoveItemDown(neighbor_point_id);
#endif
    }
  }

  if (nearest_point) {
    MEM_freeN(nearest_point);
  }
}

/**
 * Returns the minimum radius fraction used by the default weight function.
 */
static float weight_limit_fraction_get(const size_t input_size, const size_t output_size)
{
  const float beta = 0.65f;
  const float gamma = 1.5f;
  float ratio = float(output_size) / float(input_size);
  return (1.0f - std::pow(ratio, gamma)) * beta;
}

/**
 * Tile the input points.
 */
static void points_tiling(const float3 *input_points,
                          const size_t input_size,
                          void **kd_tree,
                          const float maximum_distance,
                          const float3 boundbox)

{
  Vector<float3> tiled_points(input_points, input_points + input_size);
  Vector<size_t> indices(input_size);

  /* Start building a kdtree for the samples. */
  for (size_t i = 0; i < input_size; i++) {
    indices[i] = i;
    BLI_kdtree_3d_insert(*(KDTree_3d **)kd_tree, i, input_points[i]);
  }
  BLI_kdtree_3d_balance(*(KDTree_3d **)kd_tree);

  /* Tile the tree based on the boundbox. */
  for (size_t i = 0; i < input_size; i++) {
    tile_point(&tiled_points, &indices, maximum_distance, boundbox, input_points[i], i);
  }

  /* Re-use the same kdtree, so free it before re-creating it. */
  BLI_kdtree_3d_free(*(KDTree_3d **)kd_tree);

  /* Build a new tree with the new indices and tiled points. */
  *kd_tree = BLI_kdtree_3d_new(tiled_points.size());
  for (size_t i = 0; i < tiled_points.size(); i++) {
    BLI_kdtree_3d_insert(*(KDTree_3d **)kd_tree, indices[i], tiled_points[i]);
  }
  BLI_kdtree_3d_balance(*(KDTree_3d **)kd_tree);
}

static void weighted_sample_elimination(const float3 *input_points,
                                        const size_t input_size,
                                        float3 *output_points,
                                        const size_t output_size,
                                        const float maximum_distance,
                                        const float3 boundbox,
                                        const bool do_copy_eliminated)
{
  const float minimum_distance = maximum_distance *
                                 weight_limit_fraction_get(input_size, output_size);

  void *kd_tree = BLI_kdtree_3d_new(input_size);
  const bool tiling = true;
  if (tiling) {
    points_tiling(input_points, input_size, &kd_tree, maximum_distance, boundbox);
  }
  else {
    for (size_t i = 0; i < input_size; i++) {
      BLI_kdtree_3d_insert((KDTree_3d *)kd_tree, i, input_points[i]);
    }
    BLI_kdtree_3d_balance((KDTree_3d *)kd_tree);
  }

  /* Assign weights to each sample. */
  Vector<float> weights(input_size, 0.0f);
  for (size_t point_id = 0; point_id < weights.size(); point_id++) {
    points_distance_weight_calculate(&weights,
                                     point_id,
                                     input_points,
                                     kd_tree,
                                     minimum_distance,
                                     maximum_distance,
                                     nullptr,
                                     nullptr);
  }

  /* Remove the points based on their weight. */
#ifndef CYHEAP
  Heap *heap = BLI_heap_new_ex(weights.size());
  Vector<HeapNode *> nodes(input_size, nullptr);

  for (size_t i = 0; i < weights.size(); i++) {
    nodes[i] = BLI_heap_insert(heap, -weights[i], POINTER_FROM_INT(i));
  }
#else
  cy::Heap heap;
  heap.SetDataPointer(weights.data(), input_size);
  heap.Build();
#endif

  size_t sample_size = input_size;
  while (sample_size > output_size) {
    /* For each sample around it, remove its weight contribution and update the heap. */

#ifndef CYHEAP
    size_t point_id = POINTER_AS_INT(BLI_heap_pop_min(heap));
    nodes[point_id] = nullptr;

    points_distance_weight_calculate(&weights,
                                     point_id,
                                     input_points,
                                     kd_tree,
                                     minimum_distance,
                                     maximum_distance,
                                     heap,
                                     &nodes);
#else
    size_t point_id = heap.GetTopItemID();
    heap.Pop();
    points_distance_weight_calculate(&weights,
                                     point_id,
                                     input_points,
                                     kd_tree,
                                     minimum_distance,
                                     maximum_distance,
                                     &heap,
                                     nullptr);
#endif

    sample_size--;
  }

  /* Copy the samples to the output array. */
  size_t target_size = do_copy_eliminated ? input_size : output_size;
#ifndef CYHEAP
  /* We need to traverse in the reverted order because we want
   * to first have the points that are more isolated (lower weight). */
  for (int i = target_size - 1; i >= 0; i--) {
    size_t index = POINTER_AS_INT(BLI_heap_pop_min(heap));
    output_points[i] = input_points[index];
  }
#else
  for (size_t i = 0; i < target_size; i++) {
    size_t index = heap.GetIDFromHeap(i);
    output_points[i] = input_points[index];
  }
#endif

  /* Cleanup. */
  BLI_kdtree_3d_free((KDTree_3d *)kd_tree);
#ifndef CYHEAP
  BLI_heap_free(heap, NULL);
#endif
}

static void progressive_sampling_reorder(Vector<float3> *output_points,
                                         float maximum_density,
                                         float3 boundbox)
{
  /* Re-order the points for progressive sampling. */
  Vector<float3> temporary_points(output_points->size());
  float3 *source_points = output_points->data();
  float3 *dest_points = temporary_points.data();
  size_t source_size = output_points->size();
  size_t dest_size = 0;

  while (source_size >= 3) {
    dest_size = source_size * 0.5f;

    /* Changes the weight function radius using half of the number of samples.
     * It is used for progressive sampling. */
    maximum_density *= std::sqrt(2.0f);
    weighted_sample_elimination(
        source_points, source_size, dest_points, dest_size, maximum_density, boundbox, true);

    if (dest_points != output_points->data()) {
      mempcpy((*output_points)[dest_size],
              temporary_points[dest_size],
              (source_size - dest_size) * sizeof(float3));
    }

    /* Swap the arrays around. */
    float3 *points_iter = source_points;
    source_points = dest_points;
    dest_points = points_iter;
    source_size = dest_size;
  }
  if (source_points != output_points->data()) {
    memcpy(dest_points, source_points, dest_size * sizeof(float3));
  }
}

void poisson_disk_point_elimination(Vector<float3> const *input_points,
                                    Vector<float3> *output_points,
                                    float maximum_density,
                                    float3 boundbox)
{
  weighted_sample_elimination(input_points->data(),
                              input_points->size(),
                              output_points->data(),
                              output_points->size(),
                              maximum_density,
                              boundbox,
                              false);

  progressive_sampling_reorder(output_points, maximum_density, boundbox);
}

}  // namespace blender::nodes
