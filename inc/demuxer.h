#ifndef DEMUXER_H
#define DEMUXER_H

#include <chrono>
extern "C"
{
  #include <libavformat/avformat.h>
}
#include "inc/packet.h"

/* Usage example:
Demuxer dmx("v4l2");
dmx
.set_option("framerate", 30)
.open("file:/dev/video0");
*/

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

class Demuxer
{
public:
  explicit Demuxer(const std::string& format = "");
  Demuxer& set_option(const std::string& key, const std::string& value, int flags = 0);
  Demuxer& set_option(const std::string& key, int value, int flags = 0);
  void open(const std::string& url);
  ~Demuxer();
  Packet read();
  const AVCodecParameters* stream_parameters(size_t index) const;
  size_t num_of_streams() const;
  size_t video_stream_index() const;
private:
  void throw_if_not_open() const;
  static int interrupt_callback(void* this_);
  AVFormatContext* ctx_ = nullptr;
  AVInputFormat* in_format_ = nullptr;
  AVDictionary *options_ = nullptr;
  Clock::time_point last_read_;
  static const Clock::duration READ_TIMEOUT;
};

#endif // DEMUXER_H
