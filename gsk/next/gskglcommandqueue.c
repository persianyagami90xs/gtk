/* gskglcommandqueue.c
 *
 * Copyright 2017 Timm Bäder <mail@baedert.org>
 * Copyright 2018 Matthias Clasen <mclasen@redhat.com>
 * Copyright 2018 Alexander Larsson <alexl@redhat.com>
 * Copyright 2020 Christian Hergert <chergert@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <string.h>

#include <gdk/gdkglcontextprivate.h>
#include <gdk/gdkmemorytextureprivate.h>
#include <gdk/gdkprofilerprivate.h>
#include <gsk/gskdebugprivate.h>
#include <gsk/gskroundedrectprivate.h>

#include "gskglattachmentstateprivate.h"
#include "gskglbufferprivate.h"
#include "gskglcommandqueueprivate.h"
#include "gskgluniformstateprivate.h"

#include "inlinearray.h"

G_DEFINE_TYPE (GskGLCommandQueue, gsk_gl_command_queue, G_TYPE_OBJECT)

static inline void
print_uniform (GskGLUniformFormat format,
               guint              array_count,
               gconstpointer      valueptr)
{
  const union {
    graphene_matrix_t matrix[0];
    GskRoundedRect rounded_rect[0];
    float fval[0];
    int ival[0];
    guint uval[0];
  } *data = valueptr;

  switch (format)
    {
    case GSK_GL_UNIFORM_FORMAT_1F:
      g_printerr ("1f<%f>", data->fval[0]);
      break;

    case GSK_GL_UNIFORM_FORMAT_2F:
      g_printerr ("2f<%f,%f>", data->fval[0], data->fval[1]);
      break;

    case GSK_GL_UNIFORM_FORMAT_3F:
      g_printerr ("3f<%f,%f,%f>", data->fval[0], data->fval[1], data->fval[2]);
      break;

    case GSK_GL_UNIFORM_FORMAT_4F:
      g_printerr ("4f<%f,%f,%f,%f>", data->fval[0], data->fval[1], data->fval[2], data->fval[3]);
      break;

    case GSK_GL_UNIFORM_FORMAT_1I:
    case GSK_GL_UNIFORM_FORMAT_TEXTURE:
      g_printerr ("1i<%d>", data->ival[0]);
      break;

    case GSK_GL_UNIFORM_FORMAT_1UI:
      g_printerr ("1ui<%u>", data->uval[0]);
      break;

    case GSK_GL_UNIFORM_FORMAT_COLOR: {
      char *str = gdk_rgba_to_string (valueptr);
      g_printerr ("%s", str);
      g_free (str);
      break;
    }

    case GSK_GL_UNIFORM_FORMAT_ROUNDED_RECT: {
      char *str = gsk_rounded_rect_to_string (valueptr);
      g_printerr ("%s", str);
      g_free (str);
      break;
    }

    case GSK_GL_UNIFORM_FORMAT_MATRIX: {
      float mat[16];
      graphene_matrix_to_float (&data->matrix[0], mat);
      g_printerr ("matrix<");
      for (guint i = 0; i < G_N_ELEMENTS (mat)-1; i++)
        g_printerr ("%f,", mat[i]);
      g_printerr ("%f>", mat[G_N_ELEMENTS (mat)-1]);
      break;
    }

    case GSK_GL_UNIFORM_FORMAT_1FV:
    case GSK_GL_UNIFORM_FORMAT_2FV:
    case GSK_GL_UNIFORM_FORMAT_3FV:
    case GSK_GL_UNIFORM_FORMAT_4FV:
      /* non-V variants are -4 from V variants */
      format -= 4;
      g_printerr ("[");
      for (guint i = 0; i < array_count; i++)
        {
          print_uniform (format, 0, valueptr);
          if (i + 1 != array_count)
            g_printerr (",");
          valueptr = ((guint8*)valueptr + gsk_gl_uniform_format_size (format));
        }
      g_printerr ("]");
      break;

    case GSK_GL_UNIFORM_FORMAT_2I:
      g_printerr ("2i<%d,%d>", data->ival[0], data->ival[1]);
      break;

    case GSK_GL_UNIFORM_FORMAT_3I:
      g_printerr ("3i<%d,%d,%d>", data->ival[0], data->ival[1], data->ival[2]);
      break;

    case GSK_GL_UNIFORM_FORMAT_4I:
      g_printerr ("3i<%d,%d,%d,%d>", data->ival[0], data->ival[1], data->ival[2], data->ival[3]);
      break;

    case GSK_GL_UNIFORM_FORMAT_LAST:
    default:
      g_assert_not_reached ();
    }
}

static inline void
gsk_gl_command_queue_print_batch (GskGLCommandQueue       *self,
                                  const GskGLCommandBatch *batch)
{
  static const char *command_kinds[] = { "Clear", NULL, NULL, "Draw", };
  guint framebuffer_id;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (batch != NULL);

  if (batch->any.kind == GSK_GL_COMMAND_KIND_CLEAR)
    framebuffer_id = batch->clear.framebuffer;
  else if (batch->any.kind == GSK_GL_COMMAND_KIND_DRAW)
    framebuffer_id = batch->draw.framebuffer;
  else
    return;

  g_printerr ("Batch {\n");
  g_printerr ("         Kind: %s\n", command_kinds[batch->any.kind]);
  g_printerr ("     Viewport: %dx%d\n", batch->any.viewport.width, batch->any.viewport.height);
  g_printerr ("  Framebuffer: %d\n", framebuffer_id);

  if (batch->any.kind == GSK_GL_COMMAND_KIND_DRAW)
    {
      g_printerr ("      Program: %d\n", batch->any.program);
      g_printerr ("     Vertices: %d\n", batch->draw.vbo_count);

      for (guint i = 0; i < batch->draw.bind_count; i++)
        {
          const GskGLCommandBind *bind = &self->batch_binds.items[batch->draw.bind_offset + i];
          g_print ("      Bind[%d]: %u\n", bind->texture, bind->id);
        }

      for (guint i = 0; i < batch->draw.uniform_count; i++)
        {
          const GskGLCommandUniform *uniform = &self->batch_uniforms.items[batch->draw.uniform_offset + i];
          g_printerr ("  Uniform[%02d]: ", uniform->location);
          print_uniform (uniform->info.format,
                         uniform->info.array_count,
                         gsk_gl_uniform_state_get_uniform_data (self->uniforms, uniform->info.offset));
          g_printerr ("\n");
        }
    }
  else if (batch->any.kind == GSK_GL_COMMAND_KIND_CLEAR)
    {
      g_printerr ("         Bits: 0x%x\n", batch->clear.bits);
    }

  g_printerr ("}\n");
}

