/****************************************************************************
 * apps/system/uacplay/uacplay_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <audioutils/nxaudio.h>
#include <nuttx/audio/audio.h>
#include <nuttx/usb/uac1.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define UACPLAY_RATE       48000
#define UACPLAY_BITS       16
#define UACPLAY_CHANNELS   2
#define UACPLAY_RETRY_US   20000
#define UACPLAY_VOL_MIN    (-63 * 256)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int uacplay_fill(int fd, FAR struct ap_buffer_s *apb)
{
  ssize_t nread;
  size_t total = 0;

  apb->curbyte = 0;
  apb->flags = 0;
  while (total < apb->nmaxbytes)
    {
      nread = read(fd, apb->samp + total, apb->nmaxbytes - total);
      if (nread < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (nread == 0)
        {
          return -EPIPE;
        }

      total += nread;
    }

  apb->nbytes = total;
  return OK;
}

static uint16_t uacplay_volume(FAR const struct uac1_status_s *status)
{
  int32_t volume;

  if (status->mute)
    {
      return 0;
    }

  volume = status->volume;
  if (volume < UACPLAY_VOL_MIN)
    {
      volume = UACPLAY_VOL_MIN;
    }
  else if (volume > 0)
    {
      volume = 0;
    }

  return (uint16_t)((volume - UACPLAY_VOL_MIN) * 1000 /
                    -UACPLAY_VOL_MIN);
}

static int uacplay_stream(int usbfd)
{
  struct nxaudio_s audio;
  struct uac1_status_s status;
  struct audio_msg_s msg;
  FAR struct ap_buffer_s *apb;
  unsigned int prio;
  uint16_t lastvolume = UINT16_MAX;
  ssize_t msglen;
  int ret;
  int i;

  memset(&audio, 0, sizeof(audio));
  ret = init_nxaudio_devname(&audio, UACPLAY_RATE, UACPLAY_BITS,
                             UACPLAY_CHANNELS,
                             CONFIG_SYSTEM_UACPLAY_AUDIODEV,
                             CONFIG_AUDIOUTILS_NXAUDIO_MSGQNAME);
  if (ret < 0)
    {
      printf("uacplay: cannot initialize %s\n",
             CONFIG_SYSTEM_UACPLAY_AUDIODEV);
      return ret;
    }

  /* Prefill every codec buffer.  With the board configuration's 1536-byte
   * buffers this gives 32 ms of startup elasticity without a large delay.
   */

  for (i = 0; i < audio.abufnum; i++)
    {
      apb = audio.abufs[i];
      ret = uacplay_fill(usbfd, apb);
      if (ret < 0)
        {
          goto out;
        }

      ret = nxaudio_enqbuffer(&audio, apb);
      if (ret < 0)
        {
          goto out;
        }
    }

  if (ioctl(usbfd, UAC1IOC_GETSTATUS,
            (unsigned long)(uintptr_t)&status) == 0)
    {
      lastvolume = uacplay_volume(&status);
      nxaudio_setvolume(&audio, lastvolume);
    }

  ret = nxaudio_start(&audio);
  if (ret < 0)
    {
      goto out;
    }

  printf("uacplay: streaming 48000 Hz, stereo, 16-bit PCM\n");
  for (; ; )
    {
      msglen = mq_receive(audio.mq, (FAR char *)&msg, sizeof(msg), &prio);
      if (msglen < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          ret = -errno;
          break;
        }

      if (msg.msg_id == AUDIO_MSG_DEQUEUE)
        {
          apb = msg.u.ptr;
          ret = uacplay_fill(usbfd, apb);
          if (ret < 0)
            {
              break;
            }

          if (ioctl(usbfd, UAC1IOC_GETSTATUS,
                    (unsigned long)(uintptr_t)&status) == 0)
            {
              uint16_t volume = uacplay_volume(&status);
              if (volume != lastvolume)
                {
                  nxaudio_setvolume(&audio, volume);
                  lastvolume = volume;
                }
            }

          ret = nxaudio_enqbuffer(&audio, apb);
          if (ret < 0)
            {
              break;
            }
        }
      else if (msg.msg_id == AUDIO_MSG_COMPLETE)
        {
          ret = OK;
          break;
        }
    }

out:
  nxaudio_stop(&audio);
  fin_nxaudio(&audio);
  printf("uacplay: stream stopped (%d)\n", ret);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int usbfd;

  usbfd = open(CONFIG_SYSTEM_UACPLAY_USBDEV, O_RDONLY | O_CLOEXEC);
  if (usbfd < 0)
    {
      printf("uacplay: cannot open %s: %d\n",
             CONFIG_SYSTEM_UACPLAY_USBDEV, errno);
      return EXIT_FAILURE;
    }

  printf("uacplay: waiting for USB host audio\n");
  for (; ; )
    {
      uacplay_stream(usbfd);
      usleep(UACPLAY_RETRY_US);
    }

  close(usbfd);
  return EXIT_SUCCESS;
}
