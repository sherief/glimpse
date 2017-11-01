
#include <stdbool.h>
#include <math.h>

#include "half.hpp"

#include "infer.h"
#include "xalloc.h"
#include "utils.h"
#include "loader.h"

#define N_SHIFTS 5
#define SHIFT_THRESHOLD 0.001f

#define ARRAY_LEN(ARRAY) (sizeof(ARRAY)/sizeof(ARRAY[0]))


using half_float::half;


typedef struct {
    int n_labels;

    /* XXX: There's a runtime check that we don't have joints mapped to
     * more than two labels and this can easily be bumped if necessary.
     * Keeping this at the minimum size seems likely to help improve
     * locality considering the frequency of accessing this data within
     * some of the inner loops below.
     */
    uint8_t labels[2];
} JointMapEntry;


float*
infer_labels(RDTree** forest, uint8_t n_trees, half* depth_image,
             uint32_t width, uint32_t height)
{
  uint8_t n_labels = forest[0]->header.n_labels;
  float* output_pr = (float*)
    xcalloc(width * height * forest[0]->header.n_labels, sizeof(float));

  for (uint8_t i = 0; i < n_trees; i++)
    {
      RDTree* tree = forest[i];

      // Accumulate probability map
      for (uint32_t y = 0; y < height; y++)
        {
          for (uint32_t x = 0; x < width; x++)
            {
              Int2D pixel = { (int32_t)x, (int32_t)y };
              float depth_value = depth_image[y * width + x];

              Node* node = tree->nodes;
              uint32_t id = 0;
              while (node->label_pr_idx == 0)
                {
                  float value = sample_uv(depth_image, width, height, pixel,
                                          depth_value, node->uv);

                  /* NB: The nodes are arranged in breadth-first, left then
                   * right child order with the root node at index zero.
                   *
                   * In this case if you have an index for any particular node
                   * ('id' here) then 2 * id + 1 is the index for the left
                   * child and 2 * id + 2 is the index for the right child...
                   */
                  id = (value < node->t) ? 2 * id + 1 : 2 * id + 2;

                  node = &tree->nodes[id];
                }

              /* NB: node->label_pr_idx is a base-one index since index zero
               * is reserved to indicate that the node is not a leaf node
               */
              float* pr_table =
                &tree->label_pr_tables[(node->label_pr_idx - 1) * n_labels];
              float* out_pr_table = &output_pr[(y * width * n_labels) +
                                               (x * n_labels)];
              for (int i = 0; i < n_labels; i++)
                {
                  out_pr_table[i] += pr_table[i];
                }
            }
        }
    }

  // Correct the probabilities
  for (uint32_t y = 0, idx = 0; y < height; y++)
    {
      for (uint32_t x = 0; x < width; x++)
        {
          for (uint8_t l = 0; l < n_labels; l++, idx++)
            {
              output_pr[idx] /= (float)n_trees;
            }
        }
    }

  return output_pr;
}