static inline void
gsk_gl_command_queue_capture_png (GskGLCommandQueue *self,
                                  const char        *filename,
                                  guint              width,
                                  guint              height,
                                  gboolean           flip_y)
{
  cairo_surface_t *surface;
  guint8 *data;
  guint stride;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (filename != NULL);

  stride = cairo_format_stride_for_width (CAIRO_FORMAT_ARGB32, width);
  data = g_malloc_n (height, stride);

  glReadPixels (0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, data);

  if (flip_y)
    {
      guint8 *flipped = g_malloc_n (height, stride);

      for (guint i = 0; i < height; i++)
        memcpy (flipped + (height * stride) - ((i + 1) * stride),
                data + (stride * i),
                stride);

      g_free (data);
      data = flipped;
    }

  surface = cairo_image_surface_create_for_data (data, CAIRO_FORMAT_ARGB32, width, height, stride);
  cairo_surface_write_to_png (surface, filename);

  cairo_surface_destroy (surface);
  g_free (data);
}

static inline guint
snapshot_attachments (const GskGLAttachmentState *state,
                      GskGLCommandBinds          *array)
{
  GskGLCommandBind *bind = gsk_gl_command_binds_append_n (array, G_N_ELEMENTS (state->textures));
  guint count = 0;

  for (guint i = 0; i < G_N_ELEMENTS (state->textures); i++)
    {
      if (state->textures[i].id)
        {
          bind[count].id = state->textures[i].id;
          bind[count].texture = state->textures[i].texture;
          count++;
        }
    }

  if (count != G_N_ELEMENTS (state->textures))
    array->len -= G_N_ELEMENTS (state->textures) - count;

  return count;
}

static inline guint
snapshot_uniforms (GskGLUniformState    *state,
                   GskGLUniformProgram  *program,
                   GskGLCommandUniforms *array)
{
  GskGLCommandUniform *uniform = gsk_gl_command_uniforms_append_n (array, program->n_sparse);
  guint count = 0;

  for (guint i = 0; i < program->n_sparse; i++)
    {
      guint location = program->sparse[i];
      const GskGLUniformInfo *info = &program->uniforms[location].info;

      if (!info->initial)
        {
          uniform[count].location = location;
          uniform[count].info = *info;
          count++;
        }
    }

  if (count != program->n_sparse)
    array->len -= program->n_sparse - count;

  return count;
}

static inline gboolean
snapshots_equal (GskGLCommandQueue *self,
                 GskGLCommandBatch *first,
                 GskGLCommandBatch *second)
{
  if (first->draw.bind_count != second->draw.bind_count ||
      first->draw.uniform_count != second->draw.uniform_count)
    return FALSE;

  for (guint i = 0; i < first->draw.bind_count; i++)
    {
      const GskGLCommandBind *fb = &self->batch_binds.items[first->draw.bind_offset+i];
      const GskGLCommandBind *sb = &self->batch_binds.items[second->draw.bind_offset+i];

      if (fb->id != sb->id || fb->texture != sb->texture)
        return FALSE;
    }

  for (guint i = 0; i < first->draw.uniform_count; i++)
    {
      const GskGLCommandUniform *fu = &self->batch_uniforms.items[first->draw.uniform_offset+i];
      const GskGLCommandUniform *su = &self->batch_uniforms.items[second->draw.uniform_offset+i];
      gconstpointer fdata;
      gconstpointer sdata;
      gsize len;

      /* Short circuit if we'd end up with the same memory */
      if (fu->info.offset == su->info.offset)
        continue;

      if (fu->info.format != su->info.format ||
          fu->info.array_count != su->info.array_count)
        return FALSE;

      fdata = gsk_gl_uniform_state_get_uniform_data (self->uniforms, fu->info.offset);
      sdata = gsk_gl_uniform_state_get_uniform_data (self->uniforms, su->info.offset);

      switch (fu->info.format)
        {
        case GSK_GL_UNIFORM_FORMAT_1F:
        case GSK_GL_UNIFORM_FORMAT_1FV:
        case GSK_GL_UNIFORM_FORMAT_1I:
        case GSK_GL_UNIFORM_FORMAT_TEXTURE:
        case GSK_GL_UNIFORM_FORMAT_1UI:
          len = 4;
          break;

        case GSK_GL_UNIFORM_FORMAT_2F:
        case GSK_GL_UNIFORM_FORMAT_2FV:
        case GSK_GL_UNIFORM_FORMAT_2I:
          len = 8;
          break;

        case GSK_GL_UNIFORM_FORMAT_3F:
        case GSK_GL_UNIFORM_FORMAT_3FV:
        case GSK_GL_UNIFORM_FORMAT_3I:
          len = 12;
          break;

        case GSK_GL_UNIFORM_FORMAT_4F:
        case GSK_GL_UNIFORM_FORMAT_4FV:
        case GSK_GL_UNIFORM_FORMAT_4I:
          len = 16;
          break;


        case GSK_GL_UNIFORM_FORMAT_MATRIX:
          len = sizeof (float) * 16;
          break;

        case GSK_GL_UNIFORM_FORMAT_ROUNDED_RECT:
          len = sizeof (float) * 12;
          break;

        case GSK_GL_UNIFORM_FORMAT_COLOR:
          len = sizeof (float) * 4;
          break;

        default:
          g_assert_not_reached ();
        }

      len *= fu->info.array_count;

      if (memcmp (fdata, sdata, len) != 0)
        return FALSE;
    }

  return TRUE;
}

