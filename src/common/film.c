/*
   This file is part of darktable,
   copyright (c) 2009--2010 johannes hanika.
   copyright (c) 2011-2012 henrik andersson.

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
#include "common/film.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/dtpthread.h"
#include "common/image_cache.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "views/view.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef USE_LUA
#include "lua/glist.h"
#include "lua/lua.h"
#endif

void dt_film_init(dt_film_t *film)
{
  dt_pthread_mutex_init(&film->images_mutex, NULL);
  film->last_loaded = film->num_images = 0;
  film->dirname[0] = '\0';
  film->dir = NULL;
  film->id = -1;
  film->ref = 0;
}

void dt_film_cleanup(dt_film_t *film)
{
  dt_pthread_mutex_destroy(&film->images_mutex);
  if(film->dir)
  {
    g_dir_close(film->dir);
    film->dir = NULL;
  }
}

void dt_film_set_query(const int32_t id)
{
  /* enable film id filter and set film id */
  dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
  dt_conf_set_int("plugins/lighttable/collect/item0", 0);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select id, folder from film_rolls where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_conf_set_string("plugins/lighttable/collect/string0", (gchar *)sqlite3_column_text(stmt, 1));
  }
  sqlite3_finalize(stmt);
  dt_collection_update_query(darktable.collection);
}

/** open film with given id. */
int dt_film_open2(dt_film_t *film)
{
  /* check if we got a decent film id */
  if(film->id < 0) return 1;

  /* query database for id and folder */
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select id, folder from film_rolls where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film->id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    /* fill out the film dirname */
    snprintf(film->dirname, sizeof(film->dirname), "%s", (gchar *)sqlite3_column_text(stmt, 1));
    sqlite3_finalize(stmt);
    char datetime[20];
    dt_gettime(datetime, sizeof(datetime));

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "update film_rolls set datetime_accessed = ?1 where id = ?2", -1, &stmt,
                                NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, datetime, -1, SQLITE_STATIC);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, film->id);
    sqlite3_step(stmt);

    sqlite3_finalize(stmt);
    dt_film_set_query(film->id);
    dt_control_queue_redraw_center();
    dt_view_manager_reset(darktable.view_manager);
    return 0;
  }
  else
    sqlite3_finalize(stmt);

  /* failure */
  return 1;
}

int dt_film_open(const int32_t id)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select id, folder from film_rolls where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    char datetime[20];
    dt_gettime(datetime, sizeof(datetime));

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "update film_rolls set datetime_accessed = ?1 where id = ?2", -1, &stmt,
                                NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, datetime, -1, SQLITE_STATIC);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, id);
    sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
  // TODO: prefetch to cache using image_open
  dt_film_set_query(id);
  dt_control_queue_redraw_center();
  dt_view_manager_reset(darktable.view_manager);
  return 0;
}

// FIXME: needs a rewrite
int dt_film_open_recent(const int num)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select id from film_rolls order by datetime_accessed desc limit ?1,1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, num);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if(dt_film_open(id)) return 1;
    char datetime[20];
    dt_gettime(datetime, sizeof(datetime));
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "update film_rolls set datetime_accessed = ?1 where id = ?2", -1, &stmt,
                                NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, datetime, -1, SQLITE_STATIC);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, id);
    sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
  // dt_control_update_recent_films();
  return 0;
}

int dt_film_new(dt_film_t *film, const char *directory)
{
  // Try open filmroll for folder if exists
  film->id = -1;
  int rc;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from film_rolls where folder = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, directory, -1, SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW) film->id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  if(film->id <= 0)
  {
    // create a new filmroll
    char datetime[20];
    dt_gettime(datetime, sizeof(datetime));
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "insert into film_rolls (id, datetime_accessed, folder) "
                                "values (null, ?1, ?2)",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, datetime, -1, SQLITE_STATIC);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, directory, -1, SQLITE_STATIC);
    dt_pthread_mutex_lock(&darktable.db_insert);
    rc = sqlite3_step(stmt);
    if(rc != SQLITE_DONE)
      fprintf(stderr, "[film_new] failed to insert film roll! %s\n",
              sqlite3_errmsg(dt_database_get(darktable.db)));
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from film_rolls where folder=?1",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, directory, -1, SQLITE_STATIC);
    if(sqlite3_step(stmt) == SQLITE_ROW) film->id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    dt_pthread_mutex_unlock(&darktable.db_insert);
  }

  if(film->id <= 0) return 0;
  g_strlcpy(film->dirname, directory, sizeof(film->dirname));
  film->last_loaded = 0;
  return film->id;
}

