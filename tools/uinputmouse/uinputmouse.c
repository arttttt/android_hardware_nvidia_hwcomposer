/*
 * Copyright (C) 2026 Artem Bambalov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* A mouse made of nothing, for a tablet that has none.
 *
 * Registers a relative pointing device with the kernel through uinput and
 * moves it in a smooth circle for a while. To everything above the kernel
 * this is a real mouse: the framework shows its pointer, sends the
 * composer a cursor layer, and streams positions at the device's own rate
 * -- which is exactly the scene the hardware cursor's promise is judged
 * in. When the program exits the device unplugs itself and the pointer
 * goes away, taking the hide path with it.
 *
 * Usage: uinputmouse [seconds] [rate_hz]   (defaults: 10 seconds, 60 Hz)
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int emit(int fd, int type, int code, int value) {
  struct input_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = type;
  ev.code = code;
  ev.value = value;
  return write(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) ? 0 : -1;
}

int main(int argc, char **argv) {
  const int seconds = argc > 1 ? atoi(argv[1]) : 10;
  const int rate = argc > 2 ? atoi(argv[2]) : 60;
  if (seconds <= 0 || rate <= 0 || rate > 1000) {
    fprintf(stderr, "usage: uinputmouse [seconds] [rate_hz]\n");
    return 1;
  }

  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) {
    fprintf(stderr, "cannot open /dev/uinput: %s\n", strerror(errno));
    return 1;
  }

  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
  ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
  ioctl(fd, UI_SET_EVBIT, EV_REL);
  ioctl(fd, UI_SET_RELBIT, REL_X);
  ioctl(fd, UI_SET_RELBIT, REL_Y);

  struct uinput_user_dev dev;
  memset(&dev, 0, sizeof(dev));
  snprintf(dev.name, sizeof(dev.name), "virtual-mouse");
  dev.id.bustype = BUS_USB;
  dev.id.vendor = 0x1;
  dev.id.product = 0x1;
  dev.id.version = 1;
  if (write(fd, &dev, sizeof(dev)) != (ssize_t)sizeof(dev) ||
      ioctl(fd, UI_DEV_CREATE) != 0) {
    fprintf(stderr, "cannot create the device: %s\n", strerror(errno));
    close(fd);
    return 1;
  }

  printf("mouse plugged in; settling\n");
  sleep(2);

  /* A smooth circle: every tick a small step whose direction turns a
   * little, which is what a hand does and what a frame counter must not
   * notice. */
  const long tick_ns = 1000000000L / rate;
  const long total = (long)seconds * rate;
  printf("moving for %d s at %d Hz (%ld events)\n", seconds, rate, total);
  double phase = 0.0;
  for (long i = 0; i < total; i++) {
    const int dx = (int)lround(6.0 * cos(phase));
    const int dy = (int)lround(6.0 * sin(phase));
    phase += 0.05;
    emit(fd, EV_REL, REL_X, dx);
    emit(fd, EV_REL, REL_Y, dy);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    struct timespec ts = {0, tick_ns};
    nanosleep(&ts, NULL);
  }

  printf("done; unplugging in 3 s\n");
  sleep(3);
  ioctl(fd, UI_DEV_DESTROY);
  close(fd);
  return 0;
}