static void
gsk_gl_command_queue_dispose (GObject *object)
{
  GskGLCommandQueue *self = (GskGLCommandQueue *)object;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  g_clear_object (&self->profiler);
  g_clear_object (&self->gl_profiler);
  g_clear_object (&self->context);
  g_clear_pointer (&self->attachments, gsk_gl_attachment_state_unref);
  g_clear_pointer (&self->uniforms, gsk_gl_uniform_state_unref);

  gsk_gl_command_batches_clear (&self->batches);
  gsk_gl_command_binds_clear (&self->batch_binds);
  gsk_gl_command_uniforms_clear (&self->batch_uniforms);

  gsk_gl_buffer_destroy (&self->vertices);

  G_OBJECT_CLASS (gsk_gl_command_queue_parent_class)->dispose (object);
}

static void
gsk_gl_command_queue_class_init (GskGLCommandQueueClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gsk_gl_command_queue_dispose;
}

static void
gsk_gl_command_queue_init (GskGLCommandQueue *self)
{
  self->max_texture_size = -1;

  gsk_gl_command_batches_init (&self->batches, 128);
  gsk_gl_command_binds_init (&self->batch_binds, 1024);
  gsk_gl_command_uniforms_init (&self->batch_uniforms, 2048);

  self->debug_groups = g_string_chunk_new (4096);

  gsk_gl_buffer_init (&self->vertices, GL_ARRAY_BUFFER, sizeof (GskGLDrawVertex));
}

GskGLCommandQueue *
gsk_gl_command_queue_new (GdkGLContext      *context,
                          GskGLUniformState *uniforms)
{
  GskGLCommandQueue *self;

  g_return_val_if_fail (GDK_IS_GL_CONTEXT (context), NULL);

  self = g_object_new (GSK_TYPE_GL_COMMAND_QUEUE, NULL);
  self->context = g_object_ref (context);
  self->attachments = gsk_gl_attachment_state_new ();

  /* Use shared uniform state if we're provided one */
  if (uniforms != NULL)
    self->uniforms = gsk_gl_uniform_state_ref (uniforms);
  else
    self->uniforms = gsk_gl_uniform_state_new ();

  /* Determine max texture size immediately and restore context */
  gdk_gl_context_make_current (context);
  glGetIntegerv (GL_MAX_TEXTURE_SIZE, &self->max_texture_size);

  return g_steal_pointer (&self);
}

static inline GskGLCommandBatch *
begin_next_batch (GskGLCommandQueue *self)
{
  GskGLCommandBatch *batch;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  batch = gsk_gl_command_batches_append (&self->batches);
  batch->any.next_batch_index = -1;
  batch->any.prev_batch_index = self->tail_batch_index;

  return batch;
}

static void
enqueue_batch (GskGLCommandQueue *self)
{
  guint index;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (self->batches.len > 0);

  index = self->batches.len - 1;

  if (self->head_batch_index == -1)
    self->head_batch_index = index;

  if (self->tail_batch_index != -1)
    {
      GskGLCommandBatch *prev = &self->batches.items[self->tail_batch_index];

      prev->any.next_batch_index = index;
    }

  self->tail_batch_index = index;
}

static void
discard_batch (GskGLCommandQueue *self)
{
  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (self->batches.len > 0);

  self->batches.len--;
}

void
gsk_gl_command_queue_begin_draw (GskGLCommandQueue     *self,
                                 GskGLUniformProgram   *program,
                                 const graphene_rect_t *viewport)
{
  GskGLCommandBatch *batch;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (self->in_draw == FALSE);
  g_assert (viewport != NULL);

  /* Our internal links use 16-bits, so that is our max number
   * of batches we can have in one frame.
   */
  if (self->batches.len == G_MAXINT16)
    return;

  self->program_info = program;

  batch = begin_next_batch (self);
  batch->any.kind = GSK_GL_COMMAND_KIND_DRAW;
  batch->any.program = program->program_id;
  batch->any.next_batch_index = -1;
  batch->any.viewport.width = viewport->size.width;
  batch->any.viewport.height = viewport->size.height;
  batch->draw.framebuffer = 0;
  batch->draw.uniform_count = 0;
  batch->draw.uniform_offset = self->batch_uniforms.len;
  batch->draw.bind_count = 0;
  batch->draw.bind_offset = self->batch_binds.len;
  batch->draw.vbo_count = 0;
  batch->draw.vbo_offset = gsk_gl_buffer_get_offset (&self->vertices);

  self->fbo_max = MAX (self->fbo_max, batch->draw.framebuffer);

  self->in_draw = TRUE;
}

