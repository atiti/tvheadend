/*
 *  TV Input - Linux analogue (v4lv2) interface
 *  Copyright (C) 2007 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef V4L_H_
#define V4L_H_

#define __user
#include <linux/videodev2.h>

#if defined(ENABLE_LIBAVCODEC) && defined(ENABLE_LIBAVFORMAT) 
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#endif


typedef enum {
        IO_METHOD_READ,
        IO_METHOD_MMAP,
        IO_METHOD_USERPTR,
} io_method;

struct buffer {
	void *start;
	size_t length;
};

LIST_HEAD(v4l_adapter_list, v4l_adapter);
TAILQ_HEAD(v4l_adapter_queue, v4l_adapter);


extern struct v4l_adapter_queue v4l_adapters;

typedef struct v4l_adapter {

  TAILQ_ENTRY(v4l_adapter) va_global_link;

  char *va_path;

  char *va_identifier;

  char *va_displayname;

  char *va_devicename;

  int va_file;

  uint32_t va_logging;

  int va_can_mpeg;

  int va_frame_size;

  unsigned int n_buffers;

  struct buffer *buffers;
 
  io_method io;

  int width;
  int height;

#ifdef ENABLE_LIBAVCODEC
  AVCodec *codec;
  AVCodecContext *c;
  AVFrame *picture;
  uint8_t *picture_buf;
  AVOutputFormat *fmt;
  AVFormatContext *oc;
  AVStream *audio_st, *video_st;
  double audio_pts, video_pts;

#endif


  //  struct v4l2_capability va_caps;

  struct service *va_current_service;

  struct service_list va_services;
  int va_tally;

  /** Receiver thread stuff */

  int va_fd;

  pthread_t va_thread;

  int va_pipe[2];


  /** Mpeg stream parsing */
  uint32_t va_startcode;
  int va_lenlock;

} v4l_adapter_t;


v4l_adapter_t *v4l_adapter_find_by_identifier(const char *identifier);

void v4l_adapter_set_displayname(v4l_adapter_t *va, const char *name);

void v4l_adapter_set_logging(v4l_adapter_t *va, int on);

htsmsg_t *v4l_adapter_build_msg(v4l_adapter_t *va);

service_t *v4l_service_find(v4l_adapter_t *va, const char *id, 
			    int create);

void v4l_init(void);

#endif /* V4L_H */
