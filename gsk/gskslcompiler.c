/* GTK - The GIMP Toolkit
 *   
 * Copyright © 2017 Benjamin Otte <otte@gnome.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "gskslcompilerprivate.h"

#include "gskcodesourceprivate.h"
#include "gsksldefineprivate.h"
#include "gskslpreprocessorprivate.h"
#include "gskslprogramprivate.h"
#include "gsksltokenizerprivate.h"

struct _GskSlCompiler {
  GObject parent_instance;

  GHashTable *defines;
};

G_DEFINE_QUARK (gsk-sl-compiler-error-quark, gsk_sl_compiler_error)
G_DEFINE_QUARK (gsk-sl-compiler-warning-quark, gsk_sl_compiler_warning)

G_DEFINE_TYPE (GskSlCompiler, gsk_sl_compiler, G_TYPE_OBJECT)

static void
gsk_sl_compiler_dispose (GObject *object)
{
  GskSlCompiler *compiler = GSK_SL_COMPILER (object);

  g_hash_table_destroy (compiler->defines);

  G_OBJECT_CLASS (gsk_sl_compiler_parent_class)->dispose (object);
}

static void
gsk_sl_compiler_class_init (GskSlCompilerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gsk_sl_compiler_dispose;
}

static void
gsk_sl_compiler_init (GskSlCompiler *compiler)
{
  compiler->defines = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             NULL, (GDestroyNotify) gsk_sl_define_unref);
}

GskSlCompiler *
gsk_sl_compiler_new (void)
{
  return g_object_new (GSK_TYPE_SL_COMPILER, NULL);
}

static void
gsk_sl_compiler_add_define_error_func (GskSlTokenizer        *parser,
                                       gboolean               fatal,
                                       const GskCodeLocation *location,
                                       const GskSlToken      *token,
                                       const GError          *error,
                                       gpointer               user_data)
{
  GError **real_error = user_data;

  if (!fatal)
    return;

  if (*real_error)
    return;

  *real_error = g_error_copy (error);
  g_prefix_error (real_error,
                  "%3zu:%2zu: ",
                  location->lines + 1,
                  location->line_bytes);
}

gboolean
gsk_sl_compiler_add_define (GskSlCompiler  *compiler,
                            const char     *name,
                            const char     *definition,
                            GError        **error)
{
  GskSlTokenizer *tokenizer;
  GskSlDefine *define;
  GskCodeLocation location;
  GskSlToken token = { 0, };
  GError *real_error = NULL;
  GskCodeSource *source;
  GBytes *bytes;

  g_return_val_if_fail (GSK_IS_SL_COMPILER (compiler), FALSE);
  g_return_val_if_fail (name != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!gsk_sl_string_is_valid_identifier (name))
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Define name \"%s\" is not a valid identifier", name);
      return FALSE;
    }

  if (definition == NULL)
    definition = "1";

  define = gsk_sl_define_new (name, NULL);
  bytes = g_bytes_new_static (definition, strlen (definition));
  source = gsk_code_source_new_for_bytes ("<define>", bytes);
  tokenizer = gsk_sl_tokenizer_new (source,
                                    gsk_sl_compiler_add_define_error_func,
                                    &real_error,
                                    NULL);

  while (TRUE)
    {
      do
        {
          gsk_sl_token_clear (&token);
          location = *gsk_sl_tokenizer_get_location (tokenizer);
          gsk_sl_tokenizer_read_token (tokenizer, &token);
        }
      while (gsk_sl_token_is_skipped (&token));

      if (gsk_sl_token_is (&token, GSK_SL_TOKEN_EOF))
        break;

      gsk_sl_define_add_token (define, &location, &token);
    }

  gsk_sl_token_clear (&token);
  gsk_sl_tokenizer_unref (tokenizer);
  g_bytes_unref (bytes);
  g_object_unref (source);

  if (real_error == NULL)
    {
      g_hash_table_replace (compiler->defines, (gpointer) gsk_sl_define_get_name (define), define);
      return TRUE;
    }
  else
    {
      gsk_sl_define_unref (define);
      g_propagate_error (error, real_error);
      return FALSE;
    }
}

void
gsk_sl_compiler_remove_define (GskSlCompiler *compiler,
                               const char    *name)
{
  g_return_if_fail (GSK_IS_SL_COMPILER (compiler));
  g_return_if_fail (name != NULL);

  g_hash_table_remove (compiler->defines, name);
}

GHashTable *
gsk_sl_compiler_copy_defines (GskSlCompiler *compiler)
{
  GHashTable *copy;
  GHashTableIter iter;
  gpointer key, value;

  copy = g_hash_table_new_full (g_str_hash, g_str_equal,
                                NULL, (GDestroyNotify) gsk_sl_define_unref);

  g_hash_table_iter_init (&iter, compiler->defines);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      g_hash_table_replace (copy, key, gsk_sl_define_ref (value));
    }

  return copy;
}

GskCodeSource *
gsk_sl_compiler_resolve_include (GskSlCompiler  *compiler,
                                 GskCodeSource  *source,
                                 gboolean        local,
                                 const char     *name,
                                 GError        **error)
{
  GskCodeSource *result;
  GBytes *bytes;

  if (local)
    {
      GFile *source_file = gsk_code_source_get_file (source);
      if (source_file)
        {
          GFile *parent, *file;

          parent = g_file_get_parent (source_file);
          file = g_file_resolve_relative_path (parent, name);
          result = gsk_code_source_new_for_file (file);
          g_object_unref (parent);
          g_object_unref (file);
          bytes = gsk_code_source_load (result, error);
          if (bytes)
            {
              g_bytes_unref (bytes);
              return result;
            }

          g_object_unref (result);
        }
    }
  else
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_EXIST,
                   "Could not resolve \"%s\" in search path.", name);
    }

  return NULL;
}

static GskSlProgram *
gsk_sl_compiler_compile (GskSlCompiler *compiler,
                         GskCodeSource *source)
{
  GskSlPreprocessor *preproc;
  GskSlProgram *program;

  program = g_object_new (GSK_TYPE_SL_PROGRAM, NULL);

  preproc = gsk_sl_preprocessor_new (compiler, source);

  gsk_sl_program_parse (program, preproc);

  if (gsk_sl_preprocessor_has_fatal_error (preproc))
    {
      g_object_unref (program);
      program = NULL;
    }

  gsk_sl_preprocessor_unref (preproc);

  return program;
}

GskSlProgram *
gsk_sl_compiler_compile_file (GskSlCompiler *compiler,
                              GFile         *file)
{
  GskSlProgram *program;
  GskCodeSource *source;

  g_return_val_if_fail (GSK_IS_SL_COMPILER (compiler), NULL);
  g_return_val_if_fail (G_IS_FILE (file), NULL);

  source = gsk_code_source_new_for_file (file);

  program = gsk_sl_compiler_compile (compiler, source);

  g_object_unref (source);

  return program;
}

GskSlProgram *
gsk_sl_compiler_compile_bytes (GskSlCompiler *compiler,
                               GBytes        *bytes)
{
  GskSlProgram *program;
  GskCodeSource *source;

  g_return_val_if_fail (GSK_IS_SL_COMPILER (compiler), NULL);
  g_return_val_if_fail (bytes != NULL, NULL);

  source = gsk_code_source_new_for_bytes ("<program>", bytes);

  program = gsk_sl_compiler_compile (compiler, source);

  g_object_unref (source);

  return program;
}