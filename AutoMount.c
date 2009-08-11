/*
 * Copyright (C) 2008 The Android Open Source Project
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

/*
** mountd automount support
*/


#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <pwd.h>
#include <stdlib.h>
#include <poll.h>
#include "vold.h"
#include "uevent.h"
#include "mmc.h"
#include "blkdev.h"
#include "volmgr.h"
#include "media.h"
#include <sys/mount.h>

#include "vold.h"
#include <sys/stat.h>
#include <linux/loop.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/netlink.h>
#include <cutils/log.h>
#define DEVPATH    "/dev/block/"
#define DEVPATHLENGTH 11    // strlen(DEVPATH)

// FIXME - only one loop mount is supported at a time
#define LOOP_DEVICE "/dev/block/loop0"

// timeout value for poll() when retries are pending

#define MAX_MOUNT_RETRIES   3
#define MAX_UNMOUNT_RETRIES   5


static pthread_t sAutoMountThread = 0;


// for synchronization between sAutoMountThread and the server thread
static pthread_mutex_t sMutex = PTHREAD_MUTEX_INITIALIZER;

// requests the USB mass_storage driver to begin or end sharing a block device
// via USB mass storage.

int AutoAddMountPoint(const char* device)
{
  LOGE("DEVICE: %s", device);
  char* rebun_name_device;
  char* rebun_name_mountpoint;
  rebun_name_device= (char* )malloc(128);
  rebun_name_mountpoint= (char* )malloc(128);

  sprintf(rebun_name_device, "/dev/block/%s",device);
  sprintf(rebun_name_mountpoint, "/sdcard/%s",device);
  return 0;
}

/*****************************************************
 * 
 * AUTO-MOUNTER THREAD
 * 
 *****************************************************/

// create a socket for listening to inotify events
int CreateINotifySocket()
{
    // initialize inotify
  LOGE("Creating INotify Socket");
    int fd = inotify_init();

    if (fd < 0) {
        LOGE("inotify_init failed, %s\n", strerror(errno));
        return -1;
    }

    fcntl(fd, F_SETFL, O_NONBLOCK | fcntl(fd, F_GETFL));

    return fd;
}

void fcheck(char *cc) 
{
  FILE *fp;
  fp = fopen(cc, "r");
  if( fp ) {
    LOGE("File %s OK", cc);
    fclose(fp);
  } else {
    LOGE("File %s FAIL", cc);
    // doesnt exist                                                                                                                                        
  }
}

static void* AutoMountThread(void* arg)
{

    int inotify_fd;
    int id;
    LOGE("Hi, AutoMountedThread OK");
    // initialize inotify
    inotify_fd = CreateINotifySocket();
    // watch for files created and deleted in "/dev"
    inotify_add_watch(inotify_fd, DEVPATH, IN_CREATE|IN_DELETE);
    
    while (1)
    {
        struct pollfd fds[1];
        int timeout, result;

#define INOTIFY_IDX 0
#define UEVENT_IDX  1
    
        fds[INOTIFY_IDX].fd = inotify_fd;
        fds[INOTIFY_IDX].events = POLLIN;
        fds[INOTIFY_IDX].revents = 0;
        
        result = poll(fds, 1, -1);

        // lock the mutex while we are handling events
        pthread_mutex_lock(&sMutex);

        // handle inotify notifications for block device creation and deletion
        if (fds[INOTIFY_IDX].revents == POLLIN)
        {
            struct inotify_event    event;
            char    buffer[512];
            int length = read(inotify_fd, buffer, sizeof(buffer));
            int offset = 0;

            while (length >= (int)sizeof(struct inotify_event))
            {
               struct inotify_event* event = (struct inotify_event *)&buffer[offset];
               if (event->mask == IN_CREATE) {
                   LOGE("/dev/block/%s created\n", event->name);
		   			AutoAddMountPoint(event->name);	//add by rebun
                    char ccDevice[128];
                    char ccMountPoint[128];
                    sprintf(ccDevice, "/dev/block/%s",event->name);
                    sprintf(ccMountPoint, "/sdcard/%s",event->name);
                    if( mkdir(ccMountPoint, 0777) ) {
		      LOGE("Hi, Why cann't I create this folder?");
		    }
                    
                    char *fs[] = {"ext3", "ext2", "vfat", NULL};
                    char **f;
                    for (f=fs; *f != NULL; f++) {
                      int flags, rc;
                      flags = MS_NODEV | MS_NOEXEC | MS_NOSUID | MS_NOATIME | MS_NODIRATIME;
                      LOGE("mount %s %s : %s", ccDevice, ccMountPoint, *f);
                      if (ccDevice[strlen(ccDevice)-1] <= '9' && ccDevice[strlen(ccDevice)-1] >= '1') {
                        char cc[128];
                        strcpy(cc, ccMountPoint);
                        cc[strlen(cc)-1] = '\0';
                        LOGE("I will umount [%s] first", cc);
			fcheck(ccMountPoint);
			fcheck(ccDevice);
			fcheck(cc);
     
                        if(umount(cc)) {
                          LOGE("Hi, this is a fatal bug");
                        }
                      }
                      rc = mount(ccDevice, ccMountPoint, *f, flags, NULL);
                      LOGE("RW rc = %d; errno = %d", rc, errno);
                      if (rc == 0 && errno == EROFS) {
                        flags |= MS_RDONLY;
                        rc = mount(ccDevice, ccMountPoint, *f, flags, NULL);
                        LOGE("RO rc = %d; errno = %d", rc, errno);
                      }
                    }
                    send_msg_with_data(VOLD_EVT_MOUNTED, ccMountPoint);
               } else if (event->mask == IN_DELETE) {
                   LOGE("/dev/block/%s deleted\n", event->name);
                   int rc;
                   char ccDevice[128];
                   char ccMountPoint[128];
                   sprintf(ccDevice, "/dev/block/%s",event->name);
                   sprintf(ccMountPoint, "/sdcard/%s",event->name);
                   send_msg_with_data(VOLD_EVT_UNMOUNTED, ccMountPoint);
                   rc = umount(ccMountPoint);
                   LOGE("umount %d; errno %d", rc, errno);
                   rc = rmdir(ccMountPoint);
                   LOGE("rmdir %d; errno %d", rc, errno);

               } else {
                 LOGE("Name: %s", event->name);
               }
               
               int size = sizeof(struct inotify_event) + event->len;
               length -= size;
               offset += size;
            }
        }

        pthread_mutex_unlock(&sMutex);
    }

    inotify_rm_watch(inotify_fd, id);
    close(inotify_fd);

    return NULL;
}


void StartAutoMounter()
{
  LOGE("Hi starting automounter");
    pthread_create(&sAutoMountThread, NULL, AutoMountThread, NULL);
}