void
gsk_gl_command_queue_end_draw (GskGLCommandQueue *self)
{
  GskGLCommandBatch *last_batch;
  GskGLCommandBatch *batch;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (self->batches.len > 0);

  /* Max batches is 16-bit */
  if (self->batches.len == G_MAXINT16)
    return;

  batch = gsk_gl_command_batches_tail (&self->batches);

  g_assert (self->in_draw == TRUE);
  g_assert (batch->any.kind == GSK_GL_COMMAND_KIND_DRAW);

  if G_UNLIKELY (batch->draw.vbo_count == 0)
    {
      discard_batch (self);
      self->in_draw = FALSE;
      return;
    }

  /* Track the destination framebuffer in case it changed */
  batch->draw.framebuffer = self->attachments->fbo.id;
  self->attachments->fbo.changed = FALSE;
  self->fbo_max = MAX (self->fbo_max, self->attachments->fbo.id);

  /* Save our full uniform state for this draw so we can possibly
   * reorder the draw later.
   */
  batch->draw.uniform_offset = self->batch_uniforms.len;
  batch->draw.uniform_count = snapshot_uniforms (self->uniforms, self->program_info, &self->batch_uniforms);

  /* Track the bind attachments that changed */
  if (self->program_info->has_attachments)
    {
      batch->draw.bind_offset = self->batch_binds.len;
      batch->draw.bind_count = snapshot_attachments (self->attachments, &self->batch_binds);
    }
  else
    {
      batch->draw.bind_offset = 0;
      batch->draw.bind_count = 0;
    }

  if (self->batches.len > 1)
    last_batch = &self->batches.items[self->batches.len - 2];
  else
    last_batch = NULL;

  /* Do simple chaining of draw to last batch. */
  if (last_batch != NULL &&
      last_batch->any.kind == GSK_GL_COMMAND_KIND_DRAW &&
      last_batch->any.program == batch->any.program &&
      last_batch->any.viewport.width == batch->any.viewport.width &&
      last_batch->any.viewport.height == batch->any.viewport.height &&
      last_batch->draw.framebuffer == batch->draw.framebuffer &&
      last_batch->draw.vbo_offset + last_batch->draw.vbo_count == batch->draw.vbo_offset &&
      snapshots_equal (self, last_batch, batch))
    {
      last_batch->draw.vbo_count += batch->draw.vbo_count;
      discard_batch (self);
    }
  else
    {
      enqueue_batch (self);
    }

  self->in_draw = FALSE;
  self->program_info = NULL;
}

/**
 * gsk_gl_command_queue_split_draw:
 * @self a #GskGLCommandQueue
 *
 * This function is like calling gsk_gl_command_queue_end_draw() followed by
 * a gsk_gl_command_queue_begin_draw() with the same parameters as a
 * previous begin draw (if shared uniforms where not changed further).
 *
 * This is useful to avoid comparisons inside of loops where we know shared
 * uniforms are not changing.
 *
 * This generally should just be called from gsk_gl_program_split_draw()
 * as that is where the begin/end flow happens from the render job.
 */
void
gsk_gl_command_queue_split_draw (GskGLCommandQueue *self)
{
  GskGLCommandBatch *batch;
  GskGLUniformProgram *program;
  graphene_rect_t viewport;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (self->batches.len > 0);
  g_assert (self->in_draw == TRUE);

  program = self->program_info;

  batch = gsk_gl_command_batches_tail (&self->batches);

  g_assert (batch->any.kind == GSK_GL_COMMAND_KIND_DRAW);

  viewport.origin.x = 0;
  viewport.origin.y = 0;
  viewport.size.width = batch->any.viewport.width;
  viewport.size.height = batch->any.viewport.height;

  gsk_gl_command_queue_end_draw (self);
  gsk_gl_command_queue_begin_draw (self, program, &viewport);
}

void
gsk_gl_command_queue_clear (GskGLCommandQueue     *self,
                            guint                  clear_bits,
                            const graphene_rect_t *viewport)
{
  GskGLCommandBatch *batch;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (self->in_draw == FALSE);

  if (self->batches.len == G_MAXINT16)
    return;

  if (clear_bits == 0)
    clear_bits = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;

  batch = begin_next_batch (self);
  batch->any.kind = GSK_GL_COMMAND_KIND_CLEAR;
  batch->any.viewport.width = viewport->size.width;
  batch->any.viewport.height = viewport->size.height;
  batch->clear.bits = clear_bits;
  batch->clear.framebuffer = self->attachments->fbo.id;
  batch->any.next_batch_index = -1;
  batch->any.program = 0;

  self->fbo_max = MAX (self->fbo_max, batch->clear.framebuffer);

  enqueue_batch (self);

  self->attachments->fbo.changed = FALSE;
}

GdkGLContext *
gsk_gl_command_queue_get_context (GskGLCommandQueue *self)
{
  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (self), NULL);

  return self->context;
}

void
gsk_gl_command_queue_make_current (GskGLCommandQueue *self)
{
  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (GDK_IS_GL_CONTEXT (self->context));

  gdk_gl_context_make_current (self->context);
}

void
gsk_gl_command_queue_delete_program (GskGLCommandQueue *self,
                                     guint              program)
{
  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  glDeleteProgram (program);
}

