/*
 * ===========================================================================
 *
 *       Filename:  scheduler.cpp
 *
 *    Description:  Scheduler thread and implementation
 *
 *        Version:  1.0
 *        Created:  03/31/2011 06:18:40 PM
 *
 *         Author:  Will Dietz (WD), wdietz2@illinios.edu
 *                  Kevin Larson (KL), kevinlarson1@gmail.com
 *        Project:  Koala
 *
 * ===========================================================================
 */

#include <scheduler.h>

#include <stdlib.h>
#include <signal.h>

#include <eucalyptus.h>
#include <data.h>
#include <string.h>
#include <misc.h>
#include <handlers.h>

static void schedule(void);

// "Knobs" go here.
typedef struct {
  int schedFreq;
} schedConfig_t;

typedef struct {
  ccInstance * instance;
  ccResource * resource;
} scheduledVM;

typedef char (*scAlgo)(ccResourceCache *, ccInstanceCache *, scheduledVM *);

char balanceSchedule(ccResourceCache *, ccInstanceCache *, scheduledVM*);
scAlgo scheduler = balanceSchedule;


static schedConfig_t schedConfig;

// Because I'm lazy
#define logsc(LOGLEVEL, formatstr, ...) \
  logprintfl(LOGLEVEL, "schedulerThread(): " formatstr, ##__VA_ARGS__)

static void setSignalHandler(void) {
  // set up default signal handler for this child process (for SIGTERM)
  struct sigaction newsigact;
  newsigact.sa_handler = SIG_DFL;
  newsigact.sa_flags = 0;
  sigemptyset(&newsigact.sa_mask);
  sigprocmask(SIG_SETMASK, &newsigact.sa_mask, NULL);
  sigaction(SIGTERM, &newsigact, NULL);
}

// Read in the scheduler_config from disk.
// For now, just set some default values.
static void readSchedConfig(void) {
  schedConfig.schedFreq = 30; //every 30 seconds
}

void *schedulerThread(void * unused) {
  int rc;
  ncMetadata ccMeta;
  ccMeta.correlationId = strdup("scheduler");
  ccMeta.userId = strdup("eucalyptus");

  if (!ccMeta.correlationId || !ccMeta.userId) {
    logsc(EUCAFATAL, "out of memory!\n");
    unlock_exit(1);
  }

  readSchedConfig();

  while(1) {

    logsc(EUCADEBUG, "running\n");
    logsc(EUCADEBUG, "Calling 'schedule()...\n");

    schedule();

    shawn();

    logsc(EUCADEBUG, "done, sleeping for %d\n", schedConfig.schedFreq);

    sleep(schedConfig.schedFreq);
  }



  return NULL;
}

// Compute the theoretical cores utilization percentage
double balanceLevelCores(ccResourceCache * resCache) {
  int i;

  int usedCores = 0, maxCores = 0;
  for (i = 0; i < resCache->numResources; ++i) {
    ccResource * resource = &(resCache->resources[i]);
    usedCores += (resource->maxCores - resource->availCores);
    maxCores += resource->maxCores;
  }

  return (double)usedCores / (double)maxCores;
}

char doesVMFit(ccInstance * VM, ccResource * resource, double balance) {

  int coresUsed = resource->maxCores - resource->availCores;
  int newCoresUsed = coresUsed + VM->ccvm.cores;

  double newBalance = (double)newCoresUsed / (double)resource->maxCores;

  return newBalance < balance;
}

// Sort to put the largest vm first.
static int instanceSort(const void * v1, const void * v2) {
  const ccInstance **vm1 = (const ccInstance**)v1;
  const ccInstance **vm2 = (const ccInstance**)v2;
  return (*vm2)->ccvm.cores - (*vm1)->ccvm.cores;
}

static int resourceSort(const void * v1, const void * v2) {
  const ccResource **r1 = (const ccResource**)v1;
  const ccResource **r2 = (const ccResource**)v2;

  int usedCores1 = (*r1)->maxCores - (*r1)->availCores;
  int usedCores2 = (*r2)->maxCores - (*r2)->availCores;

  return usedCores2 - usedCores1;
}

void schedule() {
  static ccResourceCache resourceCacheLocal;
  static ccInstanceCache instanceCacheLocal;

  logsc(EUCADEBUG, "Entered schedule!\n");

  logsc(EUCADEBUG, "Getting rescache lock...\n");
  sem_mywait(RESCACHE);
  memcpy(&resourceCacheLocal, resourceCache, sizeof(ccResourceCache));
  sem_mypost(RESCACHE);
  logsc(EUCADEBUG, "Getting instcache lock...\n");
  sem_mywait(INSTCACHE);
  memcpy(&instanceCacheLocal, instanceCache, sizeof(ccInstanceCache));
  sem_mypost(INSTCACHE);
  logsc(EUCADEBUG, "Locks released, instance/resource cache copied!\n");

  const int vmCount = instanceCacheLocal.numInsts;
  scheduledVM schedule[vmCount];

  // Call our scheduler...
  char result = scheduler(&resourceCacheLocal, &instanceCacheLocal, &schedule[0]);
  if (result) {
    logsc(EUCAERROR, "Failed to schedule, ignoring\n");
    return;
  }

  // And migrate all instances that aren't already on the hosts indicated by
  // the new schedule.

  int i;
  for (i = 0; i < vmCount; ++i) {
    ccInstance * VM = schedule[i].instance;
    ccResource * currentResource = schedule[i].resource;

    ccResource * targetResource = &resourceCacheLocal.resources[VM->ncHostIdx];
    if (currentResource != targetResource) {
      // TODO KOALA: Actually migrate things!

      // For now, just log the migration
      logsc(EUCAINFO, "Attempting to migrate %s from %s(%s) to %s(%s)\n",
                      VM->instanceId,
                      currentResource->hostname,
                      currentResource->ip,
                      targetResource->hostname,
                      targetResource->ip);
    }
  }
}

// 0 on success, error code on failure.
char balanceSchedule(ccResourceCache * resCache, ccInstanceCache * instCache, scheduledVM* schedule) {
  // TODO KOALA: Algorithm stability??

  double balance = balanceLevelCores(resCache);

  int schedulableCount = instCache->numInsts;
  const int vmCount = schedulableCount;
  const int resCount = resCache->numResources;

  // List of vms we need to schedule.
  ccInstance * vms[vmCount];
  ccResource * nodes[resCount];
  int i, j;
  for (i = 0; i < vmCount; ++i) vms[i] = &instCache->instances[i];
  for (i = 0; i < resCount; ++i) nodes[i] = &resCache->resources[i];

  // Sort resources, put largest resource first.
  qsort(nodes, resCount, sizeof(ccResource*), resourceSort);

  char didSomething;
  do {
    // Go through each of the resources, and greedily assign the largest VM that fits
    // and keeps it under the balance.

    didSomething = 0;
    for(i = 0; (i < resCount) && schedulableCount; ++i) {
      ccResource *targetResource = nodes[i];

      // Get an ordering of the instances, most needy first.
      qsort(vms, schedulableCount, sizeof(ccInstance*), instanceSort);

      // Pick the first VM that fits, and schedule it to this resource.
      for (j = 0; j < vmCount; ++j) {
        ccInstance * VM = vms[j];

        if (doesVMFit(VM, targetResource, balance)) {
          int index = vmCount - schedulableCount;
          schedule[index].instance = VM;
          schedule[index].resource = targetResource;

          vms[j] = vms[schedulableCount-1];
          vms[schedulableCount-1] = VM;

          schedulableCount--;
          didSomething = 1;
          logsc(EUCADEBUG, "Fitting %s to %s\n",
              VM->instanceId,
              targetResource->hostname);
          break;
        }
      }
    }
  } while(didSomething);

  if (schedulableCount) {
    // Okay, we were unable to schedule VMs under the balance, which is expected.
    // However, each instance has to go *somewhere*!

    for(i = 0; (i < resCount) && schedulableCount; ++i) {
      ccResource *targetResource = nodes[i];

      // Get an ordering of the instances, most needy first.
      qsort(vms, schedulableCount, sizeof(ccInstance*), instanceSort);

      // Pick the first VM that fits, and schedule it to this resource.
      for (j = 0; j < vmCount; ++j) {
        ccInstance * VM = vms[j];

        if (doesVMFit(VM, targetResource, 1.0)) {
          int index = vmCount - schedulableCount;
          schedule[index].instance = VM;
          schedule[index].resource = targetResource;

          vms[j] = vms[schedulableCount-1];
          vms[schedulableCount-1] = VM;

          schedulableCount--;
          break;
        }
      }
    }
  }

  if (schedulableCount) {
    logsc(EUCAERROR, "Unschedulable??\n");
    // Do nothing
    return -1;
  }

  return 0;
}