void
infer_labels_from_float(RDTree** forest, uint8_t n_trees, float* depth_image,
                        uint32_t width, uint32_t height,
                        float* output_pr)
{
  uint8_t n_labels = forest[0]->header.n_labels;

  for (uint8_t i = 0; i < n_trees; i++)
    {
      RDTree* tree = forest[i];

      // Accumulate probability map
      for (uint32_t y = 0; y < height; y++)
        {
          for (uint32_t x = 0; x < width; x++)
            {
              Int2D pixel = { (int32_t)x, (int32_t)y };
              float depth = depth_image[y * width + x];

              Node* node = tree->nodes;
              uint32_t id = 0;
              while (node->label_pr_idx == 0)
                {
                  // U in node->uv[0:2] and V in node->uv[2:4] ...
                  vector(float, 4) uv = node->uv;

                  /* XXX: we are always flooring the float to int which is
                   * consistent with what we do while training but it's worth
                   * considering that this might not be ideal and we should
                   * perhaps compare with rounding to the nearest int.
                   *
                   * The explicit casts here stop the compiler warning about
                   * the narrowing implicit cast.
                   */
                  int32_t u[2] = { (int32_t)(pixel[0] + uv[0] / depth),
                                   (int32_t)(pixel[1] + uv[1] / depth) };
                  int32_t v[2] = { (int32_t)(pixel[0] + uv[2] / depth),
                                   (int32_t)(pixel[1] + uv[3] / depth) };

                  float upixel = (u[0] >= 0 && u[0] < (int32_t)width &&
                                  u[1] >= 0 && u[1] < (int32_t)height) ?
                      depth_image[((u[1] * width) + u[0])] : 1000.f;
                  float vpixel = (v[0] >= 0 && v[0] < (int32_t)width &&
                                  v[1] >= 0 && v[1] < (int32_t)height) ?
                      depth_image[((v[1] * width) + v[0])] : 1000.f;

                  float value = upixel - vpixel;

                  /* NB: The nodes are arranged in breadth-first, left then
                   * right child order with the root node at index zero.
                   *
                   * In this case if you have an index for any particular node
                   * ('id' here) then 2 * id + 1 is the index for the left
                   * child and 2 * id + 2 is the index for the right child...
                   */
                  id = (value < node->t) ? 2 * id + 1 : 2 * id + 2;

                  node = &tree->nodes[id];
                }

              /* NB: node->label_pr_idx is a base-one index since index zero
               * is reserved to indicate that the node is not a leaf node
               */
              float* pr_table =
                &tree->label_pr_tables[(node->label_pr_idx - 1) * n_labels];
              float* out_pr_table = &output_pr[(y * width * n_labels) +
                                               (x * n_labels)];
              for (int i = 0; i < n_labels; i++)
                {
                  out_pr_table[i] += pr_table[i];
                }
            }
        }
    }

  // Correct the probabilities
  for (uint32_t y = 0, idx = 0; y < height; y++)
    {
      for (uint32_t x = 0; x < width; x++)
        {
          for (uint8_t l = 0; l < n_labels; l++, idx++)
            {
              output_pr[idx] /= (float)n_trees;
            }
        }
    }
}

/* We don't want to be making lots of function calls or dereferencing
 * lots of pointers while accessing the joint map within inner loops
 * so this lets us temporarily unpack the label mappings into a
 * tight array of JointMapEntries.
 */
static void
unpack_joint_map(JSON_Value *joint_map, JointMapEntry *map, int n_joints)
{
  for (int i = 0; i < n_joints; i++)
    {
      JSON_Object *entry = json_array_get_object(json_array(joint_map), i);
      JSON_Array *labels = json_object_get_array(entry, "labels");
      unsigned n_labels = json_array_get_count(labels);

      if (n_labels > ARRAY_LEN(map[0].labels))
        {
          fprintf(stderr, "Didn't expect joint to be mapped to > 4 labels\n");
          exit(1);
        }

      map[i].n_labels = n_labels;
      for (unsigned n = 0; n < n_labels; n++)
        {
          map[i].labels[n] = json_array_get_number(labels, n);
        }
    }
}

float*
calc_pixel_weights(half* depth_image, float* pr_table,
                   int32_t width, int32_t height, uint8_t n_labels,
                   JSON_Value* joint_map, float* weights)
{
  int n_joints = json_array_get_count(json_array(joint_map));

  JointMapEntry map[n_joints];
  unpack_joint_map(joint_map, map, n_joints);

  if (!weights)
    {
      weights = (float*)xmalloc(width * height * n_joints * sizeof(float));
    }

  for (int32_t y = 0, weight_idx = 0, pixel_idx = 0; y < height; y++)
    {
      for (int32_t x = 0; x < width; x++, pixel_idx++)
        {
          float depth = depth_image[pixel_idx];
          float depth_2 = depth * depth;

          for (uint8_t j = 0; j < n_joints; j++, weight_idx++)
            {
              float pr = 0.f;
              for (int n = 0; n < map[j].n_labels; n++)
                {
                  uint8_t label = map[j].labels[n];
                  pr += pr_table[(pixel_idx * n_labels) + label];
                }
              weights[weight_idx] = pr * depth_2;
            }
        }
    }

  return weights;
}

