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

#include <assert.h>
#include <pthread.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <inttypes.h>

#include "settings.h"

#include "tvheadend.h"
#include "service.h"
#include "v4l.h"
#include "parsers.h"
#include "notify.h"
#include "psi.h"
#include "channels.h"

#define CLEAR(x) memset (&(x), 0, sizeof (x))

struct v4l_adapter_queue v4l_adapters;

static void v4l_adapter_notify(v4l_adapter_t *va);

#if defined(ENABLE_LIBAVCODEC) && defined(ENABLE_LIBAVFORMAT)
#define STREAM_FRAME_RATE 25
/* 
 * add an audio output stream
 */
AVStream *add_audio_stream(AVFormatContext *oc, int codec_id) {
    AVCodecContext *c;
    AVStream *st;

    st = av_new_stream(oc, 1);
    if (!st) {
        fprintf(stderr, "Could not alloc stream\n");
        exit(1);
    }

    c = &st->codec;
    c->codec_id = codec_id;
    c->codec_type = CODEC_TYPE_AUDIO;

    /* put sample parameters */
    c->bit_rate = 64000;
    c->sample_rate = 44100;
    c->channels = 2;
    return st;
}

/* add a video output stream */
AVStream *add_video_stream(AVFormatContext *oc, int codec_id) {
    AVCodecContext *c;
    AVStream *st;

    st = av_new_stream(oc, 0);
    if (!st) {
        fprintf(stderr, "Could not alloc stream\n");
        exit(1);
    }
    
    c = &st->codec;
    c->codec_id = codec_id;
    c->codec_type = CODEC_TYPE_VIDEO;

    /* put sample parameters */
    c->bit_rate = 40000;
    /* resolution must be a multiple of two */
    c->width = 640;  
    c->height = 480;
    /* frames per second */
    c->frame_rate = STREAM_FRAME_RATE;  
    c->frame_rate_base = 1;
    c->gop_size = 12; /* emit one intra frame every twelve frames at most */
    if (c->codec_id == CODEC_ID_MPEG2VIDEO) {
        /* just for testing, we also add B frames */
        c->max_b_frames = 2;
    }
    if (c->codec_id == CODEC_ID_MPEG1VIDEO){
        /* needed to avoid using macroblocks in which some coeffs overflow 
           this doesnt happen with normal video, it just happens here as the 
           motion of the chroma plane doesnt match the luma plane */
        c->mb_decision=2;
    }
    // some formats want stream headers to be seperate
    if(!strcmp(oc->oformat->name, "mp4") || !strcmp(oc->oformat->name, "mov") || !strcmp(oc->oformat->name, "3gp"))
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;
    
    return st;
}
#endif

static int xioctl(int fd, int request, void* argp)
{
  int r;

  do r = ioctl(fd, request, argp);
  while (-1 == r && EINTR == errno);

  return r;
}

/**
 *
 */