static inline void
apply_uniform (gconstpointer    dataptr,
               GskGLUniformInfo info,
               guint            location)
{
  g_assert (dataptr != NULL);
  g_assert (info.format > 0);
  g_assert (location < GL_MAX_UNIFORM_LOCATIONS);

  switch (info.format)
    {
    case GSK_GL_UNIFORM_FORMAT_1F:
      glUniform1fv (location, 1, dataptr);
    break;

    case GSK_GL_UNIFORM_FORMAT_2F:
      glUniform2fv (location, 1, dataptr);
    break;

    case GSK_GL_UNIFORM_FORMAT_3F:
      glUniform3fv (location, 1, dataptr);
    break;

    case GSK_GL_UNIFORM_FORMAT_4F:
      glUniform4fv (location, 1, dataptr);
    break;

    case GSK_GL_UNIFORM_FORMAT_1FV:
      glUniform1fv (location, info.array_count, dataptr);
    break;

    case GSK_GL_UNIFORM_FORMAT_2FV:
      glUniform2fv (location, info.array_count, dataptr);
    break;

    case GSK_GL_UNIFORM_FORMAT_3FV:
      glUniform3fv (location, info.array_count, dataptr);
    break;

    case GSK_GL_UNIFORM_FORMAT_4FV:
      glUniform4fv (location, info.array_count, dataptr);
    break;

    case GSK_GL_UNIFORM_FORMAT_1I:
    case GSK_GL_UNIFORM_FORMAT_TEXTURE:
      glUniform1iv (location, 1, dataptr);
    break;

    case GSK_GL_UNIFORM_FORMAT_2I:
      glUniform2iv (location, 1, dataptr);
    break;

    case GSK_GL_UNIFORM_FORMAT_3I:
      glUniform3iv (location, 1, dataptr);
    break;

    case GSK_GL_UNIFORM_FORMAT_4I:
      glUniform4iv (location, 1, dataptr);
    break;

    case GSK_GL_UNIFORM_FORMAT_1UI:
      glUniform1uiv (location, 1, dataptr);
    break;

    case GSK_GL_UNIFORM_FORMAT_MATRIX: {
      float mat[16];
      graphene_matrix_to_float (dataptr, mat);
      glUniformMatrix4fv (location, 1, GL_FALSE, mat);
#if 0
      /* TODO: If Graphene can give us a peek here on platforms
       * where the format is float[16] (most/all x86_64?) then
       * We can avoid the SIMD operation to convert the format.
       */
      G_STATIC_ASSERT (sizeof (graphene_matrix_t) == 16*4);
      glUniformMatrix4fv (location, 1, GL_FALSE, dataptr);
#endif
    }
    break;

    case GSK_GL_UNIFORM_FORMAT_COLOR:
      glUniform4fv (location, 1, dataptr);
    break;

    case GSK_GL_UNIFORM_FORMAT_ROUNDED_RECT:
      glUniform4fv (location, 3, dataptr);
    break;

    default:
      g_assert_not_reached ();
    }
}

static inline void
apply_viewport (guint *current_width,
                guint *current_height,
                guint  width,
                guint  height)
{
  if G_UNLIKELY (*current_width != width || *current_height != height)
    {
      *current_width = width;
      *current_height = height;
      glViewport (0, 0, width, height);
    }
}

static inline void
apply_scissor (gboolean              *state,
               guint                  framebuffer,
               const graphene_rect_t *scissor,
               gboolean               has_scissor)
{
  g_assert (framebuffer != (guint)-1);

  if (framebuffer != 0 || !has_scissor)
    {
      if (*state != FALSE)
        {
          glDisable (GL_SCISSOR_TEST);
          *state = FALSE;
        }
    }
  else
    {
      if (*state != TRUE)
        {
          glEnable (GL_SCISSOR_TEST);
          glScissor (scissor->origin.x,
                     scissor->origin.y,
                     scissor->size.width,
                     scissor->size.height);
          *state = TRUE;
        }
    }
}

static inline gboolean
apply_framebuffer (int *framebuffer,
                   guint new_framebuffer)
{
  if G_UNLIKELY (new_framebuffer != *framebuffer)
    {
      *framebuffer = new_framebuffer;
      glBindFramebuffer (GL_FRAMEBUFFER, new_framebuffer);
      return TRUE;
    }

  return FALSE;
}

static inline void
gsk_gl_command_queue_unlink (GskGLCommandQueue *self,
                             GskGLCommandBatch *batch)
{
  if (batch->any.prev_batch_index == -1)
    self->head_batch_index = batch->any.next_batch_index;
  else
    self->batches.items[batch->any.prev_batch_index].any.next_batch_index = batch->any.next_batch_index;

  if (batch->any.next_batch_index == -1)
    self->tail_batch_index = batch->any.prev_batch_index;
  else
    self->batches.items[batch->any.next_batch_index].any.prev_batch_index = batch->any.prev_batch_index;

  batch->any.prev_batch_index = -1;
  batch->any.next_batch_index = -1;
}

static inline void
gsk_gl_command_queue_insert_before (GskGLCommandQueue *self,
                                    GskGLCommandBatch *batch,
                                    GskGLCommandBatch *sibling)
{
  int sibling_index;
  int index;

  g_assert (batch >= self->batches.items);
  g_assert (batch < &self->batches.items[self->batches.len]);
  g_assert (sibling >= self->batches.items);
  g_assert (sibling < &self->batches.items[self->batches.len]);

  index = gsk_gl_command_batches_index_of (&self->batches, batch);
  sibling_index = gsk_gl_command_batches_index_of (&self->batches, sibling);

  batch->any.next_batch_index = sibling_index;
  batch->any.prev_batch_index = sibling->any.prev_batch_index;

  if (batch->any.prev_batch_index > -1)
    self->batches.items[batch->any.prev_batch_index].any.next_batch_index = index;

  sibling->any.prev_batch_index = index;

  if (batch->any.prev_batch_index == -1)
    self->head_batch_index = index;
}

