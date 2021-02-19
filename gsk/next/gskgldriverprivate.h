/* gskgldriverprivate.h
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

#ifndef __GSK_GL_DRIVER_PRIVATE_H__
#define __GSK_GL_DRIVER_PRIVATE_H__

#include <gdk/gdkgltextureprivate.h>

#include "gskgltypesprivate.h"
#include "gskgltexturepoolprivate.h"

G_BEGIN_DECLS

enum {
  UNIFORM_SHARED_ALPHA,
  UNIFORM_SHARED_SOURCE,
  UNIFORM_SHARED_CLIP_RECT,
  UNIFORM_SHARED_VIEWPORT,
  UNIFORM_SHARED_PROJECTION,
  UNIFORM_SHARED_MODELVIEW,

  UNIFORM_SHARED_LAST
};

enum {
  UNIFORM_CUSTOM_SIZE = UNIFORM_SHARED_LAST,
  UNIFORM_CUSTOM_TEXTURE1,
  UNIFORM_CUSTOM_TEXTURE2,
  UNIFORM_CUSTOM_TEXTURE3,
  UNIFORM_CUSTOM_TEXTURE4,

  UNIFORM_CUSTOM_LAST
};

typedef struct {
  gconstpointer   pointer;
  float           scale_x;
  float           scale_y;
  int             filter;
  int             pointer_is_child;
  graphene_rect_t parent_rect; /* Valid when pointer_is_child */
} GskTextureKey;

#define GSL_GK_NO_UNIFORMS UNIFORM_INVALID_##__COUNTER__
#define GSK_GL_ADD_UNIFORM(pos, KEY, name) UNIFORM_##KEY = UNIFORM_SHARED_LAST + pos,
#define GSK_GL_DEFINE_PROGRAM(name, resource, uniforms) enum { uniforms };
# include "gskglprograms.defs"
#undef GSK_GL_DEFINE_PROGRAM
#undef GSK_GL_ADD_UNIFORM
#undef GSL_GK_NO_UNIFORMS

#define GSK_TYPE_NEXT_DRIVER (gsk_next_driver_get_type())

G_DECLARE_FINAL_TYPE (GskNextDriver, gsk_next_driver, GSK, NEXT_DRIVER, GObject)

struct _GskGLRenderTarget
{
  guint framebuffer_id;
  guint texture_id;
  int min_filter;
  int mag_filter;
  int width;
  int height;
};

struct _GskNextDriver
{
  GObject parent_instance;

  GskGLCommandQueue *shared_command_queue;
  GskGLCommandQueue *command_queue;

  GskGLTexturePool texture_pool;

  GskGLGlyphLibrary *glyphs;
  GskGLIconLibrary *icons;
  GskGLShadowLibrary *shadows;

  GHashTable *textures;
  GHashTable *key_to_texture_id;
  GHashTable *texture_id_to_key;

  GPtrArray *atlases;

  GHashTable *shader_cache;

  GArray *autorelease_framebuffers;
  GPtrArray *render_targets;

#define GSK_GL_NO_UNIFORMS
#define GSK_GL_ADD_UNIFORM(pos, KEY, name)
#define GSK_GL_DEFINE_PROGRAM(name, resource, uniforms) GskGLProgram *name;
# include "gskglprograms.defs"
#undef GSK_GL_NO_UNIFORMS
#undef GSK_GL_ADD_UNIFORM
#undef GSK_GL_DEFINE_PROGRAM

  gint64 current_frame_id;

  /* Used to reduce number of comparisons */
  guint stamps[UNIFORM_SHARED_LAST];

  guint debug : 1;
  guint in_frame : 1;
};

GskNextDriver     *gsk_next_driver_from_shared_context   (GdkGLContext         *context,
                                                          gboolean              debug_shaders,
                                                          GError              **error);
GskGLCommandQueue *gsk_next_driver_create_command_queue  (GskNextDriver        *self,
                                                          GdkGLContext         *context);
GdkGLContext      *gsk_next_driver_get_context           (GskNextDriver        *self);
gboolean           gsk_next_driver_create_render_target  (GskNextDriver        *self,
                                                          int                   width,
                                                          int                   height,
                                                          int                   min_filter,
                                                          int                   mag_filter,
                                                          GskGLRenderTarget   **render_target);
