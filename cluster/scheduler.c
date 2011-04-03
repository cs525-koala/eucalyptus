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

static void schedule(ncMetadata * ccMeta);

// "Knobs" go here.
typedef struct {
  int schedFreq;
} schedConfig_t;

typedef struct {
  ccInstance * instance;
  ccResource * resource;
} scheduledVM;

typedef int (*scAlgo)(ccResourceCache *, ccInstanceCache *, scheduledVM *);

int balanceSchedule(ccResourceCache *, ccInstanceCache *, scheduledVM*);
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

    schedule(&ccMeta);

    shawn();

    logsc(EUCADEBUG, "done, sleeping for %d\n", schedConfig.schedFreq);

    sleep(schedConfig.schedFreq);
  }



  return NULL;
}

void schedule(ncMetadata * ccMeta) {
  static ccResourceCache resourceCacheLocal;
  static ccInstanceCache instanceCacheLocal;

  sem_mywait(RESCACHE);
  memcpy(&resourceCacheLocal, resourceCache, sizeof(ccResourceCache));
  sem_mypost(RESCACHE);
  sem_mywait(INSTCACHE);
  memcpy(&instanceCacheLocal, instanceCache, sizeof(ccInstanceCache));
  sem_mypost(INSTCACHE);

  const int vmCount = instanceCacheLocal.numInsts;
  scheduledVM schedule[vmCount];

  // Call our scheduler...
  int count = scheduler(&resourceCacheLocal, &instanceCacheLocal, &schedule[0]);
  if (count == 0) {
    logsc(EUCAINFO, "No nodes scheduled\n");
    return;
  }

  // And migrate all instances that aren't already on the hosts indicated by
  // the new schedule.

  int i;
  for (i = 0; i < count; ++i) {
    ccInstance * VM = schedule[i].instance;
    ccResource * targetResource = schedule[i].resource;

    ccResource * sourceResource = &resourceCacheLocal.resources[VM->ncHostIdx];
    if (sourceResource == targetResource) {
      logsc(EUCAERROR, "Scheduler indicated we should move %s to resource %s that it's already on??",
          VM->instanceId, targetResource->hostname);
      return;
    }

    logsc(EUCAINFO, "Attempting to migrate %s from %s(%s) to %s(%s)\n",
        VM->instanceId,
        sourceResource->hostname,
        sourceResource->ip,
        targetResource->hostname,
        targetResource->ip);

    // Migrate the VM!
    int result = doMigrateInstance(ccMeta, VM->instanceId, sourceResource->hostname, targetResource->hostname);

    if (result) {
      logsc(EUCAERROR, "Error migrating %s from %s to %s!\n",
          VM->instanceId,
          sourceResource->hostname,
          targetResource->hostname);
    }
  }
}

// Return comparison function of the two.
// For now, returns their core utilization
// Result is >=0 is 'resource1' is 'more used' than 'resource2'
double resourceCoreUtil(ccResource * R) { return (double)(R->maxCores - R->availCores) / (double)R->maxCores; }
double balanceCompare(ccResource * resource1, ccResource * resource2) {
  double util1 = resourceCoreUtil(resource1);
  double util2 = resourceCoreUtil(resource2);
  return util1 - util2;
}

// Returns count of VMs the scheduler wants to move.
int balanceSchedule(ccResourceCache * resCache, ccInstanceCache * instCache, scheduledVM* schedule) {
  // TODO KOALA: Algorithm stability??

  const int resCount = resCache->numResources;

  // Simple algorithm:
  // Find the 'least' used host, and the 'most' used host, and move a VM from the most to the least.

  int i;

  // Find most and least used resources...
  ccResource *mostUsedResource = NULL, *leastUsedResource = NULL;
  for (i = 0; i < resCount; ++i) {
    ccResource * curResource = &resCache->resources[i];
    logsc(EUCAINFO, "Looking at %s\n", curResource->hostname);

    if (!mostUsedResource || (balanceCompare(curResource, mostUsedResource) > 0.0)) {
      mostUsedResource = curResource;
    }
    if (!leastUsedResource || (balanceCompare(curResource, leastUsedResource) < 0.0)) {
      leastUsedResource = curResource;
    }

  }

  if (mostUsedResource) logsc(EUCAINFO, "Most used resource is %s\n", mostUsedResource->hostname);
  if (leastUsedResource) logsc(EUCAINFO, "Least used resource is %s\n", leastUsedResource->hostname);

  if (mostUsedResource && leastUsedResource && (mostUsedResource != leastUsedResource)) {
    // TODO KOALA: Try to find 'largest' such instance?
    // For now, just find any instance that is 'worth' moving

    for(i = 0; i < instCache->numInsts; ++i) {
      ccInstance * curInst = &instCache->instances[i];
      ccResource * curResource = &resCache->resources[curInst->ncHostIdx];

      // If this is an instance running on 'mostUsedResource'
      if (curResource == mostUsedResource) {
        // Is this worth moving?

        double util1 = resourceCoreUtil(mostUsedResource);
        double util2 = resourceCoreUtil(leastUsedResource);

        int newCoresUsed = leastUsedResource->maxCores - leastUsedResource->availCores + curInst->ccvm.cores;
        double newUtil = (double)newCoresUsed / (double)leastUsedResource->maxCores;

        logsc(EUCADEBUG, "Cores: %d, %d, %d\n", leastUsedResource->maxCores, leastUsedResource->availCores, curInst->ccvm.cores);
        logsc(EUCADEBUG, "Util1: %f, Util2: %f, newUtil: %f\n", util1, util2, newUtil);

        if((util1 > newUtil) // Does moving this make sense? This check should also provide some sense of stability.
            && (newUtil < 1.0)) { // Can this node receive this VM?
          // Okay, we have a winner!
          schedule[0].instance = curInst;
          schedule[0].resource = leastUsedResource;

          return 1; // We found 1 VM to move.
        }
      }
    }
  }

  // If we got this far, we didn't find something to schedule.  Better luck next time!
  return 0;
}