static void
gsk_gl_command_queue_sort_batches (GskGLCommandQueue *self)
{
  int *seen;
  int *seen_free = NULL;
  int index;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (self->tail_batch_index >= 0);
  g_assert (self->fbo_max >= 0);

  /* Create our seen list with most recent index set to -1,
   * meaning we haven't yet seen that framebuffer.
   */
  if (self->fbo_max < 1024)
    seen = g_alloca (sizeof (int) * (self->fbo_max + 1));
  else
    seen = seen_free = g_new0 (int, (self->fbo_max + 1));
  for (int i = 0; i <= self->fbo_max; i++)
    seen[i] = -1;

  /* Walk in reverse, and if we've seen that framebuffer before,
   * we want to delay this operation until right before the last
   * batch we saw for that framebuffer.
   */
  index = self->tail_batch_index;

  while (index >= 0)
    {
      GskGLCommandBatch *batch = &self->batches.items[index];
      int cur_index = index;
      int fbo = -1;

      g_assert (index > -1);
      g_assert (index < self->batches.len);

      switch (batch->any.kind)
        {
        case GSK_GL_COMMAND_KIND_DRAW:
          fbo = batch->draw.framebuffer;
          break;

        case GSK_GL_COMMAND_KIND_CLEAR:
          fbo = batch->clear.framebuffer;
          break;

        default:
          g_assert_not_reached ();
        }

      index = batch->any.prev_batch_index;

      g_assert (index >= -1);
      g_assert (index < (int)self->batches.len);
      g_assert (fbo >= -1);

      if (fbo == -1)
        continue;

      g_assert (fbo <= self->fbo_max);
      g_assert (seen[fbo] >= -1);
      g_assert (seen[fbo] < (int)self->batches.len);

      if (seen[fbo] != -1 && seen[fbo] != batch->any.next_batch_index)
        {
          int mru_index = seen[fbo];
          GskGLCommandBatch *mru = &self->batches.items[mru_index];

          g_assert (mru_index > -1);

          gsk_gl_command_queue_unlink (self, batch);

          g_assert (batch->any.prev_batch_index == -1);
          g_assert (batch->any.next_batch_index == -1);

          gsk_gl_command_queue_insert_before (self, batch, mru);

          g_assert (batch->any.prev_batch_index > -1 ||
                    self->head_batch_index == cur_index);
          g_assert (batch->any.next_batch_index == seen[fbo]);
        }

      g_assert (cur_index > -1);
      g_assert (seen[fbo] >= -1);

      seen[fbo] = cur_index;
    }

  g_free (seen_free);
}

/**
 * gsk_gl_command_queue_execute:
 * @self: a #GskGLCommandQueue
 * @surface_height: the height of the backing surface
 * @scale_factor: the scale factor of the backing surface
 * #scissor: (nullable): the scissor clip if any
 *
 * Executes all of the batches in the command queue.
 */
void
gsk_gl_command_queue_execute (GskGLCommandQueue    *self,
                              guint                 surface_height,
                              guint                 scale_factor,
                              const cairo_region_t *scissor)
{
  G_GNUC_UNUSED guint count = 0;
  graphene_rect_t scissor_test;
  gboolean has_scissor = scissor != NULL;
  gboolean scissor_state = -1;
  guint program = 0;
  guint width = 0;
  guint height = 0;
  guint n_binds = 0;
  guint n_fbos = 0;
  guint n_uniforms = 0;
  guint vao_id;
  guint vbo_id;
  int textures[4];
  int framebuffer = -1;
  int next_batch_index;
  int active = -1;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (self->in_draw == FALSE);

  if (self->batches.len == 0)
    return;

  for (guint i = 0; i < G_N_ELEMENTS (textures); i++)
    textures[i] = -1;

  gsk_gl_command_queue_sort_batches (self);

  gsk_gl_command_queue_make_current (self);

#ifdef G_ENABLE_DEBUG
  gsk_gl_profiler_begin_gpu_region (self->gl_profiler);
  gsk_profiler_timer_begin (self->profiler, self->metrics.cpu_time);
#endif

  glEnable (GL_DEPTH_TEST);
  glDepthFunc (GL_LEQUAL);

  /* Pre-multiplied alpha */
  glEnable (GL_BLEND);
  glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation (GL_FUNC_ADD);

  glGenVertexArrays (1, &vao_id);
  glBindVertexArray (vao_id);

  vbo_id = gsk_gl_buffer_submit (&self->vertices);

  /* 0 = position location */
  glEnableVertexAttribArray (0);
  glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE,
                         sizeof (GskGLDrawVertex),
                         (void *) G_STRUCT_OFFSET (GskGLDrawVertex, position));

  /* 1 = texture coord location */
  glEnableVertexAttribArray (1);
  glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE,
                         sizeof (GskGLDrawVertex),
                         (void *) G_STRUCT_OFFSET (GskGLDrawVertex, uv));

  /* Setup initial scissor clip */
  if (scissor != NULL)
    {
      cairo_rectangle_int_t r;

      g_assert (cairo_region_num_rectangles (scissor) == 1);
      cairo_region_get_rectangle (scissor, 0, &r);

      scissor_test.origin.x = r.x * scale_factor;
      scissor_test.origin.y = surface_height - (r.height * scale_factor) - (r.y * scale_factor);
      scissor_test.size.width = r.width * scale_factor;
      scissor_test.size.height = r.height * scale_factor;
    }

  next_batch_index = self->head_batch_index;

  while (next_batch_index >= 0)
    {
      const GskGLCommandBatch *batch = &self->batches.items[next_batch_index];

      g_assert (next_batch_index >= 0);
      g_assert (next_batch_index < self->batches.len);
      g_assert (batch->any.next_batch_index != next_batch_index);

      count++;

      switch (batch->any.kind)
        {
        case GSK_GL_COMMAND_KIND_CLEAR:
          if (apply_framebuffer (&framebuffer, batch->clear.framebuffer))
            {
              apply_scissor (&scissor_state, framebuffer, &scissor_test, has_scissor);
              n_fbos++;
            }

          apply_viewport (&width,
                          &height,
                          batch->any.viewport.width,
                          batch->any.viewport.height);

          glClearColor (0, 0, 0, 0);
          glClear (batch->clear.bits);
        break;

        case GSK_GL_COMMAND_KIND_DRAW:
          if (batch->any.program != program)
            {
              program = batch->any.program;
              glUseProgram (program);
            }

          if (apply_framebuffer (&framebuffer, batch->draw.framebuffer))
            {
              apply_scissor (&scissor_state, framebuffer, &scissor_test, has_scissor);
              n_fbos++;
            }

          apply_viewport (&width,
                          &height,
                          batch->any.viewport.width,
                          batch->any.viewport.height);

          if G_UNLIKELY (batch->draw.bind_count > 0)
            {
              const GskGLCommandBind *bind = &self->batch_binds.items[batch->draw.bind_offset];

              for (guint i = 0; i < batch->draw.bind_count; i++)
                {
                  if (textures[bind->texture] != bind->id)
                    {
                      if (active != bind->texture)
                        {
                          active = bind->texture;
                          glActiveTexture (GL_TEXTURE0 + bind->texture);
                        }

                      glBindTexture (GL_TEXTURE_2D, bind->id);
                      textures[bind->texture] = bind->id;
                    }

                  bind++;
                }

              n_binds += batch->draw.bind_count;
            }

          if (batch->draw.uniform_count > 0)
            {
              const GskGLCommandUniform *u = &self->batch_uniforms.items[batch->draw.uniform_offset];

              for (guint i = 0; i < batch->draw.uniform_count; i++, u++)
                apply_uniform (gsk_gl_uniform_state_get_uniform_data (self->uniforms, u->info.offset),
                               u->info, u->location);

              n_uniforms += batch->draw.uniform_count;
            }

          glDrawArrays (GL_TRIANGLES, batch->draw.vbo_offset, batch->draw.vbo_count);

        break;

        default:
          g_assert_not_reached ();
        }

#if 0
      if (batch->any.kind == GSK_GL_COMMAND_KIND_DRAW ||
          batch->any.kind == GSK_GL_COMMAND_KIND_CLEAR)
        {
          char filename[128];
          g_snprintf (filename, sizeof filename,
                      "capture%03u_batch%03d_kind%u_program%u_u%u_b%u_fb%u_ctx%p.png",
                      count, next_batch_index,
                      batch->any.kind, batch->any.program,
                      batch->any.kind == GSK_GL_COMMAND_KIND_DRAW ? batch->draw.uniform_count : 0,
                      batch->any.kind == GSK_GL_COMMAND_KIND_DRAW ? batch->draw.bind_count : 0,
                      framebuffer,
                      gdk_gl_context_get_current ());
          gsk_gl_command_queue_capture_png (self, filename, width, height, TRUE);
          gsk_gl_command_queue_print_batch (self, batch);
        }
#endif

      next_batch_index = batch->any.next_batch_index;
    }

  glDeleteBuffers (1, &vbo_id);
  glDeleteVertexArrays (1, &vao_id);

  gdk_profiler_set_int_counter (self->metrics.n_binds, n_binds);
  gdk_profiler_set_int_counter (self->metrics.n_uniforms, n_uniforms);
  gdk_profiler_set_int_counter (self->metrics.n_fbos, n_fbos);
  gdk_profiler_set_int_counter (self->metrics.n_uploads, self->n_uploads);