static void
v4l_input(v4l_adapter_t *va)
{
  service_t *t = va->va_current_service;
  elementary_stream_t *st;
  struct v4l2_buffer vbuf;
  uint8_t *buf = NULL;
  uint8_t *ptr, *pkt;
  int len=0, l, r;


//#ifdef ENABLE_LIBAVCODEC
  if (!va->va_can_mpeg) {
//#else
//  if (0) {
//#endif
    tvhlog(LOG_DEBUG, "v4l", "IO: %d", va->io);
    switch(va->io) {
	case IO_METHOD_READ:
    		buf = (uint8_t *)malloc(va->va_frame_size);
    		len = read(va->va_fd, buf, va->va_frame_size);
     		break;
	case IO_METHOD_MMAP:
		CLEAR(vbuf);
		vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		vbuf.memory = V4L2_MEMORY_MMAP;
		
		if (-1 == xioctl(va->va_fd, VIDIOC_DQBUF, &vbuf)) {
			switch(errno) {
				case EAGAIN:
					//tvhlog(LOG_ERR, "v4l", "EAGAIN");
					return;
				case EIO:
				default:
					tvhlog(LOG_ERR, "v4l", "VIDIOC_DQBUF: %s (%d)", strerror(errno), errno);
					return;
			}
		}

		assert(vbuf.index < va->n_buffers);

		len = va->buffers[vbuf.index].length;
		buf = va->buffers[vbuf.index].start;
		tvhlog(LOG_DEBUG, "v4l", "v4l2 memory mapped thingy");
		break;
	case IO_METHOD_USERPTR:
		break;
    }
  //  tvhlog(LOG_DEBUG, "v4l", "v4l2 frame size: %d len: %d", va->va_frame_size, len);
  } else {
    buf = (uint8_t *)malloc(4000);

    len = read(va->va_fd, buf, 4000);
  }
  tvhlog(LOG_DEBUG, "v4l", "curlen: %d", len); 
  if(len < 1) {
      tvhlog(LOG_DEBUG, "v4l", "Errno: %s (%d)", strerror(errno), errno);
      free(buf);
      return;
  }

  tvhlog(LOG_DEBUG, "v4l", "Read %d bytes", len);

#ifdef ENABLE_LIBAVCODEC
  int outbuf_size = 100000;
  uint8_t *outbuf;
  outbuf = malloc(outbuf_size);
  int size = va->width * va->height;
  va->picture->data[0] = buf;
  va->picture->data[1] = va->picture->data[0] + size;
  va->picture->data[2] = va->picture->data[1] + size / 4;
  va->picture->linesize[0] = va->width;
  va->picture->linesize[1] = va->width / 2;
  va->picture->linesize[2] = va->width / 2;

  int out_size = avcodec_encode_video(va->c, outbuf, outbuf_size, va->picture);
  tvhlog(LOG_DEBUG, "v4l", "Encoded frame %d", out_size);
  ptr = outbuf;
#else
  ptr = buf;
#endif
  //goto theend;
  //ptr = buf;

  pthread_mutex_lock(&t->s_stream_mutex);

  service_set_streaming_status_flags(t, 
				       TSS_INPUT_HARDWARE | TSS_INPUT_SERVICE);

  while(len > 0) {
    tvhlog(LOG_INFO, "v4l", "Can MPEG: %d", va->va_can_mpeg);

    switch(va->va_startcode) {
    default:
      va->va_startcode = va->va_startcode << 8 | *ptr;
      va->va_lenlock = 0;
      ptr++; len--;
      continue;

    case 0x000001e0:
      st = t->s_video;
      break;
    case 0x000001c0:
      st = t->s_audio;
      break;
    }

    if(va->va_lenlock == 2) {
      l = st->es_buf_ps.sb_size;
      st->es_buf_ps.sb_data = pkt = realloc(st->es_buf_ps.sb_data, l);
      
      r = l - st->es_buf_ps.sb_ptr;
      if(r > len)
	r = len;
      memcpy(pkt + st->es_buf_ps.sb_ptr, ptr, r);
      
      ptr += r;
      len -= r;

      st->es_buf_ps.sb_ptr += r;
      if(st->es_buf_ps.sb_ptr == l) {

	service_set_streaming_status_flags(t, TSS_MUX_PACKETS);

	parse_mpeg_ps(t, st, pkt + 6, l - 6);

	st->es_buf_ps.sb_size = 0;
	va->va_startcode = 0;
      } else {
	assert(st->es_buf_ps.sb_ptr < l);
      }
      
    } else {
      st->es_buf_ps.sb_size = st->es_buf_ps.sb_size << 8 | *ptr;
      va->va_lenlock++;
      if(va->va_lenlock == 2) {
	st->es_buf_ps.sb_size += 6;
	st->es_buf_ps.sb_ptr = 6;
      }
      ptr++; len--;
    }
  }

  pthread_mutex_unlock(&t->s_stream_mutex);
//theend:
  switch(va->io) {
	case IO_METHOD_READ:
		free(buf);
		break;
	case IO_METHOD_MMAP:
		if (-1 == xioctl(va->va_fd, VIDIOC_QBUF, &vbuf)) {
			tvhlog(LOG_ERR, "v4l", "VIDIOC_QBUF failed");
		}
		break;
	default:
		break;
  }
}