float*
infer_joints(half* depth_image, float* pr_table, float* weights,
             int32_t width, int32_t height,
             uint8_t n_labels, JSON_Value* joint_map,
             float vfov, JIParam* params)
{
  int n_joints = json_array_get_count(json_array(joint_map));

  JointMapEntry map[n_joints];
  unpack_joint_map(joint_map, map, n_joints);

  // Use mean-shift to find the inferred joint positions, set them back into
  // the body using the given offset, and return the results
  uint32_t* n_pixels = (uint32_t*)xcalloc(n_joints, sizeof(uint32_t));
  size_t points_size = n_joints * width * height * 3 * sizeof(float);
  float* points = (float*)xmalloc(points_size);
  float* density = (float*)xmalloc(points_size);

  // Variables for reprojection of 2d point + depth
  float half_width = width / 2.f;
  float half_height = height / 2.f;
  float aspect = half_width / half_height;

  float vfov_rad = vfov * M_PI / 180.f;
  float tan_half_vfov = tanf(vfov_rad / 2.f);
  float tan_half_hfov = tan_half_vfov * aspect;
  //float hfov = atanf(tan_half_hfov) * 2;

  float root_2pi = sqrtf(2 * M_PI);

  uint32_t total_pixels = width * height;

  // Gather pixels above the given threshold
  for (int32_t y = 0, idx = 0; y < height; y++)
    {
      float t = -((y / half_height) - 1.f);
      for (int32_t x = 0; x < width; x++, idx++)
        {
          float s = (x / half_width) - 1.f;
          float depth = (float)depth_image[idx];
          if (!std::isnormal(depth))
            {
              continue;
            }

          for (uint8_t j = 0; j < n_joints; j++)
            {
              float threshold = params[j].threshold;
              uint32_t joint_idx = j * width * height;

              for (int n = 0; n < map[j].n_labels; n++)
                {
                  uint8_t label = map[j].labels[n];
                  float label_pr = pr_table[(idx * n_labels) + label];
                  if (label_pr >= threshold)
                    {
                      // Reproject point
                      points[(joint_idx + n_pixels[j]) * 3] =
                        (tan_half_hfov * depth) * s;
                      points[(joint_idx + n_pixels[j]) * 3 + 1] =
                        (tan_half_vfov * depth) * t;
                      points[(joint_idx + n_pixels[j]) * 3 + 2] =
                        depth;

                      // Store pixel weight (density)
                      density[joint_idx + n_pixels[j]] =
                        weights[(idx * n_joints) + j];

                      n_pixels[j]++;
                      break;
                    }
                }
            }
        }
    }

  float* joints = (float*)xcalloc(n_joints * 3, sizeof(float));

  // Means shift to find joint modes
  for (uint8_t j = 0; j < n_joints; j++)
    {
      if (n_pixels[j] == 0 || n_pixels[j] == total_pixels)
        {
          continue;
        }

      float bandwidth = params[j].bandwidth;
      float offset = params[j].offset;

      uint32_t joint_idx = j * width * height;
      for (uint32_t s = 0; s < N_SHIFTS; s++)
        {
          float new_points[n_pixels[j] * 3];
          bool moved = false;
          for (uint32_t p = 0; p < n_pixels[j]; p++)
            {
              float* x = &points[(joint_idx + p) * 3];
              float* nx = &new_points[p * 3];
              float numerator[3] = { 0.f, };
              float denominator = 0.f;
              for (uint32_t n = 0; n < n_pixels[j]; n++)
                {
                  float* xi = &points[(joint_idx + n) * 3];
                  float distance = sqrtf(pow(x[0] - xi[0], 2.f) +
                                         pow(x[1] - xi[1], 2.f) +
                                         pow(x[2] - xi[2], 2.f));

                  // Weighted gaussian kernel
                  float weight = density[joint_idx + n] *
                    (1.f / (bandwidth * root_2pi)) *
                    expf(-0.5f * pow(distance / bandwidth, 2.f));

                  numerator[0] += weight * xi[0];
                  numerator[1] += weight * xi[1];
                  numerator[2] += weight * xi[2];

                  denominator += weight;
                }

              nx[0] = numerator[0] / denominator;
              nx[1] = numerator[1] / denominator;
              nx[2] = numerator[2] / denominator;

              if (!moved &&
                  (fabs(nx[0] - x[0]) >= SHIFT_THRESHOLD ||
                   fabs(nx[1] - x[1]) >= SHIFT_THRESHOLD ||
                   fabs(nx[2] - x[2]) >= SHIFT_THRESHOLD))
                {
                  moved = true;
                }
            }

          memcpy((void*)&points[joint_idx * 3], (void*)new_points,
                 n_pixels[j] * 3 * sizeof(float));

          if (!moved || s == N_SHIFTS - 1)
            {
              // Find the mode we're most confident of
              float* last_point = &points[joint_idx * 3];
              float confidence = 0;

              float* best_point = last_point;
              float best_confidence = 0;

              //uint32_t unique_points = 1;

              for (uint32_t p = 0; p < n_pixels[j]; p++)
                {
                  float* point = &points[(joint_idx + p) * 3];
                  if (fabs(point[0]-last_point[0]) >= SHIFT_THRESHOLD ||
                      fabs(point[1]-last_point[1]) >= SHIFT_THRESHOLD ||
                      fabs(point[2]-last_point[2]) >= SHIFT_THRESHOLD)
                    {
                      if (confidence > best_confidence)
                        {
                          best_point = last_point;
                          best_confidence = confidence;
                        }
                      //unique_points++;
                      last_point = point;
                      confidence = 0;
                    }
                  confidence += density[joint_idx + p];
                }

              // Offset into the body
              best_point[2] += offset;

              // Store joint
              joints[j * 3] = best_point[0];
              joints[j * 3 + 1] = best_point[1];
              joints[j * 3 + 2] = best_point[2];

              break;
            }
        }
    }

  xfree(density);
  xfree(points);
  xfree(n_pixels);

  return joints;
}