#ifdef G_ENABLE_DEBUG
  {
    gint64 start_time G_GNUC_UNUSED = gsk_profiler_timer_get_start (self->profiler, self->metrics.cpu_time);
    gint64 cpu_time = gsk_profiler_timer_end (self->profiler, self->metrics.cpu_time);
    gint64 gpu_time = gsk_gl_profiler_end_gpu_region (self->gl_profiler);

    gsk_profiler_timer_set (self->profiler, self->metrics.gpu_time, gpu_time);
    gsk_profiler_timer_set (self->profiler, self->metrics.cpu_time, cpu_time);
    gsk_profiler_counter_inc (self->profiler, self->metrics.n_frames);

    gsk_profiler_push_samples (self->profiler);
  }
#endif
}

void
gsk_gl_command_queue_begin_frame (GskGLCommandQueue *self)
{
  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (self->batches.len == 0);

  gsk_gl_command_queue_make_current (self);

  self->fbo_max = 0;
  self->tail_batch_index = -1;
  self->head_batch_index = -1;
  self->in_frame = TRUE;
}

/**
 * gsk_gl_command_queue_end_frame:
 * @self: a #GskGLCommandQueue
 *
 * This function performs cleanup steps that need to be done after
 * a frame has finished. This is not performed as part of the command
 * queue execution to allow for the frame to be submitted as soon
 * as possible.
 *
 * However, it should be executed after the draw contexts end_frame
 * has been called to swap the OpenGL framebuffers.
 */
void
gsk_gl_command_queue_end_frame (GskGLCommandQueue *self)
{
  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  gsk_gl_command_queue_make_current (self);
  gsk_gl_uniform_state_end_frame (self->uniforms);

  /* Reset attachments so we don't hold on to any textures
   * that might be released after the frame.
   */
  for (guint i = 0; i < G_N_ELEMENTS (self->attachments->textures); i++)
    {
      if (self->attachments->textures[i].id != 0)
        {
          glActiveTexture (GL_TEXTURE0 + i);
          glBindTexture (GL_TEXTURE_2D, 0);

          self->attachments->textures[i].id = 0;
          self->attachments->textures[i].changed = FALSE;
          self->attachments->textures[i].initial = TRUE;
        }
    }

  g_string_chunk_clear (self->debug_groups);

  self->batches.len = 0;
  self->batch_binds.len = 0;
  self->batch_uniforms.len = 0;
  self->n_uploads = 0;
  self->tail_batch_index = -1;
  self->in_frame = FALSE;
}

gboolean
gsk_gl_command_queue_create_render_target (GskGLCommandQueue *self,
                                           int                width,
                                           int                height,
                                           int                min_filter,
                                           int                mag_filter,
                                           guint             *out_fbo_id,
                                           guint             *out_texture_id)
{
  GLuint fbo_id = 0;
  GLint texture_id;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (width > 0);
  g_assert (height > 0);
  g_assert (out_fbo_id != NULL);
  g_assert (out_texture_id != NULL);

  texture_id = gsk_gl_command_queue_create_texture (self,
                                                    width, height,
                                                    min_filter, mag_filter);

  if (texture_id == -1)
    {
      *out_fbo_id = 0;
      *out_texture_id = 0;
      return FALSE;
    }

  fbo_id = gsk_gl_command_queue_create_framebuffer (self);

  glBindFramebuffer (GL_FRAMEBUFFER, fbo_id);
  glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_id, 0);
  g_assert_cmphex (glCheckFramebufferStatus (GL_FRAMEBUFFER), ==, GL_FRAMEBUFFER_COMPLETE);

  *out_fbo_id = fbo_id;
  *out_texture_id = texture_id;

  return TRUE;
}