/**
 *
 */
static void *
v4l_thread(void *aux)
{
  v4l_adapter_t *va = aux;
  fd_set fds;
  struct timeval tv;
  int r;

  while(1) {
    FD_ZERO(&fds);
    FD_SET(va->va_pipe[0], &fds);
    FD_SET(va->va_fd, &fds);

    tv.tv_sec = 2;
    tv.tv_usec = 0;

    r = select(va->va_fd + 1, &fds, NULL, NULL, &tv);
    if(r <= 0) {
      if (EINTR == errno)
        continue;

      if (r == -1) {
        tvhlog(LOG_ALERT, "v4l", "%s: select() error %s, exiting",
	     va->va_path, strerror(errno));
        break;
      }
    }

    if (FD_ISSET(va->va_pipe[0], &fds)) { //(pfd[0].revents & POLLIN) {
      // Message on control pipe, used to exit thread, do so
      break;
    }

    if (FD_ISSET(va->va_fd, &fds)) { //(pfd[1].revents & POLLIN) { // || va->io == IO_METHOD_MMAP) {
      v4l_input(va);
    }
  }

  close(va->va_pipe[0]);
  return NULL;
}



/**
 *
 */
static int
v4l_service_start(service_t *t, unsigned int weight, int force_start)
{
  v4l_adapter_t *va = t->s_v4l_adapter;
  int frequency = t->s_v4l_frequency;
  struct v4l2_frequency vf;
  struct v4l2_format fmt;
  struct v4l2_input input;
  struct v4l2_requestbuffers req;
  enum v4l2_buf_type type;
  int result;
  v4l2_std_id std = 0xff;
  int fd,min;
  char buff[10];
 
  va->width=640;
  va->height=480;
  va->io = IO_METHOD_MMAP;

  if(va->va_current_service != NULL)
    return 1; // Adapter busy

  fd = tvh_open(va->va_path, O_RDWR | O_NONBLOCK, 0);
  if(fd == -1) {
    tvhlog(LOG_ERR, "v4l",
	   "%s: Unable to open device: %s\n", va->va_path, 
	   strerror(errno));
    return -1;
  }

  if(!va->va_file) {

    result = xioctl(fd, VIDIOC_S_STD, &std);
    if(result < 0) {
      tvhlog(LOG_ERR, "v4l",
	     "%s: Unable to set PAL -- %s", va->va_path, strerror(errno));
      close(fd);
      return -1;
    }

    memset(&vf, 0, sizeof(vf));

    vf.tuner = 0;
    vf.type = V4L2_TUNER_ANALOG_TV;
    vf.frequency = (frequency * 16) / 1000000;
    result = xioctl(fd, VIDIOC_S_FREQUENCY, &vf);
    if(result < 0) {
      tvhlog(LOG_ERR, "v4l",
	     "%s: Unable to tune to %dHz", va->va_path, frequency);
      close(fd);
      return -1;
    }

    tvhlog(LOG_INFO, "v4l",
	   "%s: Tuned to %dHz", va->va_path, frequency);
  }
  if(pipe(va->va_pipe)) {
    tvhlog(LOG_ERR, "v4l",
	   "%s: Unable to create control pipe [%s]", va->va_path, strerror(errno));
    close(fd);
    return -1;
  }

  if (!va->va_can_mpeg) {
    CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = va->width;
    fmt.fmt.pix.height = va->height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

    if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt)) {
       tvhlog(LOG_ERR, "v4l", "Unable to set format to YUYV");
    }
    if (va->width != fmt.fmt.pix.width) {
	va->width = fmt.fmt.pix.width;
	tvhlog(LOG_WARNING, "v4l", "Image width set to %d by device", va->width);
    }
    if (va->height != fmt.fmt.pix.height) {
 	va->height = fmt.fmt.pix.height;
	tvhlog(LOG_WARNING, "v4l", "Image height set to %d by device", va->height);
    }
    /* buggy driver paranoia */
    min = fmt.fmt.pix.width * 2;
    if (fmt.fmt.pix.bytesperline < min)
	fmt.fmt.pix.bytesperline = min;
    min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
    if (fmt.fmt.pix.sizeimage < min)
   	fmt.fmt.pix.sizeimage = min;
    va->va_frame_size = fmt.fmt.pix.sizeimage;

