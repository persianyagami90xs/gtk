/* gskgltexturepool.c
 *
 * Copyright 2020 Christian Hergert <chergert@redhat.com>
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version.
 *
 * This file is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <string.h>

#include <gdk/gdktextureprivate.h>

#include "gskgltexturepoolprivate.h"
#include "ninesliceprivate.h"

void
gsk_gl_texture_free (GskGLTexture *texture)
{
  if (texture != NULL)
    {
      g_assert (texture->width_link.prev == NULL);
      g_assert (texture->width_link.next == NULL);
      g_assert (texture->height_link.prev == NULL);
      g_assert (texture->height_link.next == NULL);

      if (texture->user)
        g_clear_pointer (&texture->user, gdk_texture_clear_render_data);

      if (texture->texture_id != 0)
        {
          glDeleteTextures (1, &texture->texture_id);
          texture->texture_id = 0;
        }

      for (guint i = 0; i < texture->n_slices; i++)
        {
          glDeleteTextures (1, &texture->slices[i].texture_id);
          texture->slices[i].texture_id = 0;
        }

      g_clear_pointer (&texture->slices, g_free);
      g_clear_pointer (&texture->nine_slice, g_free);

      g_slice_free (GskGLTexture, texture);
    }
}

void
gsk_gl_texture_pool_init (GskGLTexturePool *self)
{
  memset (self, 0, sizeof *self);
}

void
gsk_gl_texture_pool_clear (GskGLTexturePool *self)
{
  guint *texture_ids = g_newa (guint, self->by_width.length);
  guint i = 0;

  while (self->by_width.length > 0)
    {
      GskGLTexture *head = g_queue_peek_head (&self->by_width);

      g_queue_unlink (&self->by_width, &head->width_link);
      g_queue_unlink (&self->by_height, &head->height_link);

      texture_ids[i++] = head->texture_id;
      head->texture_id = 0;

      gsk_gl_texture_free (head);
    }

  g_assert (self->by_width.length == 0);
  g_assert (self->by_height.length == 0);

  if (i > 0)
    glDeleteTextures (i, texture_ids);
}

void
gsk_gl_texture_pool_put (GskGLTexturePool *self,
                         GskGLTexture     *texture)
{
  GList *sibling;

  g_assert (self != NULL);
  g_assert (texture != NULL);
  g_assert (texture->user == NULL);
  g_assert (texture->width_link.prev == NULL);
  g_assert (texture->width_link.next == NULL);
  g_assert (texture->width_link.data == texture);
  g_assert (texture->height_link.prev == NULL);
  g_assert (texture->height_link.next == NULL);
  g_assert (texture->height_link.data == texture);

  if (texture->permanent)
    {
      gsk_gl_texture_free (texture);
      return;
    }

  sibling = NULL;
  for (GList *iter = self->by_width.head;
       iter != NULL;
       iter = iter->next)
    {
      GskGLTexture *other = iter->data;

      if (other->width > texture->width ||
          (other->width == texture->width &&
           other->height > texture->height))
        break;

      sibling = iter;
    }

  g_queue_insert_after_link (&self->by_width, sibling, &texture->width_link);

  sibling = NULL;
  for (GList *iter = self->by_height.head;
       iter != NULL;
       iter = iter->next)
    {
      GskGLTexture *other = iter->data;

      if (other->height > texture->height ||
          (other->height == texture->height &&
           other->width > texture->width))
        break;

      sibling = iter;
    }

  g_queue_insert_after_link (&self->by_height, sibling, &texture->height_link);
}

GskGLTexture *
gsk_gl_texture_pool_get (GskGLTexturePool *self,
                         int               width,
                         int               height,
                         int               min_filter,
                         int               mag_filter,
                         gboolean          always_create)
{
  GskGLTexture *texture;

  if (always_create)
    goto create_texture;

  if (width >= height)
    {
      for (GList *iter = self->by_width.head;
           iter != NULL;
           iter = iter->next)
        {
          texture = iter->data;

          if (texture->width >= width &&
              texture->height >= height &&
              texture->min_filter == min_filter &&
              texture->mag_filter == mag_filter)
            {
              g_queue_unlink (&self->by_width, &texture->width_link);
              g_queue_unlink (&self->by_height, &texture->height_link);

              return texture;
            }
        }
    }
  else
    {
      for (GList *iter = self->by_height.head;
           iter != NULL;
           iter = iter->next)
        {
          texture = iter->data;

          if (texture->width >= width &&
              texture->height >= height &&
              texture->min_filter == min_filter &&
              texture->mag_filter == mag_filter)
            {
              g_queue_unlink (&self->by_width, &texture->width_link);
              g_queue_unlink (&self->by_height, &texture->height_link);

              return texture;
            }
        }
    }

create_texture:

  texture = g_slice_new0 (GskGLTexture);
  texture->width_link.data = texture;
  texture->height_link.data = texture;
  texture->min_filter = min_filter;
  texture->mag_filter = mag_filter;

  glGenTextures (1, &texture->texture_id);

  glActiveTexture (GL_TEXTURE0);
  glBindTexture (GL_TEXTURE_2D, texture->texture_id);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  if (gdk_gl_context_get_use_es (gdk_gl_context_get_current ()))
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  else
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

  glBindTexture (GL_TEXTURE_2D, 0);

  return texture;
}

GskGLTexture *
gsk_gl_texture_new (guint  texture_id,
                    int    width,
                    int    height,
                    int    min_filter,
                    int    mag_filter,
                    gint64 frame_id)
{
  GskGLTexture *texture;

  texture = g_slice_new0 (GskGLTexture);
  texture->texture_id = texture_id;
  texture->width_link.data = texture;
  texture->height_link.data = texture;
  texture->min_filter = min_filter;
  texture->mag_filter = mag_filter;
  texture->width = width;
  texture->height = height;
  texture->last_used_in_frame = frame_id;

  return texture;
}

const GskGLTextureNineSlice *
gsk_gl_texture_get_nine_slice (GskGLTexture         *texture,
                               const GskRoundedRect *outline,
                               float                 extra_pixels)
{
  g_assert (texture != NULL);
  g_assert (outline != NULL);

  if G_UNLIKELY (texture->nine_slice == NULL)
    {
      texture->nine_slice = g_new0 (GskGLTextureNineSlice, 9);

      nine_slice_rounded_rect (texture->nine_slice, outline);
      nine_slice_grow (texture->nine_slice, extra_pixels);
      nine_slice_to_texture_coords (texture->nine_slice, texture->width, texture->height);
    }

  return texture->nine_slice;
}