guint              gsk_next_driver_release_render_target (GskNextDriver        *self,
                                                          GskGLRenderTarget    *render_target,
                                                          gboolean              release_texture);
void               gsk_next_driver_begin_frame           (GskNextDriver        *self,
                                                          GskGLCommandQueue    *command_queue);
void               gsk_next_driver_end_frame             (GskNextDriver        *self);
void               gsk_next_driver_after_frame           (GskNextDriver        *self);
GdkTexture        *gsk_next_driver_create_gdk_texture    (GskNextDriver        *self,
                                                          guint                 texture_id);
void               gsk_next_driver_cache_texture         (GskNextDriver        *self,
                                                          const GskTextureKey  *key,
                                                          guint                 texture_id);
guint              gsk_next_driver_load_texture          (GskNextDriver        *self,
                                                          GdkTexture           *texture,
                                                          int                   min_filter,
                                                          int                   mag_filter);
GskGLTexture      *gsk_next_driver_create_texture        (GskNextDriver        *self,
                                                          float                 width,
                                                          float                 height,
                                                          int                   min_filter,
                                                          int                   mag_filter);
GskGLTexture      *gsk_next_driver_acquire_texture       (GskNextDriver        *self,
                                                          float                 width,
                                                          float                 height,
                                                          int                   min_filter,
                                                          int                   mag_filter);
void               gsk_next_driver_release_texture       (GskNextDriver        *self,
                                                          GskGLTexture         *texture);
void               gsk_next_driver_release_texture_by_id (GskNextDriver        *self,
                                                          guint                 texture_id);
GskGLTexture      *gsk_next_driver_mark_texture_permanent(GskNextDriver        *self,
                                                          guint                 texture_id);
void               gsk_next_driver_add_texture_slices    (GskNextDriver        *self,
                                                          GdkTexture           *texture,
                                                          GskGLTextureSlice   **out_slices,
                                                          guint                *out_n_slices);
GskGLProgram      *gsk_next_driver_lookup_shader         (GskNextDriver        *self,
                                                          GskGLShader          *shader,
                                                          GError              **error);
GskGLTextureAtlas *gsk_next_driver_create_atlas          (GskNextDriver        *self);
void              gsk_next_driver_save_atlases_to_png    (GskNextDriver        *self,
                                                          const char           *directory);

static inline GskGLTexture *
gsk_next_driver_get_texture_by_id (GskNextDriver *self,
                                   guint          texture_id)
{
  return g_hash_table_lookup (self->textures, GUINT_TO_POINTER (texture_id));
}

/**
 * gsk_next_driver_lookup_texture:
 * @self: a #GskNextDriver
 * @key: the key for the texture
 *
 * Looks up a texture in the texture cache by @key.
 *
 * If the texture could not be found, then zero is returned.
 *
 * Returns: a positive integer if the texture was found; otherwise 0.
 */
static inline guint
gsk_next_driver_lookup_texture (GskNextDriver       *self,
                                const GskTextureKey *key)
{
  gpointer id;

  if (g_hash_table_lookup_extended (self->key_to_texture_id, key, NULL, &id))
    {
      GskGLTexture *texture = g_hash_table_lookup (self->textures, id);

      if (texture != NULL)
        texture->last_used_in_frame = self->current_frame_id;

      return GPOINTER_TO_UINT (id);
    }

  return 0;
}

static inline void
gsk_next_driver_slice_texture (GskNextDriver      *self,
                               GdkTexture         *texture,
                               GskGLTextureSlice **out_slices,
                               guint              *out_n_slices)
{
  GskGLTexture *t;

  if ((t = gdk_texture_get_render_data (texture, self)))
    {
      *out_slices = t->slices;
      *out_n_slices = t->n_slices;
      return;
    }

  gsk_next_driver_add_texture_slices (self, texture, out_slices, out_n_slices);
}

G_END_DECLS

#endif /* __GSK_GL_DRIVER_PRIVATE_H__ */
