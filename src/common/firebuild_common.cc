/* Copyright (c) 2014 Balint Reczey <balint@balintreczey.hu> */
/* This file is an unpublished work. All rights reserved. */


#include "common/firebuild_common.h"

#include <netinet/in.h>
#include <google/protobuf/message_lite.h>

#include <cstdlib>


namespace firebuild {

/**
 * Send protobuf message via file descriptor
 *
 * Framing is very simple: 4 bytes length, then the protobuf message serialized
 *
 * No thread locking or signal blocking/delaying performed, it's up to the caller
 */
extern ssize_t fb_send_msg_unlocked(const google::protobuf::MessageLite &pb_msg,
                                    const int fd) {
  int offset = 0;
  uint32_t msg_size = pb_msg.ByteSize(), msg_size_n = htonl(msg_size);
  char *buf = new char[sizeof(msg_size) + msg_size];
  for (size_t i = 0; i < sizeof(msg_size_n); i++) {
    buf[i] = reinterpret_cast<char*>(&msg_size_n)[i];
  }

  offset += sizeof(uint32_t);

  pb_msg.SerializeWithCachedSizesToArray(
      reinterpret_cast<uint8_t*>(&buf[offset]));
  auto ret = fb_write(fd, buf, msg_size + offset);

  delete[] buf;
  return ret;
}


/**
 * Read protobuf message via file descriptor
 *
 * Framing is very simple: 4 bytes length, then the protobuf message serialized
 *
 * No thread locking or signal blocking/delaying performed, it's up to the caller
 */
extern ssize_t fb_recv_msg_unlocked(google::protobuf::MessageLite *pb_msg,
                                    const int fd) {
  uint32_t msg_size;

  /* read serialized length */
  auto ret = fb_read(fd, &msg_size, sizeof(msg_size));
  if (ret == -1 || ret == 0) {
    return ret;
  }
  msg_size = ntohl(msg_size);

  auto buf = new char[msg_size];
  /* read serialized msg */
  if ((ret = fb_read(fd, buf, msg_size)) == -1) {
    delete[] buf;
    return ret;
  }

  pb_msg->ParseFromArray(buf, msg_size);

  delete[] buf;
  return ret;
}

}  /* namespace firebuild */