#ifdef ENABLE_LIBAVCODEC
    va->codec = NULL;
    va->c = NULL;
  
    avcodec_init();
    avcodec_register_all();

    //va->fmt = 
    va->oc = av_alloc_format_context();

    va->codec = avcodec_find_encoder(CODEC_ID_MPEG2VIDEO);
    if (!va->codec) {
	tvhlog(LOG_ERR, "v4l", "Failed to initialize mpeg2 encoder");
	close(fd);
	return -1;
    }

    va->c = avcodec_alloc_context();
    va->picture = avcodec_alloc_frame();
    va->c->bit_rate = 40000; // Bitrate
    va->c->width = va->width;
    va->c->height = va->height;
    va->c->time_base = (AVRational){1,25};
    va->c->gop_size = 10;
    va->c->max_b_frames = 1;
    va->c->pix_fmt = PIX_FMT_YUV420P;

    if (avcodec_open(va->c, va->codec) < 0) {
	tvhlog(LOG_ERR, "v4l", "Failed to open codec");
	close(fd);
	return -1;
    }
#endif
    int index = 0;
    if (-1 == xioctl(fd, VIDIOC_S_INPUT, &index)) {
	tvhlog(LOG_ERR, "v4l", "Failed to set input to 0");
    }
    if (-1 == xioctl(fd, VIDIOC_G_INPUT, &index)) {
        tvhlog(LOG_ERR, "v4l", "Failed to get input selection");
    }
    CLEAR(input);
    input.index = index;
    if (-1 == xioctl(fd, VIDIOC_ENUMINPUT, &input)) {
        tvhlog(LOG_ERR, "v4l", "Failed to get input details");
    }
    tvhlog(LOG_DEBUG, "v4l", "Selected input: %d (%s)", index, input.name);
   
    switch(va->io) {
	case IO_METHOD_READ:
	case IO_METHOD_USERPTR:
		break;
	case IO_METHOD_MMAP:
		CLEAR(req);
		req.count = 4;
		req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		req.memory = V4L2_MEMORY_MMAP;
		if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
			if (EINVAL == errno)
				tvhlog(LOG_ERR, "v4l", "device does not support memory mapping");
			else {
				tvhlog(LOG_ERR, "v4l", "VIDIOC_REQBUFS failed");
				close(fd);
				return -1;
			}
		}

		if (req.count < 2) {
			tvhlog(LOG_ERR, "v4l", "Insufficient buffer memory");
			close(fd);
			return -1;
		}

		va->buffers = calloc(req.count, sizeof(*va->buffers));
		if (!va->buffers) {
			tvhlog(LOG_ERR, "v4l", "Out of memory");
			close(fd);
			return -1;
		}
		
		for(va->n_buffers = 0;va->n_buffers < req.count;++va->n_buffers) {
			struct v4l2_buffer buf;
			CLEAR(buf);
			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_MMAP;
			buf.index = va->n_buffers;
			if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf)) {
				tvhlog(LOG_ERR, "v4l", "Failed to VIDEOC_QUERYBUF");
				close(fd);
				return -1;
			}
			va->buffers[va->n_buffers].length = buf.length;
			va->buffers[va->n_buffers].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
			if (MAP_FAILED == va->buffers[va->n_buffers].start) { 
				tvhlog(LOG_ERR, "v4l", "mmap failed");
				close(fd);
				return -1;
			}
			CLEAR(buf);
			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_MMAP;
			buf.index = va->n_buffers;

			if (-1 == xioctl(fd, VIDIOC_QBUF, &buf)) {
                        	tvhlog(LOG_ERR, "v4l", "VIDIOC_QBUF failed");
				close(fd);
				return -1;
            		}
		}
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (-1 == xioctl(fd, VIDIOC_STREAMON, &type)) {
			tvhlog(LOG_ERR, "v4l", "Failed to set streaming on");
			close(fd);
			return -1;
		}

		tvhlog(LOG_DEBUG, "v4l", "INPUT_MMAP initialized");
		break;
    }

    /* Start capture by reading 0 bytes from device */
    result = read(fd, buff, 0);
	
  }

  va->va_fd = fd;
  va->va_current_service = t;
  pthread_create(&va->va_thread, NULL, v4l_thread, va);
  v4l_adapter_notify(va);
  return 0;
}