int
gsk_gl_command_queue_create_texture (GskGLCommandQueue *self,
                                     int                width,
                                     int                height,
                                     int                min_filter,
                                     int                mag_filter)
{
  GLuint texture_id = 0;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  if G_UNLIKELY (self->max_texture_size == -1)
    glGetIntegerv (GL_MAX_TEXTURE_SIZE, &self->max_texture_size);

  if (width > self->max_texture_size || height > self->max_texture_size)
    return -1;

  glGenTextures (1, &texture_id);

  glActiveTexture (GL_TEXTURE0);
  glBindTexture (GL_TEXTURE_2D, texture_id);

  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  if (gdk_gl_context_get_use_es (self->context))
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  else
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

  /* Restore the previous texture if it was set */
  if (self->attachments->textures[0].id != 0)
    glBindTexture (GL_TEXTURE_2D, self->attachments->textures[0].id);

  return (int)texture_id;
}

guint
gsk_gl_command_queue_create_framebuffer (GskGLCommandQueue *self)
{
  GLuint fbo_id;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  glGenFramebuffers (1, &fbo_id);

  return fbo_id;
}

int
gsk_gl_command_queue_upload_texture (GskGLCommandQueue *self,
                                     GdkTexture        *texture,
                                     guint              x_offset,
                                     guint              y_offset,
                                     guint              width,
                                     guint              height,
                                     int                min_filter,
                                     int                mag_filter)
{
  G_GNUC_UNUSED gint64 start_time = GDK_PROFILER_CURRENT_TIME;
  cairo_surface_t *surface = NULL;
  GdkMemoryFormat data_format;
  const guchar *data;
  gsize data_stride;
  gsize bpp;
  int texture_id;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (!GDK_IS_GL_TEXTURE (texture));
  g_assert (x_offset + width <= gdk_texture_get_width (texture));
  g_assert (y_offset + height <= gdk_texture_get_height (texture));
  g_assert (min_filter == GL_LINEAR || min_filter == GL_NEAREST);
  g_assert (mag_filter == GL_LINEAR || min_filter == GL_NEAREST);

  if (width > self->max_texture_size || height > self->max_texture_size)
    {
      g_warning ("Attempt to create texture of size %ux%u but max size is %d. "
                 "Clipping will occur.",
                 width, height, self->max_texture_size);
      width = MAX (width, self->max_texture_size);
      height = MAX (height, self->max_texture_size);
    }

  texture_id = gsk_gl_command_queue_create_texture (self, width, height, min_filter, mag_filter);
  if (texture_id == -1)
    return texture_id;

  if (GDK_IS_MEMORY_TEXTURE (texture))
    {
      GdkMemoryTexture *memory_texture = GDK_MEMORY_TEXTURE (texture);
      data = gdk_memory_texture_get_data (memory_texture);
      data_format = gdk_memory_texture_get_format (memory_texture);
      data_stride = gdk_memory_texture_get_stride (memory_texture);
    }
  else
    {
      /* Fall back to downloading to a surface */
      surface = gdk_texture_download_surface (texture);
      cairo_surface_flush (surface);
      data = cairo_image_surface_get_data (surface);
      data_format = GDK_MEMORY_DEFAULT;
      data_stride = cairo_image_surface_get_stride (surface);
    }

  self->n_uploads++;

  bpp = gdk_memory_format_bytes_per_pixel (data_format);

  /* Swtich to texture0 as 2D. We'll restore it later. */
  glActiveTexture (GL_TEXTURE0);
  glBindTexture (GL_TEXTURE_2D, texture_id);

  gdk_gl_context_upload_texture (gdk_gl_context_get_current (),
                                 data + x_offset * bpp + y_offset * data_stride,
                                 width, height, data_stride,
                                 data_format, GL_TEXTURE_2D);

  /* Restore previous texture state if any */
  if (self->attachments->textures[0].id > 0)
    glBindTexture (self->attachments->textures[0].target,
                   self->attachments->textures[0].id);

  g_clear_pointer (&surface, cairo_surface_destroy);

  if (gdk_profiler_is_running ())
    gdk_profiler_add_markf (start_time, GDK_PROFILER_CURRENT_TIME-start_time,
                            "Upload Texture",
                            "Size %dx%d", width, height);

  return texture_id;
}

void
gsk_gl_command_queue_set_profiler (GskGLCommandQueue *self,
                                   GskProfiler       *profiler)
{
#ifdef G_ENABLE_DEBUG
  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (GSK_IS_PROFILER (profiler));

  if (g_set_object (&self->profiler, profiler))
    {
      self->gl_profiler = gsk_gl_profiler_new (self->context);

      self->metrics.n_frames = gsk_profiler_add_counter (profiler, "frames", "Frames", FALSE);
      self->metrics.cpu_time = gsk_profiler_add_timer (profiler, "cpu-time", "CPU Time", FALSE, TRUE);
      self->metrics.gpu_time = gsk_profiler_add_timer (profiler, "gpu-time", "GPU Time", FALSE, TRUE);

      self->metrics.n_binds = gdk_profiler_define_int_counter ("attachments", "Number of texture attachments");
      self->metrics.n_fbos = gdk_profiler_define_int_counter ("fbos", "Number of framebuffers attached");
      self->metrics.n_uniforms = gdk_profiler_define_int_counter ("uniforms", "Number of uniforms changed");
      self->metrics.n_uploads = gdk_profiler_define_int_counter ("uploads", "Number of texture uploads");
    }
#endif
}
