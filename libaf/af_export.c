/*
 * af_export.c - AmigaOS4 patched version using file I/O instead of mmap
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include "config.h"
#ifndef __amigaos4__
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif
#include "mp_msg.h"
#include "path.h"
#include "af.h"

#define DEF_SZ 512
#define SHARED_FILE "mplayer-af_export"
#define SIZE_HEADER (2 * sizeof(int) + sizeof(unsigned long long))

typedef struct af_export_s {
  unsigned long long count;
  void* buf[AF_NCH];
  int sz;
  int wi;
#ifdef __amigaos4__
  FILE* vis_file;
#else
  int fd;
  uint8_t *mmap_area;
#endif
  char* filename;
} af_export_t;

static int control(struct af_instance_s* af, int cmd, void* arg)
{
  af_export_t* s = af->setup;
  switch (cmd) {
  case AF_CONTROL_REINIT: {
    int i = 0;
#ifndef __amigaos4__
    int mapsize;
#endif
    free(s->buf[0]);
#ifdef __amigaos4__
    if (s->vis_file) { fclose(s->vis_file); s->vis_file = NULL; }
#else
    if (s->mmap_area)
      munmap(s->mmap_area, SIZE_HEADER + (af->data->bps * s->sz * af->data->nch));
    if (s->fd) close(s->fd);
#endif
    af->data->rate   = ((af_data_t*)arg)->rate;
    af->data->nch    = ((af_data_t*)arg)->nch;
    af->data->format = AF_FORMAT_S16_NE;
    af->data->bps    = 2;
    if (s->sz == 0) s->sz = DEF_SZ;
    s->buf[0] = calloc(s->sz * af->data->nch, af->data->bps);
    if (NULL == s->buf[0])
      mp_msg(MSGT_AFILTER, MSGL_FATAL, "[export] Out of memory\n");
    for (i = 1; i < af->data->nch; i++)
      s->buf[i] = (uint8_t *)s->buf[0] + i * s->sz * af->data->bps;
    mp_msg(MSGT_AFILTER, MSGL_INFO, "[export] Exporting to: %s\n", s->filename);
#ifdef __amigaos4__
    s->vis_file = fopen(s->filename, "wb");
    if (!s->vis_file) {
      mp_msg(MSGT_AFILTER, MSGL_FATAL, "[export] Cannot open: %s\n", s->filename);
      return AF_ERROR;
    }
    {
      int h1 = af->data->nch;
      int h2 = s->sz * af->data->bps * af->data->nch;
      unsigned long long h3 = 0;
      fwrite(&h1, sizeof(int), 1, s->vis_file);
      fwrite(&h2, sizeof(int), 1, s->vis_file);
      fwrite(&h3, sizeof(unsigned long long), 1, s->vis_file);
      fflush(s->vis_file);
    }
    mp_msg(MSGT_AFILTER, MSGL_INFO, "[export] AmigaOS4 ready: %s\n", s->filename);
#else
    s->fd = open(s->filename, O_RDWR | O_CREAT | O_TRUNC, 0640);
    if (s->fd < 0) {
      mp_msg(MSGT_AFILTER, MSGL_FATAL, "[export] Cannot open: %s\n", s->filename);
      return AF_ERROR;
    }
    mapsize = SIZE_HEADER + (af->data->bps * s->sz * af->data->nch);
    for (i = 0; i < mapsize; i++) { char null = 0; write(s->fd, &null, 1); }
    s->mmap_area = mmap(0, mapsize, PROT_READ|PROT_WRITE, MAP_SHARED, s->fd, 0);
    if (s->mmap_area == NULL)
      mp_msg(MSGT_AFILTER, MSGL_FATAL, "[export] Cannot mmap: %s\n", s->filename);
    *((int*)s->mmap_area) = af->data->nch;
    *((int*)s->mmap_area + 1) = s->sz * af->data->bps * af->data->nch;
    msync(s->mmap_area, mapsize, MS_ASYNC);
#endif
    return af_test_output(af, (af_data_t*)arg);
  }
  case AF_CONTROL_COMMAND_LINE: {
    int i = 0;
    char *str = arg;
    if (!str) { free(s->filename); s->filename = get_path(SHARED_FILE); return AF_OK; }
#ifdef __amigaos4__
    /* AmigaOS4: skip volume colon e.g. T: */
    while (str[i] && str[i] != ':') i++;
    if (str[i] == ':') i++;
    while (str[i] && str[i] != ':') i++;