/**
 *
 */
static void
v4l_service_refresh(service_t *t)
{

}


/**
 *
 */
static void
v4l_service_stop(service_t *t)
{
  char c = 'q';
  enum v4l2_buf_type type;
  int i = 0;
  v4l_adapter_t *va = t->s_v4l_adapter;

  assert(va->va_current_service != NULL);

  if(va->io == IO_METHOD_MMAP) {
	for(i=0;i<va->n_buffers;i++) {
		if (-1 == munmap(va->buffers[i].start, va->buffers[i].length))
			tvhlog(LOG_ERR, "v4l", "munmap failed");
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == xioctl(va->va_fd, VIDIOC_STREAMOFF, &type)) {
		tvhlog(LOG_ERR, "v4l", "VIDIOC_STREAMOFF error");
	}

	tvhlog(LOG_DEBUG, "v4l", "INPUT_MMAP stopped");
  }

  if(tvh_write(va->va_pipe[1], &c, 1))
    tvhlog(LOG_ERR, "v4l", "Unable to close video thread -- %s",
	   strerror(errno));
  
  pthread_join(va->va_thread, NULL);

  close(va->va_pipe[1]);
  close(va->va_fd);

  va->va_current_service = NULL;
  v4l_adapter_notify(va);
}


/**
 *
 */
static void
v4l_service_save(service_t *t)
{
  v4l_adapter_t *va = t->s_v4l_adapter;
  htsmsg_t *m = htsmsg_create_map();

  htsmsg_add_u32(m, "frequency", t->s_v4l_frequency);
  
  if(t->s_ch != NULL) {
    htsmsg_add_str(m, "channelname", t->s_ch->ch_name);
    htsmsg_add_u32(m, "mapped", 1);
  }
  

  pthread_mutex_lock(&t->s_stream_mutex);
  psi_save_service_settings(m, t);
  pthread_mutex_unlock(&t->s_stream_mutex);
  
  hts_settings_save(m, "v4lservices/%s/%s",
		    va->va_identifier, t->s_identifier);

  htsmsg_destroy(m);
}


/**
 *
 */
static int
v4l_service_quality(service_t *t)
{
  return 100;
}

/**
 *
 */
static int
v4l_service_is_enabled(service_t *t)
{
  return t->s_enabled;
}


/**
 *
 */
static int
v4l_grace_period(service_t *t)
{
  return 5;
}


/**
 * Generate a descriptive name for the source
 */
static void
v4l_service_setsourceinfo(service_t *t, struct source_info *si)
{
  char buf[64];
  memset(si, 0, sizeof(struct source_info));

  si->si_type = S_MPEG_PS;
  si->si_adapter = strdup(t->s_v4l_adapter->va_displayname);

  snprintf(buf, sizeof(buf), "%d Hz", t->s_v4l_frequency);
  si->si_mux = strdup(buf);
}


/**
 *
 */
