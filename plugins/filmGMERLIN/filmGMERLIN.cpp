////////////////////////////////////////////////////////
//
// GEM - Graphics Environment for Multimedia
//
// zmoelnig@iem.kug.ac.at
//
// Implementation file
//
//    Copyright (c) 1997-1999 Mark Danks.
//    Copyright (c) Günther Geiger.
//    Copyright (c) 2001-2011 IOhannes m zmölnig. forum::für::umläute. IEM. zmoelnig@iem.at
//    For information on usage and redistribution, and for a DISCLAIMER OF ALL
//    WARRANTIES, see the file, "GEM.LICENSE.TERMS" in this distribution.
//
/////////////////////////////////////////////////////////
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef HAVE_LIBGMERLIN_AVDEC
# define HAVE_GMERLIN
#endif


#ifdef HAVE_GMERLIN
#include "filmGMERLIN.h"
#include "plugins/PluginFactory.h"
#include "Gem/RTE.h"
#include "Gem/Properties.h"


//#define GEM_FILMGMERLIN_TRACKSWITCH 1

using namespace gem::plugins;

REGISTER_FILMFACTORY("gmerlin", filmGMERLIN);

/////////////////////////////////////////////////////////
//
// filmGMERLIN
//
/////////////////////////////////////////////////////////
// Constructor
//
/////////////////////////////////////////////////////////

filmGMERLIN :: filmGMERLIN(void) :
  m_numFrames(-1), m_numTracks(-1),
  m_file(NULL),
  m_opt(NULL),
  m_track(0), m_stream(0),
  m_gframe(NULL),
  m_finalframe(NULL),
  m_gconverter(NULL),
  m_fps(0.), m_fps_num(1), m_fps_denum(1),
  m_next_timestamp(0),
#ifdef USE_FRAMETABLE
  m_frametable(NULL),
#endif
  m_doConvert(false)
{
  m_gconverter=gavl_video_converter_create ();
}

////////////////////////////////////////////////////////
// Destructor
//
/////////////////////////////////////////////////////////
filmGMERLIN :: ~filmGMERLIN(void)
{
  close();
  if(m_gconverter)gavl_video_converter_destroy(m_gconverter);m_gconverter=NULL;
}

void filmGMERLIN :: close(void)
{
  if(m_file)bgav_close(m_file);m_file=NULL;
#ifdef USE_FRAMETABLE
  if(m_frametable)gavl_frame_table_destroy(m_frametable); m_frametable=NULL;
#endif
  /* LATER: free frame buffers */

}

/////////////////////////////////////////////////////////
// logging
//
/////////////////////////////////////////////////////////
void filmGMERLIN::log(bgav_log_level_t level, const char *log_domain, const char *message)
{
  switch(level) {
  case BGAV_LOG_DEBUG:
    verbose(1, "[GEM:filmGMERLIN:%s] %s", log_domain, message);
    break;
  case BGAV_LOG_INFO:
    verbose(0, "[GEM:filmGMERLIN:%s] %s", log_domain, message);
    break;
  case BGAV_LOG_WARNING:
    verbose(0, "[GEM:filmGMERLIN:%s] %s", log_domain, message);
    break;
  case BGAV_LOG_ERROR:
    error("[GEM:filmGMERLIN:%s!] %s", log_domain, message);
    break;
  default:break;
  }
}
void filmGMERLIN::log_callback (void *data, bgav_log_level_t level, const char *log_domain, const char *message)
{
  ((filmGMERLIN*)(data))->log(level, log_domain, message);
}

bool filmGMERLIN :: isThreadable(void) {
  if(m_numFrames<0)
    return false;
  return true;
}

