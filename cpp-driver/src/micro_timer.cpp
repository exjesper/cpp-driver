/*
  Copyright (c) DataStax, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "micro_timer.hpp"

#include "memory.hpp"

#ifdef HAVE_TIMERFD
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>
#endif

namespace cass {

#ifdef HAVE_TIMERFD

MicroTimer::MicroTimer()
  : handle_(NULL)
  , fd_(-1)
  , state_(CLOSED) { }

int MicroTimer::start(uv_loop_t* loop,
                      uint64_t timeout_us,
                      const Callback& callback) {
  int rc = 0;
  if (fd_ == -1) {
    fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (fd_ == -1)  return errno;
  }
  if (handle_ == NULL) {
    handle_ = Memory::allocate<uv_poll_t>();
    handle_->loop = NULL;
    handle_->data = this;
  }
  if (state_ == CLOSED) {
    rc = uv_poll_init(loop, handle_, fd_);
    if (rc != 0) return rc;
    state_ = STOPPED;
  }
  if (state_ == STOPPED) {
    rc = uv_poll_start(handle_, UV_READABLE, on_timeout);
    if (rc != 0) return rc;
    set_time(timeout_us);
    state_ = STARTED;
  }
  callback_ = callback;
  return rc;
}

void MicroTimer::stop() {
  if (state_ == STARTED) {
    state_ = STOPPED;
    set_time(0);
    uv_poll_stop(handle_);
  }
}

void MicroTimer::close_handle() {
  if (fd_ != -1) {
    close(fd_);
    fd_ = -1;
  }
  if (handle_ != NULL) {
    if (state_ == CLOSED) { // The handle was allocated, but initialization failed.
      Memory::deallocate(handle_);
    } else { // If initialized or started then close the handle properly.
      uv_close(reinterpret_cast<uv_handle_t*>(handle_), on_close);
    }
    state_ = CLOSED;
    handle_ = NULL;
  }
}

bool MicroTimer::is_running() const {
  return state_ == STARTED;
}

void MicroTimer::set_time(uint64_t timeout_us) {
  struct itimerspec ts;
  memset(&ts.it_interval, 0, sizeof(struct timespec));
  ts.it_value.tv_sec = timeout_us / (1000 * 1000);
  ts.it_value.tv_nsec = (timeout_us % (1000 * 1000))  * 1000;
  timerfd_settime(fd_, 0, &ts, NULL);
}

void MicroTimer::on_timeout(uv_poll_t* poll, int status, int events) {
  MicroTimer* timer = static_cast<MicroTimer*>(poll->data);
  timer->handle_timeout();
}

void MicroTimer::handle_timeout() {
  uint64_t count;
  int result = read(fd_, &count, sizeof(count));
  UNUSED_(result);
  state_ = STOPPED;
  uv_poll_stop(handle_);
  callback_(this);
}

void MicroTimer::on_close(uv_handle_t* handle) {
  Memory::deallocate(reinterpret_cast<uv_poll_t*>(handle));
}
#else

#define PERCENT_OF_MILLSECOND_TO_USE_TIMER 95

MicroTimer::MicroTimer()
  : timeout_ns_(0) { }

int MicroTimer::start(uv_loop_t* loop,
                      uint64_t timeout_us,
                      const Callback& callback) {
  if (is_running()) {
    return 0;
  }

  uint64_t ms = timeout_us / 1000;
  uint64_t us =  timeout_us - (ms * 1000);

  timeout_ns_ = uv_hrtime() + timeout_us * 1000; // Convert to nanoseconds
  callback_ = callback;

  if (us >= (1000 * PERCENT_OF_MILLSECOND_TO_USE_TIMER) / 100) {
    // If the requested sub-millisecond part of the timeout is within a certain
    // percentage of a millisecond then round up to the next millisecond and use
    // the timer.
    return timer_.start(loop, ms + 1,
                        bind_callback(&MicroTimer::on_timeout, this));
  } else if (ms > 0) {
    // There are whole milliseconds to wait for and possibly a sub-millisecond
    // part to wait for after the timer.
    return timer_.start(loop, ms,
                        bind_callback(&MicroTimer::on_timeout, this));
  } else {
    // Otherwise, idle (i.e. busy wait) the loop until our sub-millisecond timeout
    // is reached.
    return idle_.start(loop,
                       bind_callback(&MicroTimer::on_idle, this));
  }
}

void MicroTimer::stop() {
  idle_.stop();
  timer_.stop();
}

void MicroTimer::close_handle() {
  idle_.close_handle();
  timer_.close_handle();
}

bool MicroTimer::is_running() const {
  return idle_.is_running() || timer_.is_running();
}

void MicroTimer::on_timeout(Timer* timer) {
  uint64_t now = uv_hrtime();
  if (now >= timeout_ns_) {
    // The goal timeout was reached using the timer, trigger the callback.
    callback_(this);
  } else {
    // There's still a sub-millisecond part to wait for so idle the loop until
    // the timeout is reached.
    idle_.start(timer_.loop(), bind_callback(&MicroTimer::on_idle, this));
  }
}

void MicroTimer::on_idle(Idle* idle) {
  uint64_t now = uv_hrtime();
  if (now >= timeout_ns_) {
    // The goal timeout was reached by idling the loop, trigger the callback.
    idle_.stop();
    callback_(this);
  }
}

#endif

} // namespace cass
