/** @file setupRoot.c
 *  @brief Prepare a shifter environment based on image in the filesystem.
 *
 *  The setupRoot program prepares a shifter environment, including performing
 *  site-required modifications and user-requested bind mounts.  This is 
 *  intended to be run by a WLM prologue prior to batch script execution.
 *
 *  @author Douglas M. Jacobsen <dmjacobsen@lbl.gov>
 */

/* Shifter, Copyright (c) 2015, The Regents of the University of California,
 * through Lawrence Berkeley National Laboratory (subject to receipt of any
 * required approvals from the U.S. Dept. of Energy).  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. Neither the name of the University of California, Lawrence Berkeley
 *     National Laboratory, U.S. Dept. of Energy nor the names of its
 *     contributors may be used to endorse or promote products derived from this
 *     software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *  
 * You are under no obligation whatsoever to provide any bug fixes, patches, or
 * upgrades to the features, functionality or performance of the source code
 * ("Enhancements") to anyone; however, if you choose to make your Enhancements
 * available either publicly, or directly to Lawrence Berkeley National
 * Laboratory, without imposing a separate written license agreement for such
 * Enhancements, then you hereby grant the following license: a  non-exclusive,
 * royalty-free perpetual license to install, use, modify, prepare derivative
 * works, incorporate into other computer software, distribute, and sublicense
 * such enhancements or derivative works thereof, in binary and source code
 * form.
 */


#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#include <dirent.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>

#include "ImageData.h"
#include "UdiRootConfig.h"
#include "shifter_core.h"

#include "config.h"

#define VOLUME_ALLOC_BLOCK 10

typedef struct _SetupRootConfig {
    char *sshPubKey;
    char *user;
    char *imageType;
    char *imageIdentifier;
    uid_t uid;
    char *minNodeSpec;
    char **volumeMapFrom;
    char **volumeMapTo;
    char **volumeMapFlags;

    size_t volumeMap_capacity;
    char **volumeMapFrom_ptr;
    char **volumeMapTo_ptr;
    char **volumeMapFlags_ptr;
    int verbose;
} SetupRootConfig;

static void _usage(int);
static char *_filterString(char *input);
int parse_SetupRootConfig(int argc, char **argv, SetupRootConfig *config);
void free_SetupRootConfig(SetupRootConfig *config);
void fprint_SetupRootConfig(FILE *, SetupRootConfig *config);
int getImage(ImageData *, SetupRootConfig *, UdiRootConfig *);

int main(int argc, char **argv) {
    UdiRootConfig udiConfig;
    SetupRootConfig config;

    ImageData image;

    memset(&udiConfig, 0, sizeof(UdiRootConfig));
    memset(&config, 0, sizeof(SetupRootConfig));
    memset(&image, 0, sizeof(ImageData));

    clearenv();
    setenv("PATH", "/usr/bin:/usr/sbin:/bin:/sbin", 1);

    if (parse_SetupRootConfig(argc, argv, &config) != 0) {
        fprintf(stderr, "FAILED to parse command line arguments. Exiting.\n");
        _usage(1);
    }
    if (parse_UdiRootConfig(UDIROOT_CONFIG_FILE, &udiConfig, 0) != 0) {
        fprintf(stderr, "FAILED to parse udiRoot configuration. Exiting.\n");
        exit(1);
    }
    if (config.verbose) {
        fprint_SetupRootConfig(stdout, &config);
        fprint_UdiRootConfig(stdout, &udiConfig);
    }

    if (getImage(&image, &config, &udiConfig) != 0) {
        fprintf(stderr, "FAILED to get image %s of type %s\n", config.imageIdentifier, config.imageType);
        exit(1);
    }
    if (config.verbose) {
        fprint_ImageData(stdout, &image);
    }
    if (image.useLoopMount) {
        if (mountImageLoop(&image, &udiConfig) != 0) {
            fprintf(stderr, "FAILED to mount image on loop device.\n");
            exit(1);
        }
    }
    if (mountImageVFS(&image, config.user, config.minNodeSpec, &udiConfig) != 0) {
        fprintf(stderr, "FAILED to mount image into UDI\n");
        exit(1);
    }

    if (config.sshPubKey != NULL && strlen(config.sshPubKey) > 0
            && config.user != NULL && strlen(config.user) > 0
            && config.uid != 0) {
        if (setupImageSsh(config.sshPubKey, config.user, config.uid, &udiConfig) != 0) {
            fprintf(stderr, "FAILED to setup ssh configuration\n");
            exit(1);
        }
        if (startSshd(&udiConfig) != 0) {
            fprintf(stderr, "FAILED to start sshd\n");
            exit(1);
        }
    }

    if (setupUserMounts(&image, config.volumeMapFrom, config.volumeMapTo, config.volumeMapFlags, &udiConfig) != 0) {
        fprintf(stderr, "FAILED to setup user-requested mounts.\n");
        exit(1);
    }

    return 0;
}

static void _usage(int exitStatus) {
    exit(exitStatus);
}