#else
    while (str[i] && str[i] != ':') i++;
#endif
    free(s->filename);
    s->filename = calloc(i + 1, 1);
    memcpy(s->filename, str, i);
    s->filename[i] = 0;
    sscanf(str + i + 1, "%d", &s->sz);
    return af->control(af, AF_CONTROL_EXPORT_SZ | AF_CONTROL_SET, &s->sz);
  }
  case AF_CONTROL_EXPORT_SZ | AF_CONTROL_SET:
    s->sz = *(int *)arg;
    if (s->sz <= 0 || s->sz > 2048)
      mp_msg(MSGT_AFILTER, MSGL_ERR, "[export] Size must be 1-2048\n");
    return AF_OK;
  case AF_CONTROL_EXPORT_SZ | AF_CONTROL_GET:
    *(int*)arg = s->sz;
    return AF_OK;
  }
  return AF_UNKNOWN;
}

static void uninit(struct af_instance_s* af)
{
  free(af->data);
  af->data = NULL;
  if (af->setup) {
    af_export_t* s = af->setup;
    free(s->buf[0]);
#ifdef __amigaos4__
    if (s->vis_file) { fclose(s->vis_file); s->vis_file = NULL; }
#else
    if (s->mmap_area) munmap(s->mmap_area, sizeof(af_export_t));
    if (s->fd > -1) close(s->fd);
#endif
    free(s->filename);
    free(af->setup);
    af->setup = NULL;
  }
}

static af_data_t* play(struct af_instance_s* af, af_data_t* data)
{
  af_data_t*   c   = data;
  af_export_t* s   = af->setup;
  int16_t*     a   = c->audio;
  int          nch = c->nch;
  int          len = c->len / c->bps;
  int          sz  = s->sz;
  int          flag = 0;
  int          ch, i;

  for (ch = 0; ch < nch; ch++) {
    int wi = s->wi;
    int16_t* b = s->buf[ch];
    for (i = ch; i < len; i += nch) {
      b[wi++] = a[i];
      if (wi >= sz) { flag = 1; break; }
    }
    s->wi = wi % s->sz;
  }

  if (flag) {
#ifdef __amigaos4__
    if (s->vis_file) {
      int h1 = nch;
      int h2 = sz * c->bps * nch;
      s->count++;
      rewind(s->vis_file);
      fwrite(&h1, sizeof(int), 1, s->vis_file);
      fwrite(&h2, sizeof(int), 1, s->vis_file);
      fwrite(&s->count, sizeof(unsigned long long), 1, s->vis_file);
      fwrite(s->buf[0], c->bps * nch, sz, s->vis_file);
      fflush(s->vis_file);
    }
#else
    memcpy(s->mmap_area + SIZE_HEADER, s->buf[0], sz * c->bps * nch);
    s->count++;
    memcpy(s->mmap_area + SIZE_HEADER - sizeof(s->count), &s->count, sizeof(s->count));
#endif
  }
  return data;
}

static int af_open(af_instance_t* af)
{
  af->control = control;
  af->uninit  = uninit;
  af->play    = play;
  af->mul = 1;
  af->data  = calloc(1, sizeof(af_data_t));
  af->setup = calloc(1, sizeof(af_export_t));
  if (!af->data || !af->setup) return AF_ERROR;
  ((af_export_t *)af->setup)->filename = get_path(SHARED_FILE);
  return AF_OK;
}

const af_info_t af_info_export = {
  "Sound export filter",
  "export",
  "Anders; Gustavo Sverzut Barbieri",
  "",
  AF_FLAGS_REENTRANT,
  af_open
};