/////////////////////////////////////////////////////////
// really open the file !
//
/////////////////////////////////////////////////////////
bool filmGMERLIN :: open(const std::string sfilename, const gem::Properties&wantProps)
{
  close();

  m_track=0;

  m_file = bgav_create();
  if(!m_file) return false;
  m_opt = bgav_get_options(m_file);
  if(!m_opt) return false;

  /*
    bgav_options_set_connect_timeout(m_opt,   connect_timeout);
    bgav_options_set_read_timeout(m_opt,      read_timeout);
    bgav_options_set_network_bandwidth(m_opt, network_bandwidth);
  */
  bgav_options_set_seek_subtitles(m_opt, 0);
  bgav_options_set_sample_accurate(m_opt, 1);

  bgav_options_set_log_callback(m_opt,
                                log_callback,
                                this);

  const char*filename=sfilename.c_str();

  if(!strncmp(filename, "vcd://", 6))
    {
      if(!bgav_open_vcd(m_file, filename + 5))
        {
          verbose(0, "[GEM:filmGMERLIN] Could not open VCD Device %s",  filename + 5);
          return false;
        }
    }
  else if(!strncmp(filename, "dvd://", 6))
    {
      if(!bgav_open_dvd(m_file, filename + 5))
        {
          verbose(0, "[GEM:filmGMERLIN] Could not open DVD Device %s", filename + 5);
          return false;
        }
    }
  else if(!strncmp(filename, "dvb://", 6))
    {
      if(!bgav_open_dvb(m_file, filename + 6))
        {
          verbose(0, "[GEM:filmGMERLIN] Could not open DVB Device %s", filename + 6);
          return false;
        }
    }
  else {
    if(!bgav_open(m_file, filename)) {
      verbose(0, "[GEM:filmGMERLIN] Could not open file %s", filename);
      close();

      return false;
    }
  }
  if(bgav_is_redirector(m_file))
    {
      int i=0;
      int num_urls=bgav_redirector_get_num_urls(m_file);
      verbose(1, "[GEM:filmGMERLIN] Found redirector:");
      for(i = 0; i < num_urls; i++)
        {
          verbose(1, "[GEM:filmGMERLIN] #%d: '%s' -> %s", i, bgav_redirector_get_name(m_file, i), bgav_redirector_get_url(m_file, i));
        }
      for(i = 0; i < num_urls; i++) {
        filename=(char*)bgav_redirector_get_url(m_file, i);
        close();
        if (open(filename, wantProps)) {
          return true;
        }
      }
      /* couldn't open a redirected stream */
      return false;
    }

  /*
   * ok, we have been able to open the "file"
   * now get some information from it...
   */
  m_numTracks = bgav_num_tracks(m_file);
  // LATER: check whether this track has a video-stream...
  int numvstreams=bgav_num_video_streams (m_file, m_track);
  if(numvstreams) {
    bgav_select_track(m_file, m_track);
  } else {
    verbose(1, "[GEM:filmGMERLIN] track %d does not contain a video-stream: skipping");
  }

  bgav_set_video_stream(m_file, m_stream, BGAV_STREAM_DECODE);
  if(!bgav_start(m_file)) {
    close();
    return false;
  }
  m_next_timestamp=bgav_video_start_time(m_file, m_track);

  gavl_video_format_t*gformat = (gavl_video_format_t*)bgav_get_video_format (m_file, m_stream);
  m_gframe = gavl_video_frame_create_nopad(gformat);


  gavl_video_format_t finalformat[1];
  finalformat->frame_width = gformat->frame_width;
  finalformat->frame_height = gformat->frame_height;
  finalformat->image_width = gformat->image_width;
  finalformat->image_height = gformat->image_height;
  finalformat->pixel_width = gformat->pixel_width;
  finalformat->pixel_height = gformat->pixel_height;
  finalformat->frame_duration = gformat->frame_duration;
  finalformat->timescale = gformat->timescale;

#ifdef __APPLE__
  /* GAVL_UYVY definitely works on PowerPC
   * m.e.grimm also reported that he had to use UYVY,
   * i assume he is on x86_64;
   *  so make it the default for now
   */
  finalformat->pixelformat=GAVL_UYVY;
  /* don't know what this is exactly: */
  //finalformat->pixelformat=GAVL_YUY2;
#else
  finalformat->pixelformat=GAVL_RGBA_32;
#endif
	
  m_finalframe = gavl_video_frame_create_nopad(finalformat);
  m_doConvert= (gavl_video_converter_init (m_gconverter, gformat, finalformat)>0);
  m_image.image.xsize=gformat->frame_width;
  m_image.image.ysize=gformat->frame_height;
#ifdef __APPLE__
  m_image.image.setCsizeByFormat(GL_YUV422_GEM);
#else
  m_image.image.setCsizeByFormat(GL_RGBA);
#endif
  m_image.image.notowned=true;
  m_image.image.upsidedown=true;
  m_image.newfilm=true;

  if(gformat->frame_duration) {
    m_fps = gformat->timescale / gformat->frame_duration;
  } else {
    m_fps = gformat->timescale;
  }

  m_fps_num=gformat->timescale;
  m_fps_denum=gformat->frame_duration;

  m_numFrames=-1;
#ifdef USE_FRAMETABLE
  m_frametable=bgav_get_frame_table(m_file, m_track);
  if(m_frametable)
    m_numFrames=gavl_frame_table_num_frames (m_frametable);
#endif

  gavl_time_t dur=bgav_get_duration (m_file, m_track);
  if(m_numFrames<0)
    if(dur!=GAVL_TIME_UNDEFINED)
      m_numFrames = gavl_time_to_frames(m_fps_num,
					m_fps_denum,
					dur);

  return true;
}

