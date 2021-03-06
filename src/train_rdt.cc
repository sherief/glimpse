/*
 * Copyright (C) 2017 Glimp IP Ltd
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <random>
#include <thread>
#include <pthread.h>
#include <time.h>
#include <signal.h>

#include "half.hpp"

#include "xalloc.h"
#include "llist.h"
#include "utils.h"
#include "loader.h"
#include "train_utils.h"

using half_float::half;

static bool verbose = false;
static bool interrupted = false;
static uint32_t seed = 0;

typedef struct {
  int32_t  width;         // Width of training images
  int32_t  height;        // Height of training images
  float    fov;           // Camera field of view
  uint8_t  n_labels;      // Number of labels in label images

  uint32_t n_images;      // Number of training images
  uint8_t* label_images;  // Label images (row-major)
  half*    depth_images;  // Depth images (row-major)

  uint32_t n_uv;          // Number of combinations of u,v pairs
  float    uv_range;      // Range of u,v combinations to generate
  uint32_t n_t;           // The number of thresholds
  float    t_range;       // Range of thresholds to test
  uint8_t  max_depth;     // Maximum depth to train to
  uint32_t n_pixels;      // Number of pixels to sample
  UVPair*  uvs;           // A list of uv pairs to test
  float*   ts;            // A list of thresholds to test
} TrainContext;

typedef struct {
  uint32_t  id;              // Unique id to place the node a tree.
  uint32_t  depth;           // Tree depth at which this node sits.
  uint32_t  n_pixels;        // Number of pixels that have reached this node.
  Int3D*    pixels;          // A list of pixel pairs and image indices.
} NodeTrainData;

typedef struct {
  TrainContext*      ctx;                // The context to use
  NodeTrainData**    data;               // The node data to use and modify
  uint32_t           c_start;            // The uv combination to start on
  uint32_t           c_end;              // The uv combination to end on
  float*             root_nhistogram;    // Normalised histogram of labels
  float*             best_gain;          // Best gain achieved
  uint32_t*          best_uv;            // Index of the best uv combination
  uint32_t*          best_t;             // Index of the best threshold
  uint32_t*          n_lr_pixels;        // Number of pixels in each branch
  pthread_barrier_t* ready_barrier;      // Barrier to wait on to start work
  pthread_barrier_t* finished_barrier;   // Barrier to wait on when finished
} TrainThreadData;

static NodeTrainData*
create_node_train_data(TrainContext* ctx, uint32_t id, uint32_t depth,
                       uint32_t n_pixels, Int3D* pixels)
{
  NodeTrainData* data = (NodeTrainData*)xcalloc(1, sizeof(NodeTrainData));

  data->id = id;
  data->depth = depth;

  if (pixels)
    {
      data->pixels = pixels;
      data->n_pixels = n_pixels;
    }
  else
    {
      // Assume this is the root node and generate random coordinates
      data->n_pixels = ctx->n_images * ctx->n_pixels;
      data->pixels = (Int3D*)xmalloc(data->n_pixels * sizeof(Int3D));

      //std::random_device rd;
      std::mt19937 rng(seed);
      std::uniform_int_distribution<int> rand_x(0, ctx->width - 1);
      std::uniform_int_distribution<int> rand_y(0, ctx->height - 1);
      for (uint32_t i = 0, idx = 0; i < ctx->n_images; i++)
        {
          for (uint32_t j = 0; j < ctx->n_pixels; j++, idx++)
            {
              data->pixels[idx].xy[0] = rand_x(rng);
              data->pixels[idx].xy[1] = rand_y(rng);
              data->pixels[idx].i = i;
            }
        }
    }

  return data;
}

static void
destroy_node_train_data(NodeTrainData* data)
{
  xfree(data->pixels);
  xfree(data);
}

static inline Int2D
normalize_histogram(uint32_t* histogram, uint8_t n_labels, float* normalized)
{
  Int2D sums = { 0, 0 };

  for (int i = 0; i < n_labels; i++)
    {
      if (histogram[i] > 0)
        {
          sums[0] += histogram[i];
          ++sums[1];
        }
    }

  if (sums[0] > 0)
    {
      for (int i = 0; i < n_labels; i++)
        {
          normalized[i] = histogram[i] / (float)sums[0];
        }
    }
  else
    {
      memset(normalized, 0, n_labels * sizeof(float));
    }

  return sums;
}

static inline float
calculate_shannon_entropy(float* normalized_histogram, uint8_t n_labels)
{
  float entropy = 0.f;
  for (int i = 0; i < n_labels; i++)
    {
      float value = normalized_histogram[i];
      if (value > 0.f && value < 1.f)
        {
          entropy += -value * log2f(value);
        }
    }
  return entropy;
}

static inline float
calculate_gain(float entropy, uint32_t n_pixels,
               float l_entropy, uint32_t l_n_pixels,
               float r_entropy, uint32_t r_n_pixels)
{
  return entropy - ((l_n_pixels / (float)n_pixels * l_entropy) +
                    (r_n_pixels / (float)n_pixels * r_entropy));
}

static void
accumulate_histograms(TrainContext* ctx, NodeTrainData* data,
                      uint32_t c_start, uint32_t c_end,
                      uint32_t* root_histogram, uint32_t* lr_histograms)
{
  for (uint32_t p = 0; p < data->n_pixels && !interrupted; p++)
    {
      Int2D pixel = data->pixels[p].xy;
      uint32_t i = data->pixels[p].i;
      uint32_t image_idx = i * ctx->width * ctx->height;

      half* depth_image = &ctx->depth_images[image_idx];
      uint8_t* label_image = &ctx->label_images[image_idx];

      uint32_t pixel_idx = (pixel[1] * ctx->width) + pixel[0];
      uint8_t label = label_image[pixel_idx];
      float depth = depth_image[pixel_idx];

      if (label >= ctx->n_labels)
        {
          fprintf(stderr, "Label '%u' is bigger than expected (max %u)\n",
                  (uint32_t)label, (uint32_t)ctx->n_labels - 1);
          exit(1);
        }

      // Accumulate root histogram
      ++root_histogram[label];

      // Don't waste processing time if this is the last depth
      if (data->depth >= (uint32_t)ctx->max_depth - 1)
        {
          continue;
        }

      // Accumulate LR branch histograms

      // Sample pixels
      float samples[c_end - c_start];
      for (uint32_t c = c_start; c < c_end; c++)
        {
          UVPair uv = ctx->uvs[c];
          samples[c - c_start] = sample_uv(depth_image,
                                           ctx->width, ctx->height,
                                           pixel, depth, uv);
        }

      // Partition on thresholds
      for (uint32_t c = 0, lr_histogram_idx = 0; c < c_end - c_start; c++)
        {
          for (uint32_t t = 0; t < ctx->n_t;
               t++, lr_histogram_idx += ctx->n_labels * 2)
            {
              // Accumulate histogram for this particular uvt combination
              // on both theoretical branches
              float threshold = ctx->ts[t];
              ++lr_histograms[samples[c] < threshold ?
                lr_histogram_idx + label :
                lr_histogram_idx + ctx->n_labels + label];
            }
        }
    }
}

static void*
thread_body(void* userdata)
{
  TrainThreadData* data = (TrainThreadData*)userdata;

  // Histogram for the node being processed
  uint32_t* root_histogram = (uint32_t*)
    malloc(data->ctx->n_labels * sizeof(uint32_t));

  // Histograms for each uvt combination being tested
  uint32_t* lr_histograms = (uint32_t*)
    malloc(data->ctx->n_labels * (data->c_end - data->c_start) *
           data->ctx->n_t * 2 * sizeof(uint32_t));

  float* nhistogram = (float*)xmalloc(data->ctx->n_labels * sizeof(float));
  float* root_nhistogram = data->root_nhistogram ? data->root_nhistogram :
    (float*)xmalloc(data->ctx->n_labels * sizeof(float));

  while (1)
    {
      // Wait for everything to be ready to start processing
      pthread_barrier_wait(data->ready_barrier);

      // Quit out if we've nothing left to process or we've been interrupted
      if (!(*data->data) || interrupted)
        {
          break;
        }

      // Clear histogram accumulators
      memset(root_histogram, 0, data->ctx->n_labels * sizeof(uint32_t));
      memset(lr_histograms, 0, data->ctx->n_labels *
             (data->c_end - data->c_start) * data->ctx->n_t * 2 *
             sizeof(uint32_t));

      // Accumulate histograms
      accumulate_histograms(data->ctx, *data->data, data->c_start, data->c_end,
                            root_histogram, lr_histograms);

      // Calculate the normalised label histogram and get the number of pixels
      // and the number of labels in the root histogram.
      Int2D root_n_pixels = normalize_histogram(root_histogram,
                                                data->ctx->n_labels,
                                                root_nhistogram);

      // Determine the best u,v,t combination
      *data->best_gain = 0.f;

      // If there's only 1 label, skip all this, gain is zero
      if (root_n_pixels[1] > 1 &&
          (*data->data)->depth < (uint32_t)data->ctx->max_depth - 1)
        {
          // Calculate the shannon entropy for the normalised label histogram
          float entropy = calculate_shannon_entropy(root_nhistogram,
                                                    data->ctx->n_labels);

          // Calculate the gain for each combination of u,v,t and store the best
          for (uint32_t i = data->c_start, lr_histo_base = 0;
               i < data->c_end && !interrupted; i++)
            {
              for (uint32_t j = 0; j < data->ctx->n_t && !interrupted;
                   j++, lr_histo_base += data->ctx->n_labels * 2)
                {
                  float l_entropy, r_entropy, gain;

                  Int2D l_n_pixels =
                    normalize_histogram(&lr_histograms[lr_histo_base],
                                        data->ctx->n_labels, nhistogram);
                  if (l_n_pixels[0] == 0 || l_n_pixels[0] == root_n_pixels[0])
                    {
                      continue;
                    }
                  l_entropy = calculate_shannon_entropy(nhistogram,
                                                        data->ctx->n_labels);

                  Int2D r_n_pixels =
                    normalize_histogram(
                      &lr_histograms[lr_histo_base + data->ctx->n_labels],
                      data->ctx->n_labels, nhistogram);
                  r_entropy = calculate_shannon_entropy(nhistogram,
                                                        data->ctx->n_labels);

                  gain = calculate_gain(entropy, root_n_pixels[0],
                                        l_entropy, l_n_pixels[0],
                                        r_entropy, r_n_pixels[0]);

                  if (gain > *data->best_gain)
                    {
                      *data->best_gain = gain;
                      *data->best_uv = i;
                      *data->best_t = j;
                      data->n_lr_pixels[0] = l_n_pixels[0];
                      data->n_lr_pixels[1] = r_n_pixels[0];
                    }
                }
            }
        }

      // Signal work is finished
      pthread_barrier_wait(data->finished_barrier);
    }

  xfree(root_histogram);
  xfree(lr_histograms);
  if (!data->root_nhistogram)
    {
      xfree(root_nhistogram);
    }
  xfree(nhistogram);
  xfree(data);

  pthread_exit(NULL);
}

static void
collect_pixels(TrainContext* ctx, NodeTrainData* data, UVPair uv, float t,
               Int3D** l_pixels, Int3D** r_pixels, uint32_t* n_lr_pixels)
{
  *l_pixels = (Int3D*)xmalloc((n_lr_pixels[0] ? n_lr_pixels[0] :
                                                data->n_pixels) *
                              sizeof(Int3D));
  *r_pixels = (Int3D*)xmalloc((n_lr_pixels[1] ? n_lr_pixels[1] :
                                                data->n_pixels) *
                              sizeof(Int3D));

  uint32_t l_index = 0;
  uint32_t r_index = 0;
  for (uint32_t p = 0; p < data->n_pixels; p++)
    {
      Int3D* pixel = &data->pixels[p];
      uint32_t image_idx = pixel->i * ctx->width * ctx->height;
      half* depth_image = &ctx->depth_images[image_idx];

      float depth = depth_image[(pixel->xy[1] * ctx->width) + pixel->xy[0]];
      float value = sample_uv(depth_image, ctx->width, ctx->height,
                              pixel->xy, depth, uv);

      if (value < t)
        {
          (*l_pixels)[l_index++] = *pixel;
        }
      else
        {
          (*r_pixels)[r_index++] = *pixel;
        }
    }

  if (n_lr_pixels[0] != l_index)
    {
      *l_pixels = (Int3D*)xrealloc(*l_pixels, l_index * sizeof(Int3D));
      n_lr_pixels[0] = l_index;
    }

  if (n_lr_pixels[1] != r_index)
    {
      *r_pixels = (Int3D*)xrealloc(*r_pixels, r_index * sizeof(Int3D));
      n_lr_pixels[1] = r_index;
    }
}

static bool
list_free_cb(LList* node, uint32_t index, void* userdata)
{
  xfree(node->data);
  return true;
}

static void
print_usage(FILE* stream)
{
  fprintf(stream,
"Usage: train_rdt <data dir> <index name> <out file> [OPTIONS]\n"
"Train a randomised decision tree to infer n_labels from depth and label images\n"
"with a given camera FOV. Default values assume depth data to be in meters.\n"
"\n"
"  -l, --limit=NUMBER[,NUMBER]   Limit training data to this many images.\n"
"                                Optionally, skip the first N images.\n"
"  -s, --shuffle                 Shuffle order of training images.\n"
"  -p, --pixels=NUMBER           Number of pixels to sample per image.\n"
"                                  (default: 2000)\n"
"  -t, --thresholds=NUMBER       Number of thresholds to test.\n"
"                                  (default: 50)\n"
"  -r, --t-range=NUMBER          Range of thresholds to test.\n"
"                                  (default: 1.29)\n"
"  -c, --combos=NUMBER           Number of UV combinations to test.\n"
"                                  (default: 2000)\n"
"  -u, --uv-range=NUMBER         Range of UV combinations to test.\n"
"                                  (default 1.29)\n"
"  -d, --depth=NUMBER            Depth to train tree to.\n"
"                                  (default: 20)\n"
"  -m, --threads=NUMBER          Number of threads to use.\n"
"                                  (default: autodetect)\n"
"  -b, --background=NUMBER       Index of the background label\n"
"                                  (default: 0)\n"
"  -n, --seed=NUMBER             Seed to use for RNG.\n"
"                                  (default: 0)\n"
"  -i, --continue                Continue training from an interrupted run.\n"
"  -v, --verbose                 Verbose output.\n"
"  -h, --help                    Display this message.\n");
}

void
sigint_handler(int signum)
{
  if (!interrupted)
    {
      printf("\nUser-triggered interrupt, saving checkpoint...\n");
      interrupted = true;
    }
  else
    {
      printf("\nInterrupted during checkpoint, quitting!\n");
      exit(1);
    }
}

int
main(int argc, char **argv)
{
  TrainContext ctx = { 0, };
  TimeForDisplay since_begin, since_last;
  struct timespec begin, last, now;
  uint32_t n_threads = std::thread::hardware_concurrency();
  uint8_t bg_label = 0;

  if (argc < 4)
    {
      print_usage(stderr);
      exit(1);
    }

  const char *data_dir = argv[1];
  const char *index_name = argv[2];
  const char *out_filename = argv[3];

  // Set default parameters
  ctx.n_uv = 2000;
  ctx.uv_range = 1.29;
  ctx.n_t = 50;
  ctx.t_range = 1.29;
  ctx.max_depth = 20;
  ctx.n_pixels = 2000;
  uint32_t limit = UINT32_MAX;
  uint32_t skip = 0;
  bool shuffle = false;

  for (int i = 4; i < argc; i++)
    {
      // All arguments should start with '-'
      if (argv[i][0] != '-')
        {
          print_usage(stderr);
          return 1;
        }
      char* arg = &argv[i][1];

      char param = '\0';
      char* value = NULL;
      if (arg[0] == '-')
        {
          // Store the location of the value (if applicable)
          value = strchr(arg, '=');
          if (value)
            {
              value += 1;
            }

          // Check argument
          arg++;
          if (strstr(arg, "limit="))
            {
              param = 'l';
            }
          else if (strcmp(arg, "shuffle") == 0)
            {
              param = 's';
            }
          else if (strstr(arg, "pixels="))
            {
              param = 'p';
            }
          else if (strstr(arg, "thresholds="))
            {
              param = 't';
            }
          else if (strstr(arg, "t-range="))
            {
              param = 'r';
            }
          else if (strstr(arg, "combos="))
            {
              param = 'c';
            }
          else if (strstr(arg, "uv-range="))
            {
              param = 'u';
            }
          else if (strstr(arg, "depth="))
            {
              param = 'd';
            }
          else if (strstr(arg, "background="))
            {
              param = 'b';
            }
          else if (strstr(arg, "threads="))
            {
              param = 'm';
            }
          else if (strstr(arg, "seed="))
            {
              param = 'n';
            }
          else if (strcmp(arg, "continue") == 0)
            {
              param = 'i';
            }
          else if (strcmp(arg, "verbose") == 0)
            {
              param = 'v';
            }
          else if (strcmp(arg, "help") == 0)
            {
              param = 'h';
            }
          arg--;
        }
      else
        {
          if (arg[1] == '\0')
            {
              param = arg[0];
            }

          if (i + 1 < argc)
            {
              value = argv[i + 1];
            }
        }

      // Check for parameter-less options
      switch(param)
        {
        case 's':
          shuffle = true;
          continue;
        case 'i':
          interrupted = true;
          continue;
        case 'v':
          verbose = true;
          continue;
        case 'h':
          print_usage(stdout);
          return 0;
        }

      // Now check for options that require parameters
      if (!value)
        {
          print_usage(stderr);
          return 1;
        }
      if (arg[0] != '-')
        {
          i++;
        }

      switch(param)
        {
        case 'l':
          limit = (uint32_t)strtol(value, &value, 10);
          if (value[0] != '\0')
            {
              skip = (uint32_t)strtol(value + 1, NULL, 10);
            }
          break;
        case 'p':
          ctx.n_pixels = (uint32_t)atoi(value);
          break;
        case 't':
          ctx.n_t = (uint32_t)atoi(value);
          break;
        case 'r':
          ctx.t_range = strtof(value, NULL);
          break;
        case 'c':
          ctx.n_uv = (uint32_t)atoi(value);
          break;
        case 'u':
          ctx.uv_range = strtof(value, NULL);
          break;
        case 'd':
          ctx.max_depth = (uint8_t)atoi(value);
          break;
        case 'b':
          bg_label = (uint8_t)atoi(value);
          break;
        case 'm':
          n_threads = (uint32_t)atoi(value);
          break;
        case 'n':
          seed = (uint32_t)atoi(value);
          break;

        default:
          print_usage(stderr);
          return 1;
        }
    }

  printf("Scanning training directories...\n");
  gather_train_data(data_dir,
                    index_name,
                    NULL, // no joint map
                    limit, skip, shuffle,
                    &ctx.n_images, NULL, &ctx.width, &ctx.height,
                    &ctx.depth_images, &ctx.label_images, NULL,
                    &ctx.n_labels,
                    &ctx.fov);

  // Work out pixels per meter and adjust uv range accordingly
  float ppm = (ctx.height / 2.f) / tanf(ctx.fov / 2.f);
  ctx.uv_range *= ppm;

  // Calculate the u,v,t parameters that we're going to test
  printf("Preparing training metadata...\n");
  ctx.uvs = (UVPair*)xmalloc(ctx.n_uv * sizeof(UVPair));
  //std::random_device rd;
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> rand_uv(-ctx.uv_range / 2.f,
                                                 ctx.uv_range / 2.f);
  for (uint32_t i = 0; i < ctx.n_uv; i++)
    {
      ctx.uvs[i][0] = rand_uv(rng);
      ctx.uvs[i][1] = rand_uv(rng);
      ctx.uvs[i][2] = rand_uv(rng);
      ctx.uvs[i][3] = rand_uv(rng);
    }
  ctx.ts = (float*)xmalloc(ctx.n_t * sizeof(float));
  for (uint32_t i = 0; i < ctx.n_t; i++)
    {
      ctx.ts[i] = -ctx.t_range / 2.f + (i * ctx.t_range / (float)(ctx.n_t - 1));
    }

  // Allocate memory for the normalised histogram of the currently training node
  float* root_nhistogram = (float*)xmalloc(ctx.n_labels * sizeof(float));

  NodeTrainData* node_data = NULL;
  printf("Initialising %u threads...\n", n_threads);
  pthread_barrier_t ready_barrier, finished_barrier;
  if (pthread_barrier_init(&ready_barrier, NULL, n_threads + 1) != 0 ||
      pthread_barrier_init(&finished_barrier, NULL, n_threads + 1) != 0)
    {
      fprintf(stderr, "Error initialising thread barriers\n");
      return 1;
    }
  uint32_t n_c = ctx.n_uv / n_threads;
  float* best_gains = (float*)malloc(n_threads * sizeof(float));
  uint32_t* best_uvs = (uint32_t*)malloc(n_threads * sizeof(uint32_t));
  uint32_t* best_ts = (uint32_t*)malloc(n_threads * sizeof(uint32_t));
  uint32_t* all_n_lr_pixels = (uint32_t*)
    malloc(n_threads * 2 * sizeof(uint32_t));
  pthread_t threads[n_threads];
  for (uint32_t i = 0; i < n_threads; i++)
    {
      TrainThreadData* thread_data = (TrainThreadData*)
        xmalloc(sizeof(TrainThreadData));
      thread_data->ctx = &ctx;
      thread_data->data = &node_data;
      thread_data->c_start = i * n_c;
      thread_data->c_end = (i == n_threads - 1) ? ctx.n_uv : (i + 1) * n_c;
      thread_data->root_nhistogram = (i == 0) ? root_nhistogram : NULL;
      thread_data->best_gain = &best_gains[i];
      thread_data->best_uv = &best_uvs[i];
      thread_data->best_t = &best_ts[i];
      thread_data->n_lr_pixels = &all_n_lr_pixels[i * 2];
      thread_data->ready_barrier = &ready_barrier;
      thread_data->finished_barrier = &finished_barrier;

      if (pthread_create(&threads[i], NULL, thread_body,
                         (void*)thread_data) != 0)
        {
          fprintf(stderr, "Error creating thread\n");
          return 1;
        }
    }

  // Allocate memory to store the decision tree.
  uint32_t n_nodes = (uint32_t)roundf(powf(2.f, ctx.max_depth)) - 1;
  Node* tree = (Node*)xcalloc(n_nodes, sizeof(Node));

  // Initialise root node training data and add it to the queue
  LList* train_queue = llist_new(create_node_train_data(&ctx, 0, 0, 0, NULL));

  // Initialise histogram count
  uint32_t n_histograms = 0;
  LList* tree_histograms = NULL;

  // If -i was passed, try to load the partial tree and repopulate the training
  // queue and tree histogram list
  RDTree* checkpoint;
  if (interrupted && (checkpoint = read_tree(out_filename)))
    {
      printf("Restoring checkpoint...\n");

      // Do some basic validation
      if (checkpoint->header.n_labels != ctx.n_labels)
        {
          fprintf(stderr, "Checkpoint has %d labels, expected %d\n",
                  (int)checkpoint->header.n_labels, (int)ctx.n_labels);
          return 1;
        }

      if (fabs(checkpoint->header.fov - ctx.fov) > 1e-6)
        {
          fprintf(stderr, "Checkpoint has FOV %.2f, expected %.2f\n",
                  checkpoint->header.fov, ctx.fov);
          return 1;
        }

      if (checkpoint->header.depth > ctx.max_depth)
        {
          fprintf(stderr,
                  "Can't train with a lower depth than checkpoint (%d < %d)\n",
                  (int)ctx.max_depth, (int)checkpoint->header.depth);
          return 1;
        }

      // Restore nodes
      uint32_t n_checkpoint_nodes = (uint32_t)
        roundf(powf(2.f, checkpoint->header.depth)) - 1;
      memcpy(tree, checkpoint->nodes, n_checkpoint_nodes * sizeof(Node));

      // Navigate the tree to determine any unfinished nodes and the last
      // trained depth
      LList* checkpoint_queue = train_queue;
      train_queue = NULL;
      while (checkpoint_queue)
        {
          NodeTrainData* data = (NodeTrainData*)
            llist_pop(&checkpoint_queue, NULL, NULL);
          Node* node = &tree[data->id];

          // Check if the node has a valid probability table and copy it to
          // the list if so. Given the order in which we iterate over the tree,
          // we can just append to the list. Note that the code expects
          // tree_histograms to point to the end of the list.
          if (node->label_pr_idx != 0 && node->label_pr_idx != UINT32_MAX)
            {
              float* pr_table = &checkpoint->
                label_pr_tables[ctx.n_labels * (node->label_pr_idx - 1)];
              float* pr_copy = (float*)xmalloc(ctx.n_labels * sizeof(float));
              memcpy(pr_copy, pr_table, ctx.n_labels * sizeof(float));
              tree_histograms = llist_insert_after(tree_histograms,
                                                   llist_new(pr_copy));
              ++n_histograms;
            }

          // Check if the node is either marked as incomplete, or it sits on
          // the last depth of the tree and we're trying to train deeper.
          if (node->label_pr_idx == UINT32_MAX ||
              (data->depth == (uint32_t)(checkpoint->header.depth - 1) &&
               ctx.max_depth > checkpoint->header.depth))
            {
              // This node is referenced and incomplete, add it to the training
              // queue.
              train_queue ?
                llist_insert_after(train_queue, llist_new(data)) :
                train_queue = llist_new(data);
              continue;
            }

          // If the node isn't a leaf-node, calculate which pixels should go
          // to the next two nodes and add them to the checkpoint queue
          if (node->label_pr_idx == 0)
            {
              Int3D* l_pixels;
              Int3D* r_pixels;
              uint32_t n_lr_pixels[] = { 0, 0 };
              collect_pixels(&ctx, data, node->uv, node->t, &l_pixels, &r_pixels,
                             n_lr_pixels);

              uint32_t id = (2 * data->id) + 1;
              uint32_t depth = data->depth + 1;
              NodeTrainData* ldata = create_node_train_data(
                &ctx, id, depth, n_lr_pixels[0], l_pixels);
              NodeTrainData* rdata = create_node_train_data(
                &ctx, id + 1, depth, n_lr_pixels[1], r_pixels);

              checkpoint_queue =
                llist_first(
                  llist_insert_after(
                    llist_insert_after(llist_last(checkpoint_queue),
                                       llist_new(ldata)),
                    llist_new(rdata)));
            }

          // Free the unused training data
          destroy_node_train_data(data);
        }

      free_tree(checkpoint);
      interrupted = false;

      if (!train_queue)
        {
          fprintf(stderr, "Tree already fully trained.\n");
          return 1;
        }
    }
  else
    {
      // Mark nodes in tree as unfinished, for checkpoint restoration
      for (uint32_t i = 0; i < n_nodes; i++)
        {
          tree[i].label_pr_idx = UINT32_MAX;
        }
    }

  printf("Beginning training...\n");
  signal(SIGINT, sigint_handler);
  clock_gettime(CLOCK_MONOTONIC, &begin);
  last = begin;
  uint32_t last_depth = UINT32_MAX;
  while (train_queue != NULL)
    {
      uint32_t best_uv = 0;
      uint32_t best_t = 0;
      uint32_t *n_lr_pixels = NULL;
      float best_gain = 0.0;

      LList* current = train_queue;
      node_data = (NodeTrainData*)current->data;

      if (node_data->depth != last_depth)
        {
          clock_gettime(CLOCK_MONOTONIC, &now);
          since_begin = get_time_for_display(&begin, &now);
          since_last = get_time_for_display(&last, &now);
          last = now;
          last_depth = node_data->depth;
          printf("(%02d:%02d:%02d / %02d:%02d:%02d) Training depth %u (%u nodes)\n",
                 since_begin.hours, since_begin.minutes, since_begin.seconds,
                 since_last.hours, since_last.minutes, since_last.seconds,
                 last_depth + 1, llist_length(train_queue));
        }

      // Signal threads to start work
      pthread_barrier_wait(&ready_barrier);

      // Wait for threads to finish
      pthread_barrier_wait(&finished_barrier);


      if (interrupted)
        {
          break;
        }
      // Quit if we've been interrupted
      if (interrupted)
        {
          break;
        }

      // See which thread got the best uvt combination
      for (uint32_t i = 0; i < n_threads; i++)
        {
          if (best_gains[i] > best_gain)
            {
              best_gain = best_gains[i];
              best_uv = best_uvs[i];
              best_t = best_ts[i];
              n_lr_pixels = &all_n_lr_pixels[i * 2];
            }
        }

      // Add this node to the tree and possible add left/ride nodes to the
      // training queue.
      Node* node = &tree[node_data->id];
      if (best_gain > 0.f && (node_data->depth + 1) < ctx.max_depth)
        {
          node->uv = ctx.uvs[best_uv];
          node->t = ctx.ts[best_t];
          if (verbose)
            {
              printf("  Node (%u)\n"
                     "    Gain: %f\n"
                     "    U: (%f, %f)\n"
                     "    V: (%f, %f)\n"
                     "    T: %f\n",
                     node_data->id, best_gain,
                     node->uv[0], node->uv[1],
                     node->uv[2], node->uv[3],
                     node->t);
            }

          Int3D* l_pixels;
          Int3D* r_pixels;

          collect_pixels(&ctx, node_data, node->uv, node->t,
                         &l_pixels, &r_pixels, n_lr_pixels);

          uint32_t id = (2 * node_data->id) + 1;
          uint32_t depth = node_data->depth + 1;
          NodeTrainData* ldata = create_node_train_data(
            &ctx, id, depth, n_lr_pixels[0], l_pixels);
          NodeTrainData* rdata = create_node_train_data(
            &ctx, id + 1, depth, n_lr_pixels[1], r_pixels);

          // Insert nodes into the training queue
          llist_insert_after(
            llist_insert_after(llist_last(train_queue), llist_new(ldata)),
            llist_new(rdata));

          // Mark the node as a continuing node
          node->label_pr_idx = 0;
        }
      else
        {
          if (verbose)
            {
              printf("  Leaf node (%u)\n", (uint32_t)node_data->id);
              for (int i = 0; i < ctx.n_labels; i++)
                {
                  if (root_nhistogram[i] > 0.f)
                    {
                      printf("    %02d - %f\n", i, root_nhistogram[i]);
                    }
                }
            }

          node->label_pr_idx = ++n_histograms;
          float* node_histogram = (float*)xmalloc(ctx.n_labels * sizeof(float));
          memcpy(node_histogram, root_nhistogram, ctx.n_labels * sizeof(float));
          tree_histograms = llist_insert_after(tree_histograms,
                                               llist_new(node_histogram));
        }

      // Remove this node from the queue
      train_queue = train_queue->next;
      llist_free(llist_remove(current), NULL, NULL);

      // We no longer need the train data, free it
      destroy_node_train_data(node_data);
    }

  // Signal threads to free memory and quit
  node_data = NULL;
  pthread_barrier_wait(&ready_barrier);

  for (uint32_t i = 0; i < n_threads; i++)
    {
      if (pthread_join(threads[i], NULL) != 0)
        {
          fprintf(stderr, "Error joining thread, trying to continue...\n");
        }
    }

  // Free memory that isn't needed anymore
  xfree(root_nhistogram);
  xfree(ctx.uvs);
  xfree(ctx.ts);
  xfree(ctx.label_images);
  xfree(ctx.depth_images);
  xfree(best_gains);
  xfree(best_uvs);
  xfree(best_ts);
  xfree(all_n_lr_pixels);

  // Restore tree histograms list pointer
  tree_histograms = llist_first(tree_histograms);

  // Write to file
  clock_gettime(CLOCK_MONOTONIC, &now);
  since_begin = get_time_for_display(&begin, &now);
  since_last = get_time_for_display(&last, &now);
  last = now;
  printf("(%02d:%02d:%02d / %02d:%02d:%02d) Writing output to '%s'...\n",
         since_begin.hours, since_begin.minutes, since_begin.seconds,
         since_last.hours, since_last.minutes, since_last.seconds,
         out_filename);

  RDTHeader header = { { 'R', 'D', 'T' }, RDT_VERSION, ctx.max_depth, \
                       ctx.n_labels, bg_label, ctx.fov };
  RDTree rdtree = { header, tree, llist_length(tree_histograms), NULL };
  rdtree.label_pr_tables = (float*)
    xmalloc(ctx.n_labels * rdtree.n_pr_tables * sizeof(float));
  float* pr_table = rdtree.label_pr_tables;
  for (LList* l = tree_histograms; l; l = l->next, pr_table += ctx.n_labels)
    {
      memcpy(pr_table, l->data, sizeof(float) * ctx.n_labels);
    }

  save_tree(&rdtree, out_filename);

  char* out_filename_json = (char*)xmalloc(strlen(out_filename) + 6);
  strcpy(out_filename_json, out_filename);
  strcat(out_filename_json, ".json");
  save_tree_json(&rdtree, out_filename_json, false);

  xfree(rdtree.label_pr_tables);
  xfree(out_filename_json);

  // Free the last data
  xfree(tree);
  llist_free(tree_histograms, list_free_cb, NULL);

  clock_gettime(CLOCK_MONOTONIC, &now);
  since_begin = get_time_for_display(&begin, &now);
  since_last = get_time_for_display(&last, &now);
  last = now;
  printf("(%02d:%02d:%02d / %02d:%02d:%02d) %s\n",
         since_begin.hours, since_begin.minutes, since_begin.seconds,
         since_last.hours, since_last.minutes, since_last.seconds,
         interrupted ? "Interrupted!" : "Done!");

  return 0;
}
