/*
 *  Copyright 2010 Thomas Bonfort
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#ifdef USE_SQLITE

#include "geocache.h"
#include <apr_strings.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sqlite3.h>

static char* _get_dbname(geocache_context *ctx, geocache_cache_sqlite *cache,
      geocache_tileset *tileset, geocache_grid *grid) {
   return apr_pstrcat(ctx->pool,cache->dbdir,"/",tileset->name,"#",grid->name,".db",NULL);
}

static const char* _get_tile_dimkey(geocache_context *ctx, geocache_tile *tile) {
   if(tile->dimensions) {
      const apr_array_header_t *elts = apr_table_elts(tile->dimensions);
      int i = elts->nelts;
      if(i>1) {
         char *key = "";
         while(i--) {
            apr_table_entry_t *entry = &(APR_ARRAY_IDX(elts,i,apr_table_entry_t));
            if(i) {
               key = apr_pstrcat(ctx->pool,key,entry->val,"#",NULL);
            } else {
               key = apr_pstrcat(ctx->pool,key,entry->val,NULL);
            }
         }
         return key;
      } else if(i){
         apr_table_entry_t *entry = &(APR_ARRAY_IDX(elts,0,apr_table_entry_t));
         return entry->val;
      } else {
         return "";
      }
   } else {
      return "";
   }
}


static sqlite3* _get_conn(geocache_context *ctx, geocache_tile* tile, int readonly) {
   sqlite3* handle;
   int flags;
   if(readonly) {
      flags = SQLITE_OPEN_READONLY;
   } else {
      flags = SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE;
   }
   char *dbfile = _get_dbname(ctx,(geocache_cache_sqlite*)tile->tileset->cache,tile->tileset,tile->grid_link->grid);
   int ret = sqlite3_open_v2(dbfile,&handle,flags,NULL);
   if(ret != SQLITE_OK) {
      ctx->set_error(ctx,500,"failed to connect to sqlite db %s: %s",dbfile,sqlite3_errmsg(handle));
      return NULL;
   }
   sqlite3_busy_timeout(handle,3000);
   return handle;
}


static int _geocache_cache_sqlite_has_tile(geocache_context *ctx, geocache_tile *tile) {
   char *sql;
   if(tile->dimensions) {
      sql = "SELECT 1 from tiles where x=? and y=? and z=? and dim=?";
   } else {
      sql = "SELECT 1 from tiles where x=? and y=? and z=?";
   }
   sqlite3* handle = _get_conn(ctx,tile,1);
   if(GC_HAS_ERROR(ctx)) {
      sqlite3_close(handle);
      return GEOCACHE_FALSE;
   }

   sqlite3_stmt *stmt;
   sqlite3_prepare(handle,sql,-1,&stmt,NULL);
   sqlite3_bind_int(stmt,1,tile->x);
   sqlite3_bind_int(stmt,2,tile->y);
   sqlite3_bind_int(stmt,3,tile->z);
   if(tile->dimensions) {
      const char *dim = _get_tile_dimkey(ctx,tile);
      sqlite3_bind_text(stmt,4,dim,-1,SQLITE_STATIC);
   }
   int ret = sqlite3_step(stmt);
   if(ret != SQLITE_DONE && ret != SQLITE_ROW) {
      ctx->set_error(ctx,500,"sqlite backend failed on has_tile: %s",sqlite3_errmsg(handle));
   }
   if(ret == SQLITE_DONE) {
      ret = GEOCACHE_FALSE;
   } else if(ret == SQLITE_ROW){
      ret = GEOCACHE_TRUE;
   }
   sqlite3_finalize(stmt);
   sqlite3_close(handle);
   return ret;
}

static void _geocache_cache_sqlite_delete(geocache_context *ctx, geocache_tile *tile) {
   sqlite3* handle = _get_conn(ctx,tile,0);
   GC_CHECK_ERROR(ctx);
   char *sql;
   if(tile->dimensions) {
    sql = "DELETE from tiles where x=? and y=? and z=? and dim=?";
   } else {
    sql = "DELETE from tiles where x=? and y=? and z=?";
   }
   sqlite3_stmt *stmt;
   sqlite3_prepare(handle,sql,-1,&stmt,NULL);
   sqlite3_bind_int(stmt,1,tile->x);
   sqlite3_bind_int(stmt,2,tile->y);
   sqlite3_bind_int(stmt,3,tile->z);
   if(tile->dimensions) {
      const char* dim = _get_tile_dimkey(ctx,tile);
      sqlite3_bind_text(stmt,4,dim,-1,SQLITE_STATIC);
   }
   int ret = sqlite3_step(stmt);
   if(ret != SQLITE_DONE && ret != SQLITE_ROW) {
      ctx->set_error(ctx,500,"sqlite backend failed on delete: %s",sqlite3_errmsg(handle));
   }
   sqlite3_finalize(stmt);
   sqlite3_close(handle);
}


static int _geocache_cache_sqlite_get(geocache_context *ctx, geocache_tile *tile) {
   sqlite3* handle = _get_conn(ctx,tile,1);
   if(GC_HAS_ERROR(ctx)) {
      sqlite3_close(handle);
      return GEOCACHE_FAILURE;
   }
   sqlite3_stmt *stmt;
   char *sql;
   if(tile->dimensions) {
      sql = "SELECT data from tiles where x=? and y=? and z=? and dim=?";
   } else {
      sql = "SELECT data from tiles where x=? and y=? and z=?";
   }
   sqlite3_prepare(handle,sql,-1,&stmt,NULL);
   sqlite3_bind_int(stmt,1,tile->x);
   sqlite3_bind_int(stmt,2,tile->y);
   sqlite3_bind_int(stmt,3,tile->z);
   if(tile->dimensions) {
      const char* dim = _get_tile_dimkey(ctx,tile);
      sqlite3_bind_text(stmt,4,dim,-1,SQLITE_STATIC);
   }
   int ret = sqlite3_step(stmt);
   if(ret!=SQLITE_DONE && ret != SQLITE_ROW) {
      ctx->set_error(ctx,500,"sqlite backend failed on get: %s",sqlite3_errmsg(handle));
      sqlite3_finalize(stmt);
      sqlite3_close(handle);
      return GEOCACHE_FAILURE;
   }
   if(ret == SQLITE_DONE) {
      sqlite3_finalize(stmt);
      sqlite3_close(handle);
      return GEOCACHE_CACHE_MISS;
   } else {
      const void *blob = sqlite3_column_blob(stmt,0);
      int size = sqlite3_column_bytes(stmt, 0);
      tile->data = geocache_buffer_create(size,ctx->pool);
      memcpy(tile->data->buf, blob,size);
      tile->data->size = size;
      sqlite3_finalize(stmt);
      sqlite3_close(handle);
      return GEOCACHE_SUCCESS;
   }
}

static void _geocache_cache_sqlite_set(geocache_context *ctx, geocache_tile *tile) {
   sqlite3* handle = _get_conn(ctx,tile,0);
   GC_CHECK_ERROR(ctx);
   sqlite3_stmt *stmt;
   char *sql;
   if(tile->dimensions) {
      sql = "insert into tiles(x,y,z,data,dim) values (?,?,?,?,?)";
   } else {
      sql = "insert into tiles(x,y,z,data) values (?,?,?,?)";
   }
   sqlite3_prepare(handle, sql,-1,&stmt,NULL);
   sqlite3_bind_int(stmt,1,tile->x);
   sqlite3_bind_int(stmt,2,tile->y);
   sqlite3_bind_int(stmt,3,tile->z);
   sqlite3_bind_blob(stmt,4,tile->data->buf, tile->data->size,SQLITE_STATIC);
   if(tile->dimensions) {
      const char* dim = _get_tile_dimkey(ctx,tile);
      sqlite3_bind_text(stmt,5,dim,-1,SQLITE_STATIC);
   }
   int ret = sqlite3_step(stmt);
   if(ret != SQLITE_DONE && ret != SQLITE_ROW) {
      ctx->set_error(ctx,500,"sqlite backend failed on set: %s",sqlite3_errmsg(handle));
   }
   sqlite3_finalize(stmt);
   sqlite3_close(handle);
}

static void _geocache_cache_sqlite_configuration_parse(geocache_context *ctx, ezxml_t node, geocache_cache *cache) {
   ezxml_t cur_node;
   geocache_cache_sqlite *dcache = (geocache_cache_sqlite*)cache;
   if ((cur_node = ezxml_child(node,"base")) != NULL) {
      dcache->dbdir = apr_pstrdup(ctx->pool,cur_node->txt);
   }
   if(!dcache->dbdir) {
      ctx->set_error(ctx,500,"sqlite cache \"%s\" is missing <base> directory",cache->name);
      return;
   }
}
   
/**
 * \private \memberof geocache_cache_sqlite
 */
