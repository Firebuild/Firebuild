/* Copyright (c) 2021 Interri Kft. */
/* This file is an unpublished work. All rights reserved. */

#include "firebuild/epoll.h"

#include <string.h>
#include <sys/epoll.h>
#include <time.h>

#include <cassert>
#include <vector>

#include "firebuild/utils.h"

namespace firebuild {

void Epoll::ensure_room_fd(int fd) {
  if (fd >= static_cast<ssize_t>(fd_contexts_.size())) {
    fd_contexts_.resize(fd + 1);
  }
}

bool Epoll::is_added_fd(int fd) {
  return fd < static_cast<ssize_t>(fd_contexts_.size())
      && fd_contexts_[fd].callback != nullptr;
}

void Epoll::add_fd(int fd, uint32_t events,
                   void (*callback)(const struct epoll_event* event, void *callback_user_data),
                   void *callback_user_data) {
  ensure_room_fd(fd);
  assert(fd_contexts_[fd].callback == nullptr);
  fd_contexts_[fd].callback = callback;
  fd_contexts_[fd].callback_user_data = callback_user_data;
  struct epoll_event ee;
  memset(&ee, 0, sizeof(ee));
  ee.events = events;
  ee.data.fd = fd;
  epoll_ctl(epollfd_, EPOLL_CTL_ADD, fd, &ee);
}

void Epoll::del_fd(int fd) {
  ensure_room_fd(fd);
  assert(fd_contexts_[fd].callback != nullptr);
  fd_contexts_[fd].callback = nullptr;
  epoll_ctl(epollfd_, EPOLL_CTL_DEL, fd, NULL);

  /* When deleting an fd, make sure to also delete it from the yet unprocessed part of
   * epoll_wait()'s returned events. Do this by setting .data.fd to -1.
   *
   * Example: epoll_wait() returns a set of two events, one for fd1, one for fd2. The callback of
   * fd1 might remove fd2 from the epoll set, or might even close fd2, and might even open another
   * file which happens to receive the same file descriptor. Calling fd2's registered callback in
   * the next iteration of process_all_events()'s loop could result in uncontrollable bad
   * consequences. */
  for (int i = event_current_ + 1; i < event_count_; i++) {
    if (events_[i].data.fd == fd) {
      events_[i].data.fd = -1;
      break;
    }
  }
}

void Epoll::maybe_del_fd(int fd) {
  /* Note: if fd is not added to the epoll set then there's no way it could be present anywhere in
   * events_. So in that case it's okay to skip the tricky loop of del_fd(), too. */
  if (is_added_fd(fd)) {
    del_fd(fd);
  }
}

int Epoll::add_timer(int ms,
                     void (*callback)(void *callback_user_data),
                     void *callback_user_data) {
  /* Find the first empty slot. */
  int timer_id;
  for (timer_id = 0; timer_id <= largest_timer_id_; timer_id++) {
    if (timer_contexts_[timer_id].callback == nullptr) {
      break;
    }
  }
  if (timer_id > largest_timer_id_) {
    largest_timer_id_++;
    if (static_cast<size_t>(timer_id) == timer_contexts_.size()) {
      timer_contexts_.resize(timer_id + 1);
    }
  }

  /* Set the callback. */
  timer_contexts_[timer_id].callback = callback;
  timer_contexts_[timer_id].callback_user_data = callback_user_data;

  /* Compute when to fire. */
  struct timespec now, delay;
  clock_gettime(CLOCK_MONOTONIC, &now);
  delay.tv_sec = ms / 1000;
  delay.tv_nsec = (ms % 1000) * 1000 * 1000;
  timespecadd(&now, &delay, &timer_contexts_[timer_id].when);

  /* Update next_timer_ to point to the timeout that will elapse next. */
  if (next_timer_ < 0 ||
      timespeccmp(&timer_contexts_[timer_id].when, &timer_contexts_[next_timer_].when, <)) {
    next_timer_ = timer_id;
  }

  return timer_id;
}

void Epoll::del_timer(int timer_id) {
  assert(timer_contexts_[timer_id].callback != nullptr);
  timer_contexts_[timer_id].callback = nullptr;

  /* Cap largest_timer_id to point to the new largest used timer slot. */
  while (largest_timer_id_ >= 0 && timer_contexts_[largest_timer_id_].callback == nullptr) {
    largest_timer_id_--;
  }

  /* Update next_timer_. */
  if (timer_id == next_timer_) {
    /* Iterate over the array to find the closest in time, skipping deleted entries. */
    next_timer_ = -1;
    for (int i = 0; i <= largest_timer_id_; i++) {
      if (timer_contexts_[i].callback != nullptr &&
          (next_timer_ < 0 ||
           timespeccmp(&timer_contexts_[i].when, &timer_contexts_[next_timer_].when, <))) {
        next_timer_ = i;
      }
    }
  }

  /* Note that the trick we do in del_fd() is not necessary here. If the callback of a timer deletes
   * another timer, or creates a new one, maybe even occupying a deleted one's id, the "worst" that
   * can happen is that the ongoing process_all_events() will already execute that timer if it has
   * already elapsed. */
}

}  // namespace firebuild