int dt_film_import(const char *dirname)
{
  int rc;
  sqlite3_stmt *stmt;

  /* initialize a film object*/
  dt_film_t *film = (dt_film_t *)malloc(sizeof(dt_film_t));
  dt_film_init(film);
  film->id = -1;

  /* lookup if film exists and reuse id */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from film_rolls where folder = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, dirname, -1, SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW) film->id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  /* if we didn't find a id, lets instansiate a new filmroll */
  if(film->id <= 0)
  {
    char datetime[20];
    dt_gettime(datetime, sizeof(datetime));
    /* insert a new film roll into database */
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "insert into film_rolls (id, datetime_accessed, folder) values "
                                "(null, ?1, ?2)",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, datetime, -1, SQLITE_STATIC);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, dirname, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if(rc != SQLITE_DONE)
      fprintf(stderr, "[film_import] failed to insert film roll! %s\n",
              sqlite3_errmsg(dt_database_get(darktable.db)));
    sqlite3_finalize(stmt);

    /* requery for filmroll and fetch new id */
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from film_rolls where folder=?1",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, dirname, -1, SQLITE_STATIC);
    if(sqlite3_step(stmt) == SQLITE_ROW) film->id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }

  /* bail out if we got troubles */
  if(film->id <= 0)
  {
    // if the film is empty => remove it again.
    if(dt_film_is_empty(film->id))
    {
      dt_film_remove(film->id);
    }
    dt_film_cleanup(film);
    free(film);
    return 0;
  }

  /* at last put import film job on queue */
  film->last_loaded = 0;
  g_strlcpy(film->dirname, dirname, sizeof(film->dirname));
  film->dir = g_dir_open(film->dirname, 0, NULL);
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG, dt_film_import1_create(film));

  return film->id;
}

void dt_film_remove_empty()
{
  // remove all empty film rolls from db:
  gboolean raise_signal = FALSE;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT id,folder FROM film_rolls AS B WHERE "
                                                             "(SELECT COUNT(A.id) FROM images AS A WHERE "
                                                             "A.film_id=B.id)=0",
                              -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    sqlite3_stmt *inner_stmt;
    raise_signal = TRUE;
    const gint id = sqlite3_column_int(stmt, 0);
    const gchar *folder = (const gchar *)sqlite3_column_text(stmt, 1);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM film_rolls WHERE id=?1", -1,
                                &inner_stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(inner_stmt, 1, id);
    sqlite3_step(inner_stmt);
    sqlite3_finalize(inner_stmt);

    if(dt_util_is_dir_empty(folder)) rmdir(folder);
  }
  sqlite3_finalize(stmt);
  if(raise_signal) dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_REMOVED);
}

int dt_film_is_empty(const int id)
{
  int empty = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images where film_id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  if(sqlite3_step(stmt) != SQLITE_ROW) empty = 1;
  sqlite3_finalize(stmt);
  return empty;
}

// This is basically the same as dt_image_remove() from common/image.c.
// It just does the iteration over all images in the SQL statement
void dt_film_remove(const int id)
{
  // only allowed if local copies have their original accessible

  sqlite3_stmt *stmt;

  gboolean remove_ok = TRUE;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT id FROM images WHERE film_id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    if(!dt_image_safe_remove(imgid))
    {
      remove_ok = FALSE;
      break;
    }
  }
  sqlite3_finalize(stmt);

  if(!remove_ok)
  {
    dt_control_log(_("cannot remove film roll having local copies with non accessible originals"));
    return;
  }

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from tagged_images where imgid in "
                                                             "(select id from images where film_id = ?1)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from history where imgid in "
                                                             "(select id from images where film_id = ?1)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from color_labels where imgid in "
                                                             "(select id from images where film_id = ?1)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from meta_data where id in "
                                                             "(select id from images where film_id = ?1)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from selected_images where imgid in "
                                                             "(select id from images where film_id = ?1)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images where film_id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const uint32_t imgid = sqlite3_column_int(stmt, 0);
    dt_image_local_copy_reset(imgid);
    dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
    dt_image_cache_remove(darktable.image_cache, imgid);
  }
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from images where id in "
                                                             "(select id from images where film_id = ?1)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from film_rolls where id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  // dt_control_update_recent_films();
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