static void _geocache_cache_sqlite_configuration_post_config(geocache_context *ctx,
      geocache_cache *cache, geocache_cfg *cfg) {
   sqlite3 *db;
   char *errmsg;

   apr_hash_index_t *tileindex_index = apr_hash_first(ctx->pool,cfg->tilesets);

   while(tileindex_index) {
      geocache_tileset *tileset;
      const void *key; apr_ssize_t keylen;
      apr_hash_this(tileindex_index,&key,&keylen,(void**)&tileset);
      if(!strcmp(tileset->cache->name,cache->name)) {
         int i;
         for(i=0;i<tileset->grid_links->nelts;i++) {
            geocache_grid_link *gridlink = APR_ARRAY_IDX(tileset->grid_links,i,geocache_grid_link*);
            geocache_grid *grid = gridlink->grid;
            char *dbname = _get_dbname(ctx,(geocache_cache_sqlite*)cache,tileset,grid);
            sqlite3_open(dbname, &db);
            sqlite3_exec(db, "create table if not exists tiles(x integer, y integer, z integer, data blob, ctime date, mtime date, atime date)", 0, 0, &errmsg);
            sqlite3_exec(db, "create index if not exists tilesidx on tiles (x,y,z)", 0, 0, &errmsg);
            if(tileset->dimensions) {
               /* the following would return an error if the column already exists, but we ignore it */
               sqlite3_exec(db, "alter table tiles add column dim text",0, 0, &errmsg);
               sqlite3_exec(db, "create index if not exists tilesdimidx on tiles (dim)", 0, 0, &errmsg);
            }
            sqlite3_close(db);
         }
      }
      tileindex_index = apr_hash_next(tileindex_index);
   }
}

/**
 * \brief creates and initializes a geocache_sqlite_cache
 */
geocache_cache* geocache_cache_sqlite_create(geocache_context *ctx) {
   geocache_cache_sqlite *cache = apr_pcalloc(ctx->pool,sizeof(geocache_cache_sqlite));
   if(!cache) {
      ctx->set_error(ctx, 500, "failed to allocate sqlite cache");
      return NULL;
   }
   cache->cache.metadata = apr_table_make(ctx->pool,3);
   cache->cache.type = GEOCACHE_CACHE_SQLITE;
   cache->cache.tile_delete = _geocache_cache_sqlite_delete;
   cache->cache.tile_get = _geocache_cache_sqlite_get;
   cache->cache.tile_exists = _geocache_cache_sqlite_has_tile;
   cache->cache.tile_set = _geocache_cache_sqlite_set;
   cache->cache.configuration_post_config = _geocache_cache_sqlite_configuration_post_config;
   cache->cache.configuration_parse = _geocache_cache_sqlite_configuration_parse;
   return (geocache_cache*)cache;
}

#endif

/* vim: ai ts=3 sts=3 et sw=3
*/