/////////////////////////////////////////////////////////
// render
//
/////////////////////////////////////////////////////////
pixBlock* filmGMERLIN :: getFrame(void){
  if(!m_file)return NULL;
  bgav_read_video(m_file, m_gframe, m_stream);
  if(m_doConvert) {
    gavl_video_convert (m_gconverter, m_gframe, m_finalframe);
    m_image.image.data=m_finalframe->planes[0];
  } else {
    m_image.image.data=m_gframe->planes[0];
  }
  m_image.newimage=true;
  m_image.image.upsidedown=true;

  m_next_timestamp=m_gframe->timestamp+m_gframe->duration;
  return &m_image;
}


/////////////////////////////////////////////////////////
// changeFrame
//
/////////////////////////////////////////////////////////
film::errCode filmGMERLIN :: changeImage(int imgNum, int trackNum){
  if(trackNum<0) {
    /* just automatically proceed to the next frame: this might speed up things for linear decoding */
    return film::SUCCESS;
  }

  if(!m_file)return film::FAILURE;

#if GEM_FILMGMERLIN_TRACKSWITCH
  // LATER implement track-switching
  // this really shares a lot of code with open() so it should go into a separate function
  if(trackNum) {
    if(m_numTracks>trackNum || trackNum<0) {
      error("[GEM:filmGMERLIN] selected invalid track %d of %d", trackNum, m_numTracks);
    } else {
      int numvstreams=bgav_num_video_streams (m_file, m_track);
      verbose(1, "[GEM:filmGMERLIN] track %d contains %d video streams", m_track, numvstreams);
      if(numvstreams) {
        bgav_select_track(m_file, m_track);
#ifdef USE_FRAMETABLE
        if(m_frametable) {
          gavl_frame_table_destroy(m_frametable);
          m_frametable=bgav_get_frame_table(m_file, m_track);
        }
#endif
      } else {
        error("[GEM:filmGMERLIN] track %d does not contain a video-stream: skipping");
      }
    }
  }
#endif /* GEM_FILMGMERLIN_TRACKSWITCH */

  if(imgNum>=m_numFrames || imgNum<0)return film::FAILURE;

  if(bgav_can_seek(m_file)) {
    if(0) {
#ifdef USE_FRAMETABLE
    } else if(m_frametable) {
      //  no assumptions about fixed framerate
      int64_t seekpos = gavl_frame_table_frame_to_time(m_frametable, imgNum, NULL);
      bgav_seek_video(m_file, m_track, seekpos);
      return film::SUCCESS;
#endif
    } else {
      //  assuming fixed framerate
      /*
        Plaum: "Relying on a constant framerate is not good."
        m_fps_denum and m_fps_num are set only once!
      */
      int64_t seekposOrg = (int64_t)imgNum*(int64_t)m_fps_denum;
      int64_t seekpos = seekposOrg;

#if 0
      // LATER: set a minimum frame offset
      // keep in mind that m_fps_denum could be 1!
      // so it's better to calculate the difference in milliseconds and compare
      int64_t diff=m_next_timestamp-seekpos;
# define TIMESTAMP_OFFSET_MAX 5
      if(diff<TIMESTAMP_OFFSET_MAX && diff>(TIMESTAMP_OFFSET_MAX * -1)) {
        // hey we are already there...
        return film::SUCCESS;
      }
#endif

      bgav_seek_scaled(m_file, &seekpos, m_fps_num);
      if(seekposOrg == seekpos)
        return film::SUCCESS;
      /* never mind: always return success... */
      return film::SUCCESS;
    }
  }

  return film::FAILURE;
}
///////////////////////////////
// Properties
bool filmGMERLIN::enumProperties(gem::Properties&readable,
			      gem::Properties&writeable) {
  readable.clear();
  writeable.clear();

  gem::any value;
  value=0.;
  readable.set("fps", value);
  readable.set("frames", value);
  readable.set("width", value);
  readable.set("height", value);

  return false;
}

void filmGMERLIN::setProperties(gem::Properties&props) {
}

void filmGMERLIN::getProperties(gem::Properties&props) {
  std::vector<std::string> keys=props.keys();
  gem::any value;
  double d;
  unsigned int i=0;
  for(i=0; i<keys.size(); i++) {
    std::string key=keys[i];
    props.erase(key);
    if("fps"==key) {
      d=m_fps;
      value=d; props.set(key, value);
    }
    if("frames"==key && m_numFrames>=0) {
      d=m_numFrames;
      value=d; props.set(key, value);
    }
    if("tracks"==key && m_numTracks>=0) {
      d=m_numTracks;
      value=d; props.set(key, value);
    }
    if("width"==key) {
      d=m_image.image.xsize;
      value=d; props.set(key, value);
    }
    if("height"==key) {
      d=m_image.image.ysize;
      value=d; props.set(key, value);
    }
  }
}


#endif // GMERLIN