float*
reproject(half* depth_image, int32_t width, int32_t height,
          float vfov, float threshold, uint32_t* n_points)
{
  float half_width = width / 2.f;
  float half_height = height / 2.f;
  float aspect = half_width / half_height;

  float vfov_rad = vfov * M_PI / 180.f;
  float tan_half_vfov = tanf(vfov_rad / 2.f);
  float tan_half_hfov = tan_half_vfov * aspect;

  float* point_cloud = (float*)xmalloc(width * height * 3 * sizeof(float));

  *n_points = 0;
  int32_t ty = -1;
  for (int32_t y = 0, idx = 0; y < height; y++)
    {
      float t;
      for (int32_t x = 0; x < width; x++, idx++)
        {
          float depth = (float)depth_image[idx];
          if (!std::isnormal(depth) || depth > threshold)
            {
              continue;
            }

          float s = (x / half_width) - 1.f;
          if (ty != y)
            {
              t = -((y / half_height) - 1.f);
              ty = y;
            }
          uint32_t cloud_idx = (*n_points) * 3;

          point_cloud[cloud_idx] = (tan_half_hfov * depth) * s;
          point_cloud[cloud_idx + 1] = (tan_half_vfov * depth) * t;
          point_cloud[cloud_idx + 2] = depth;

          (*n_points)++;
        }
    }

  point_cloud = (float*)xrealloc(point_cloud, (*n_points) * 3 * sizeof(float));

  return point_cloud;
}

half*
project(float* point_cloud, uint32_t n_points, int32_t width, int32_t height,
        float vfov, float background)
{
  float half_width = width / 2.f;
  float half_height = height / 2.f;
  float aspect = half_width / half_height;

  float vfov_rad = vfov * M_PI / 180.f;
  float tan_half_vfov = tanf(vfov_rad / 2.f);
  float tan_half_hfov = tan_half_vfov * aspect;

  half* depth_image = (half*)xmalloc(width * height * sizeof(half));
  half bg_half = (half)background;
  for (int32_t i = 0; i < width * height; i++)
    {
      depth_image[i] = bg_half;
    }

  for (uint32_t i = 0, idx = 0; i < n_points; i++, idx += 3)
    {
      float* point = &point_cloud[idx];

      float x = point[0] / (tan_half_hfov * point[2]);
      if (x < -1.0f || x > 1.0f)
        {
          continue;
        }

      float y = -point[1] / (tan_half_vfov * point[2]);
      if (y < -1.0f || y > 1.0f)
        {
          continue;
        }

      x = (x + 1.0f) * half_width;
      y = (y + 1.0f) * half_height;

      int32_t col = x + 0.5f;
      int32_t row = y + 0.5f;

      depth_image[row * width + col] = (half)point[2];
    }

  return depth_image;
}