service_t *
v4l_service_find(v4l_adapter_t *va, const char *id, int create)
{
  service_t *t;
  char buf[200];

  int vaidlen = strlen(va->va_identifier);

  if(id != NULL) {

    if(strncmp(id, va->va_identifier, vaidlen))
      return NULL;

    LIST_FOREACH(t, &va->va_services, s_group_link)
      if(!strcmp(t->s_identifier, id))
	return t;
  }

  if(create == 0)
    return NULL;
  
  if(id == NULL) {
    va->va_tally++;
    snprintf(buf, sizeof(buf), "%s_%d", va->va_identifier, va->va_tally);
    id = buf;
  } else {
    va->va_tally = MAX(atoi(id + vaidlen + 1), va->va_tally);
  }

  t = service_create(id, SERVICE_TYPE_V4L, 0);

  t->s_start_feed    = v4l_service_start;
  t->s_refresh_feed  = v4l_service_refresh;
  t->s_stop_feed     = v4l_service_stop;
  t->s_config_save   = v4l_service_save;
  t->s_setsourceinfo = v4l_service_setsourceinfo;
  t->s_quality_index = v4l_service_quality;
  t->s_is_enabled    = v4l_service_is_enabled;
  t->s_grace_period  = v4l_grace_period;
  t->s_iptv_fd = -1;
  t->s_v4l_adapter = va;

  pthread_mutex_lock(&t->s_stream_mutex); 
  service_make_nicename(t);
  t->s_video = service_stream_create(t, -1, SCT_MPEG2VIDEO); 
  t->s_audio = service_stream_create(t, -1, SCT_MPEG2AUDIO); 
  pthread_mutex_unlock(&t->s_stream_mutex); 

  LIST_INSERT_HEAD(&va->va_services, t, s_group_link);

  return t;
}


/**
 *
 */
static void
v4l_adapter_add(const char *path, const char *displayname, 
		const char *devicename, int file, int can_mpeg)
{
  v4l_adapter_t *va;
  int i, r;

  va = calloc(1, sizeof(v4l_adapter_t));

  va->va_identifier = strdup(path);

  r = strlen(va->va_identifier);
  for(i = 0; i < r; i++)
    if(!isalnum((int)va->va_identifier[i]))
      va->va_identifier[i] = '_';

  va->va_displayname = strdup(displayname);
  va->va_path = path ? strdup(path) : NULL;
  va->va_devicename = devicename ? strdup(devicename) : NULL;
  va->va_file = file;
  va->va_can_mpeg = can_mpeg;

  TAILQ_INSERT_TAIL(&v4l_adapters, va, va_global_link);
}


/**
 *
 */