int parse_SetupRootConfig(int argc, char **argv, SetupRootConfig *config) {
    int opt = 0;
    optind = 1;

    while ((opt = getopt(argc, argv, "v:s:u:U:N:V")) != -1) {
        switch (opt) {
            case 'V': config->verbose = 1; break;
            case 'v':
                {
                    char *from  = strtok(optarg, ":");
                    char *to    = strtok(NULL,   ":");
                    char *flags = strtok(NULL,   ":");
                    size_t cnt = config->volumeMapFrom_ptr - config->volumeMapFrom;

                    if (from == NULL || to == NULL) {
                        fprintf(stderr, "ERROR: invalid format for volume map!");
                        _usage(1);
                    }

                    if (config->volumeMapFrom == NULL || (cnt + 2) >= config->volumeMap_capacity) {
                        char **fromPtr = realloc(config->volumeMapFrom, config->volumeMap_capacity + VOLUME_ALLOC_BLOCK);
                        char **toPtr = realloc(config->volumeMapTo, config->volumeMap_capacity + VOLUME_ALLOC_BLOCK);
                        char **flagsPtr = realloc(config->volumeMapFlags, config->volumeMap_capacity + VOLUME_ALLOC_BLOCK);
                        if (fromPtr == NULL || toPtr == NULL || flagsPtr == NULL) {
                            fprintf(stderr, "ERROR: unable to allocate memory for volume map!\n");
                            _usage(1);
                        }
                        config->volumeMapFrom = fromPtr;
                        config->volumeMapTo = toPtr;
                        config->volumeMapFlags = flagsPtr;
                        config->volumeMapFrom_ptr = fromPtr + cnt;
                        config->volumeMapTo_ptr = toPtr + cnt;
                        config->volumeMapFlags_ptr = flagsPtr + cnt;
                    }
                    *(config->volumeMapFrom_ptr) = strdup(from);
                    *(config->volumeMapTo_ptr) = strdup(to);
                    *(config->volumeMapFlags_ptr) = (flags ? strdup(flags) : NULL);
                    config->volumeMapFrom_ptr++;
                    config->volumeMapTo_ptr++;
                    config->volumeMapFlags_ptr++;
                    *(config->volumeMapFrom_ptr) = NULL;
                    *(config->volumeMapTo_ptr) = NULL;
                    *(config->volumeMapFlags_ptr) = NULL;
                }

                break;
            case 's':
                config->sshPubKey = strdup(optarg);
                break;
            case 'u':
                config->user = strdup(optarg);
                break;
            case 'U':
                config->uid = strtoul(optarg, NULL, 10);
                break;
            case 'N':
                config->minNodeSpec = strdup(optarg);
                break;
            case '?':
                fprintf(stderr, "Missing an argument!\n");
                _usage(1);
                break;
            default:
                break;
        }
    }

    int remaining = argc - optind;
    if (remaining != 2) {
        fprintf(stderr, "Must specify image type and image identifier\n");
        _usage(1);
    }
    config->imageType = _filterString(argv[optind++]);
    config->imageIdentifier = _filterString(argv[optind++]);
    return 0;
}

void free_SetupRootConfig(SetupRootConfig *config) {
    char **volPtr = NULL;
    if (config->sshPubKey != NULL) {
        free(config->sshPubKey);
    }
    if (config->user != NULL) {
        free(config->user);
    }
    if (config->imageType != NULL) {
        free(config->imageType);
    }
    if (config->imageIdentifier != NULL) {
        free(config->imageIdentifier);
    }
    if (config->minNodeSpec != NULL) {
        free(config->minNodeSpec);
    }
    for (volPtr = config->volumeMapFrom; volPtr && *volPtr; volPtr++) {
        free(*volPtr);
    }
    for (volPtr = config->volumeMapTo; volPtr && *volPtr; volPtr++) {
        free(*volPtr);
    }
    for (volPtr = config->volumeMapFlags; volPtr && *volPtr; volPtr++) {
        free(*volPtr);
    }
    if (config->volumeMapFrom) {
        free(config->volumeMapFrom);
    }
    if (config->volumeMapTo) {
        free(config->volumeMapTo);
    }
    if (config->volumeMapFlags) {
        free(config->volumeMapFlags);
    }
    free(config);
}

void fprint_SetupRootConfig(FILE *fp, SetupRootConfig *config) {
    if (config == NULL || fp == NULL) return;
    fprintf(fp, "***** SetupRootConfig *****\n");
    fprintf(fp, "imageType: %s\n", (config->imageType ? config->imageType : ""));
    fprintf(fp, "imageIdentifier: %s\n", (config->imageIdentifier ? config->imageIdentifier : ""));
    fprintf(fp, "sshPubKey: %s\n", (config->sshPubKey ? config->sshPubKey : ""));
    fprintf(fp, "user: %s\n", (config->user ? config->user : ""));
    fprintf(fp, "uid: %d\n", config->uid);
    fprintf(fp, "minNodeSpec: %s\n", (config->minNodeSpec ? config->minNodeSpec : ""));
    fprintf(fp, "volumeMap: %lu maps\n", (config->volumeMapFrom_ptr - config->volumeMapFrom));
    if (config->volumeMapFrom) {
        char **from = config->volumeMapFrom;
        char **to = config->volumeMapTo;
        char **flags = config->volumeMapFlags;
        for (; *from && *to; from++, to++, flags++) {
            fprintf(fp, "    FROM: %s, TO: %s, FLAGS: %s\n", *from, *to, (*flags ? *flags : "NONE"));
        }
    }
    fprintf(fp, "***** END SetupRootConfig *****\n");
}

int getImage(ImageData *imageData, SetupRootConfig *config, UdiRootConfig *udiConfig) {
    int ret = parse_ImageData(config->imageIdentifier, udiConfig, imageData);
    return ret;
}

static char *_filterString(char *input) {
    ssize_t len = 0;
    char *ret = NULL;
    char *rptr = NULL;
    char *wptr = NULL;
    if (input == NULL) return NULL;

    len = strlen(input) + 1;
    ret = (char *) malloc(sizeof(char) * len);
    if (ret == NULL) return NULL;

    rptr = input;
    wptr = ret;
    while (wptr - ret < len && *rptr != 0) {
        if (isalnum(*rptr) || *rptr == '_' || *rptr == ':' || *rptr == '.' || *rptr == '+' || *rptr == '-') {
            *wptr++ = *rptr;
        }
        rptr++;
    }
    *wptr = 0;
    return ret;
}
