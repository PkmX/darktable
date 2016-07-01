/*
    This file is part of darktable,
    copyright (c) 2016 Chih-Mao Chen

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bauhaus/bauhaus.h"
#include "common/imageio_png.h"
#include "develop/imageop.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <libgen.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

DT_MODULE_INTROSPECTION(1, dt_iop_haldclut_params_t)

typedef struct dt_iop_haldclut_params_t
{
  char filepath[512];
} dt_iop_haldclut_params_t;

typedef struct dt_iop_haldclut_gui_data_t
{
  GtkWidget *filepath;
} dt_iop_haldclut_gui_data_t;

typedef struct dt_iop_haldclut_global_data_t
{
} dt_iop_haldclut_global_data_t;

const char *name()
{
  return _("hald clut");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int groups()
{
  return IOP_GROUP_COLOR;
}

// From `HaldCLUT_correct.c' by Eskil Steenberg (http://www.quelsolaar.com) (BSD licensed)
void correct_pixel(float *input, float *output, float *clut, unsigned int level)
{
  int color, red, green, blue, i, j;
  float tmp[6], r, g, b;
  level *= level;

  red = input[0] * (float)(level - 1);
  if(red > level - 2) red = (float)level - 2;
  if(red < 0) red = 0;

  green = input[1] * (float)(level - 1);
  if(green > level - 2) green = (float)level - 2;
  if(green < 0) green = 0;

  blue = input[2] * (float)(level - 1);
  if(blue > level - 2) blue = (float)level - 2;
  if(blue < 0) blue = 0;

  r = input[0] * (float)(level - 1) - red;
  g = input[1] * (float)(level - 1) - green;
  b = input[2] * (float)(level - 1) - blue;

  color = red + green * level + blue * level * level;

  i = color * 3;
  j = (color + 1) * 3;

  tmp[0] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[1] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[2] = clut[i] * (1 - r) + clut[j] * r;

  i = (color + level) * 3;
  j = (color + level + 1) * 3;

  tmp[3] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[4] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[5] = clut[i] * (1 - r) + clut[j] * r;

  output[0] = tmp[0] * (1 - g) + tmp[3] * g;
  output[1] = tmp[1] * (1 - g) + tmp[4] * g;
  output[2] = tmp[2] * (1 - g) + tmp[5] * g;

  i = (color + level * level) * 3;
  j = (color + level * level + 1) * 3;

  tmp[0] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[1] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[2] = clut[i] * (1 - r) + clut[j] * r;

  i = (color + level + level * level) * 3;
  j = (color + level + level * level + 1) * 3;

  tmp[3] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[4] = clut[i++] * (1 - r) + clut[j++] * r;
  tmp[5] = clut[i] * (1 - r) + clut[j] * r;

  tmp[0] = tmp[0] * (1 - g) + tmp[3] * g;
  tmp[1] = tmp[1] * (1 - g) + tmp[4] * g;
  tmp[2] = tmp[2] * (1 - g) + tmp[5] * g;

  output[0] = output[0] * (1 - b) + tmp[0] * b;
  output[1] = output[1] * (1 - b) + tmp[1] * b;
  output[2] = output[2] * (1 - b) + tmp[2] * b;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_haldclut_params_t *params = (dt_iop_haldclut_params_t *)piece->data;

  const int ch = piece->colors;

  dt_imageio_png_t png;
  if(read_header(params->filepath, &png))
  {
    fprintf(stderr, "[haldclut] invalid header from `%s'\n", params->filepath);
    goto noop;
  }

  dt_print(DT_DEBUG_DEV, "[haldclut] png: width=%d, height=%d, color_type=%d, bit_depth=%d\n", png.width,
           png.height, png.color_type, png.bit_depth);

  unsigned int level = 2;
  while(level * level * level < png.width) ++level;
  if(level * level * level != png.width)
  {
    fprintf(stderr, "[haldclut] invalid level %d %d\n", level, png.width);
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    goto noop;
  }

  const size_t buf_size = (size_t)png.height * png_get_rowbytes(png.png_ptr, png.info_ptr);
  dt_print(DT_DEBUG_DEV, "[haldclut] allocating %zu bytes\n", buf_size);
  uint8_t *buf = dt_alloc_align(16, buf_size);
  if(!buf)
  {
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    fprintf(stderr, "[haldclut] error allocating buffer for lut\n");
    goto noop;
  }

  if(read_image(&png, (void *)buf))
  {
    dt_free_align(buf);
    fprintf(stderr, "[haldclut] could not read image `%s'\n", params->filepath);
    goto noop;
  }

  float *clut = dt_alloc_align(16, buf_size * sizeof(float));
  if(!clut)
  {
    dt_free_align(buf);
    fprintf(stderr, "[haldclut] error allocating buffer for lut\n");
    goto noop;
  }

  for(size_t i = 0; i < buf_size; ++i)
  {
    clut[i] = buf[i] / 256.0f;
  }

  dt_free_align(buf);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(clut, level)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    float *in = ((float *)i) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)o) + (size_t)ch * roi_out->width * j;
    for(int i = 0; i < roi_out->width; i++)
    {
      for(int c = 0; c < 3; ++c) in[c] = in[c] < 0.0f ? 0.0f : (in[c] > 1.0f ? 1.0f : in[c]);
      correct_pixel(in, out, clut, level);
      in += ch;
      out += ch;
    }
  }

  dt_free_align(clut);
  return;

noop:
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    float *in = ((float *)i) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)o) + (size_t)ch * roi_out->width * j;
    for(int i = 0; i < roi_out->width; i++)
    {
      for(int c = 0; c < 3; ++c) out[c] = in[c];
      in += ch;
      out += ch;
    }
  }
}

void reload_defaults(dt_iop_module_t *module)
{
}

void init(dt_iop_module_t *module)
{
  module->data = NULL;
  module->params = calloc(1, sizeof(dt_iop_haldclut_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_haldclut_params_t));
  module->default_enabled = 0;
  module->priority = 910; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_haldclut_params_t);
  module->gui_data = NULL;
  dt_iop_haldclut_params_t tmp = (dt_iop_haldclut_params_t){ { "" } };

  memcpy(module->params, &tmp, sizeof(dt_iop_haldclut_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_haldclut_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  module->data = malloc(sizeof(dt_iop_haldclut_global_data_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  free(module->data);
  module->data = NULL;
}

static void filepath_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_haldclut_params_t *p = (dt_iop_haldclut_params_t *)self->params;
  snprintf(p->filepath, sizeof(p->filepath), "%s", gtk_entry_get_text(GTK_ENTRY(w)));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void button_clicked(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_haldclut_gui_data_t *g = (dt_iop_haldclut_gui_data_t *)self->gui_data;
  dt_iop_haldclut_params_t *p = (dt_iop_haldclut_params_t *)self->params;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("select haldclut file"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN, _("_cancel"), GTK_RESPONSE_CANCEL,
      _("_select"), GTK_RESPONSE_ACCEPT, (char *)NULL);
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  if (strlen(p->filepath) == 0 || access(p->filepath, F_OK) == -1)
  {
    gchar* def_path = dt_conf_get_string("plugins/darkroom/haldclut/def_path");
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), def_path);
    g_free(def_path);
  }
  else
  {
    gtk_file_chooser_select_filename(GTK_FILE_CHOOSER(filechooser), p->filepath);
  }

  GtkFileFilter* filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_mime_type(filter, "image/png");
  gtk_file_filter_set_name(filter, _("hald cluts (png)"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *filepath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    gtk_entry_set_text(GTK_ENTRY(g->filepath), filepath);
    snprintf(p->filepath, sizeof(p->filepath), "%s", filepath);
    g_free(filepath);
  }
  gtk_widget_destroy(filechooser);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_haldclut_gui_data_t *g = (dt_iop_haldclut_gui_data_t *)self->gui_data;
  dt_iop_haldclut_params_t *p = (dt_iop_haldclut_params_t *)self->params;
  gtk_entry_set_text(GTK_ENTRY(g->filepath), p->filepath);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_haldclut_gui_data_t));
  dt_iop_haldclut_gui_data_t *g = (dt_iop_haldclut_gui_data_t *)self->gui_data;

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(8));
  g->filepath = gtk_entry_new();
  gtk_box_pack_start(GTK_BOX(hbox), g->filepath, TRUE, TRUE, 0);
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(g->filepath));
  g_signal_connect(G_OBJECT(g->filepath), "changed", G_CALLBACK(filepath_callback), self);

  GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_directory, CPF_DO_NOT_USE_BORDER);
  gtk_widget_set_size_request(button, DT_PIXEL_APPLY_DPI(18), DT_PIXEL_APPLY_DPI(18));
  gtk_widget_set_tooltip_text(button, _("select haldcult file"));
  gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), self);

  self->widget = hbox;
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_haldclut_gui_data_t *g = (dt_iop_haldclut_gui_data_t *)self->gui_data;
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(g->filepath));
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