static void
v4l_adapter_check(const char *path, int fd)
{
  int r, i;

  char devicename[100];

  struct v4l2_capability caps;

  r = xioctl(fd, VIDIOC_QUERYCAP, &caps);

  if(r) {
    tvhlog(LOG_WARNING, "v4l", 
	   "%s: Can not query capabilities, device skipped", path);
    return;
  }

  tvhlog(LOG_INFO, "v4l", "%s: %s %s %s capabilities: 0x%08x",
	 path, caps.driver, caps.card, caps.bus_info, caps.capabilities);

  if(!(caps.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
    tvhlog(LOG_WARNING, "v4l", 
	   "%s: Device is not a video capture device, device skipped", path);
    return;
  }

  if (!(caps.capabilities & V4L2_CAP_READWRITE)) {
    tvhlog(LOG_WARNING, "v4l", "%s: Device does not support read i/o", path);
  }
  if (!(caps.capabilities & V4L2_CAP_STREAMING)) {
    tvhlog(LOG_WARNING, "v4l", "%s: Device does not support streaming i/o", path);
  }
  /* Enum video standards */

  for(i = 0;; i++) {
    struct v4l2_standard standard;
    memset(&standard, 0, sizeof(standard));
    standard.index = i;

    if(xioctl(fd, VIDIOC_ENUMSTD, &standard))
      break;

    tvhlog(LOG_INFO, "v4l", 
	   "%s: Standard #%d: %016llx %s, frameperiod: %d/%d, %d lines",
	   path,
	   standard.index, 
	   standard.id,
	   standard.name,
	   standard.frameperiod.numerator,
	   standard.frameperiod.denominator,
	   standard.framelines);
  }

  /* Enum video inputs */

  for(i = 0;; i++) {
    struct v4l2_input input;
    memset(&input, 0, sizeof(input));
    input.index = i;
    
    if(xioctl(fd, VIDIOC_ENUMINPUT, &input))
      break;

    const char *type;
    switch(input.type) {
    case V4L2_INPUT_TYPE_TUNER:
      type = "Tuner";
      break;
    case V4L2_INPUT_TYPE_CAMERA:
      type = "Camera";
      break;
    default:
      type = "Unknown";
      break;
    }

    int f = input.status;

    tvhlog(LOG_INFO, "v4l", 
	   "%s: Input #%d: %s (%s), audio:0x%x, tuner:%d, standard:%016llx, "
	   "%s%s%s",
	   path,
	   input.index,
	   input.name,
	   type,
	   input.audioset,
	   input.tuner,
	   input.std,
	   f & V4L2_IN_ST_NO_POWER  ? "[No power] " : "",
	   f & V4L2_IN_ST_NO_SIGNAL ? "[No signal] " : "",
	   f & V4L2_IN_ST_NO_COLOR  ? "[No color] " : "");
  }

  int can_mpeg = 0;

  /* Enum formats */
  for(i = 0;; i++) {

    struct v4l2_fmtdesc fmtdesc;
    memset(&fmtdesc, 0, sizeof(fmtdesc));
    fmtdesc.index = i;
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if(xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc))
      break;

    tvhlog(LOG_INFO, "v4l", 
	   "%s: Format #%d: %s [%.4s] %s",
	   path,
	   fmtdesc.index,
	   fmtdesc.description,
	   (char*)&fmtdesc.pixelformat,
	   fmtdesc.flags & V4L2_FMT_FLAG_COMPRESSED ? "(compressed)" : "");

    if(fmtdesc.pixelformat == V4L2_PIX_FMT_MPEG)
      can_mpeg = 1;
  }


  if(!(caps.capabilities & V4L2_CAP_TUNER)) {
    tvhlog(LOG_WARNING, "v4l", 
	   "%s: Device does not have a tuner, device skipped", path);
    return;
  }

  if(!can_mpeg) {
    tvhlog(LOG_WARNING, "v4l", 
	   "%s: Device lacks MPEG encoder, soft encoding needs to be used", path);
  }


  snprintf(devicename, sizeof(devicename), "%s %s %s",
	   caps.card, caps.driver, caps.bus_info);

  tvhlog(LOG_INFO, "v4l",
	 "%s: Using adapter", devicename);

  v4l_adapter_add(path, devicename, devicename, 0,can_mpeg);
}


/**
 *
 */
static void 
v4l_adapter_probe(const char *path)
{
  int fd;

  fd = tvh_open(path, O_RDWR | O_NONBLOCK, 0);

  if(fd == -1) {
    if(errno != ENOENT)
      tvhlog(LOG_ALERT, "v4l",
	     "Unable to open %s -- %s", path, strerror(errno));
    return;
  }

  v4l_adapter_check(path, fd);

  close(fd);
}





/**
 * Save config for the given adapter
 */
static void
v4l_adapter_save(v4l_adapter_t *va)
{
  htsmsg_t *m = htsmsg_create_map();

  lock_assert(&global_lock);

  htsmsg_add_str(m, "displayname", va->va_displayname);
  htsmsg_add_u32(m, "logging", va->va_logging);
  hts_settings_save(m, "v4ladapters/%s", va->va_identifier);
  htsmsg_destroy(m);
}


/**
 *
 */
htsmsg_t *
v4l_adapter_build_msg(v4l_adapter_t *va)
{
  htsmsg_t *m = htsmsg_create_map();

  htsmsg_add_str(m, "identifier", va->va_identifier);
  htsmsg_add_str(m, "name", va->va_displayname);
  htsmsg_add_str(m, "type", "v4l");

  if(va->va_path)
    htsmsg_add_str(m, "path", va->va_path);

  if(va->va_devicename)
    htsmsg_add_str(m, "devicename", va->va_devicename);

  if(va->va_current_service != NULL) {
    char buf[100];
    snprintf(buf, sizeof(buf), "%d Hz", 
	     va->va_current_service->s_v4l_frequency);
    htsmsg_add_str(m, "currentMux", buf);
  } else {
    htsmsg_add_str(m, "currentMux", "- inactive -");
  }

  return m;
}


