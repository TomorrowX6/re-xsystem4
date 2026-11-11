/* Copyright (C) 2019 Nunuhara Cabbage <nunuhara@haniwa.technology>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://gnu.org/licenses/>.
 */

#ifndef SYSTEM4_SAVEDATA_H
#define SYSTEM4_SAVEDATA_H

#include <stdbool.h>
#include <stdio.h>
#include "cJSON.h"
#include "vm.h"

FILE *atomic_save_open(const char *path, const char *mode, char **tmp_path_out);
bool atomic_save_commit(FILE *fp, const char *path, char *tmp_path, bool keep_backup);
void atomic_save_abort(FILE *fp, char *tmp_path);

void json_load_page(struct page *page, cJSON *vars, bool call_dtors);

int save_json(const char *filename, cJSON *json);
cJSON *load_json(const char *filename);
int save_globals(const char *keyname, const char *filename, const char *group_name, int *n);
int load_globals(const char *keyname, const char *filename, const char *group_name, int *n);
int delete_save_file(const char *filename);
void savedata_flush_on_background(void);
void savedata_reset(void);

#endif /* SYSTEM4_SAVEDATA_H */