/**
 *
 */
static void
v4l_adapter_notify(v4l_adapter_t *va)
{
  notify_by_msg("tvAdapter", v4l_adapter_build_msg(va));
}


/**
 *
 */
v4l_adapter_t *
v4l_adapter_find_by_identifier(const char *identifier)
{
  v4l_adapter_t *va;
  
  TAILQ_FOREACH(va, &v4l_adapters, va_global_link)
    if(!strcmp(identifier, va->va_identifier))
      return va;
  return NULL;
}


/**
 *
 */
void
v4l_adapter_set_displayname(v4l_adapter_t *va, const char *name)
{
  lock_assert(&global_lock);

  if(!strcmp(name, va->va_displayname))
    return;

  tvhlog(LOG_NOTICE, "v4l", "Adapter \"%s\" renamed to \"%s\"",
	 va->va_displayname, name);

  tvh_str_set(&va->va_displayname, name);
  v4l_adapter_save(va);
  v4l_adapter_notify(va);
}


/**
 *
 */
void
v4l_adapter_set_logging(v4l_adapter_t *va, int on)
{
  if(va->va_logging == on)
    return;

  lock_assert(&global_lock);

  tvhlog(LOG_NOTICE, "v4l", "Adapter \"%s\" detailed logging set to: %s",
	 va->va_displayname, on ? "On" : "Off");

  va->va_logging = on;
  v4l_adapter_save(va);
  v4l_adapter_notify(va);
}



/**
 *
 */
static void
v4l_service_create_by_msg(v4l_adapter_t *va, htsmsg_t *c, const char *name)
{
  const char *s;
  unsigned int u32;

  service_t *t = v4l_service_find(va, name, 1);

  if(t == NULL)
    return;

  s = htsmsg_get_str(c, "channelname");
  if(htsmsg_get_u32(c, "mapped", &u32))
    u32 = 0;
  
  if(!htsmsg_get_u32(c, "frequency", &u32))
    t->s_v4l_frequency = u32;

  if(s && u32)
    service_map_channel(t, channel_find_by_name(s, 1, 0), 0);
}

/**
 *
 */
static void
v4l_service_load(v4l_adapter_t *va)
{
  htsmsg_t *l, *c;
  htsmsg_field_t *f;

  if((l = hts_settings_load("v4lservices/%s", va->va_identifier)) == NULL)
    return;
 
  HTSMSG_FOREACH(f, l) {
    if((c = htsmsg_get_map_by_field(f)) == NULL)
      continue;
    
    v4l_service_create_by_msg(va, c, f->hmf_name);
  }
  htsmsg_destroy(l);
}

/**
 *
 */
void
v4l_init(void)
{
  htsmsg_t *l, *c;
  htsmsg_field_t *f;
  char buf[256];
  int i;
  v4l_adapter_t *va;

  TAILQ_INIT(&v4l_adapters);
  
  for(i = 0; i < 8; i++) {
    snprintf(buf, sizeof(buf), "/dev/video%d", i);
    v4l_adapter_probe(buf);
  }

  l = hts_settings_load("v4ladapters");
  if(l != NULL) {
    HTSMSG_FOREACH(f, l) {
      if((c = htsmsg_get_map_by_field(f)) == NULL)
	continue;

      if((va = v4l_adapter_find_by_identifier(f->hmf_name)) == NULL) {
	/* Not discovered by hardware, create it */

	va = calloc(1, sizeof(v4l_adapter_t));
	va->va_identifier = strdup(f->hmf_name);
	va->va_path = NULL;
	va->va_devicename = NULL;

     }

      tvh_str_update(&va->va_displayname, htsmsg_get_str(c, "displayname"));
      htsmsg_get_u32(c, "logging", &va->va_logging);


      va->buffers = NULL;
      va->n_buffers = 0;
    }
    htsmsg_destroy(l);
  }

  TAILQ_FOREACH(va, &v4l_adapters, va_global_link)
    v4l_service_load(va);
}
