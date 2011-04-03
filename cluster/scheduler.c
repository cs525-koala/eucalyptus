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

int balanceScheduler(ccResourceCache *, ccInstanceCache *, scheduledVM*);
int groupingScheduler(ccResourceCache *, ccInstanceCache *, scheduledVM*);
int funScheduler(ccResourceCache *, ccInstanceCache *, scheduledVM*);
scAlgo scheduler = funScheduler;


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

  srand((uintptr_t)&ccMeta + time(NULL));

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
    logsc(EUCADEBUG, "No nodes scheduled\n");
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

char isSchedulable(ccInstance *inst) {
  // An instance is only schedulable if it's running
  return inst && !strcmp(inst->state, "Extant");
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

int randZeroAnd(int n) {
  double nn = n;
  return (int)(nn * (rand() / (RAND_MAX + 1.0)));
}

int* randomizedOrder(int count) {
  int * array = malloc(count*sizeof(int));
  int i;

  for (i = 0; i < count; ++i) {
    array[i] = i;
  }

  for (i = count -1; i >= 0; --i) {
    int swap = randZeroAnd(i);

    int tmp = array[swap];
    array[swap] = array[i];
    array[i] = tmp;
  }

  return array;
}

// Returns count of VMs the scheduler wants to move.
int balanceScheduler(ccResourceCache * resCache, ccInstanceCache * instCache, scheduledVM* schedule) {
  // TODO KOALA: Algorithm stability??

  const int resCount = resCache->numResources;

  // Simple algorithm:
  // Find the 'least' used host, and the 'most' used host, and move a VM from the most to the least.

  int i;
  int * instOrder = randomizedOrder(instCache->numInsts);
  int * resOrder = randomizedOrder(resCache->numResources);

  // Find most and least used resources...
  ccResource *mostUsedResource = NULL, *leastUsedResource = NULL;
  for (i = 0; i < resCount; ++i) {
    ccResource * curResource = &resCache->resources[resOrder[i]];
    logsc(EUCADEBUG, "Looking at %s (Util %f)\n",
        curResource->hostname, resourceCoreUtil(curResource));

    if (!mostUsedResource || (balanceCompare(curResource, mostUsedResource) > 0.0)) {
      mostUsedResource = curResource;
    }
    if (!leastUsedResource || (balanceCompare(curResource, leastUsedResource) < 0.0)) {
      leastUsedResource = curResource;
    }

  }

  if (mostUsedResource) logsc(EUCADEBUG, "Most used resource is %s\n", mostUsedResource->hostname);
  if (leastUsedResource) logsc(EUCADEBUG, "Least used resource is %s\n", leastUsedResource->hostname);

  if (mostUsedResource && leastUsedResource && (mostUsedResource != leastUsedResource)) {
    // TODO KOALA: Try to find 'largest' such instance?
    // For now, just find any instance that is 'worth' moving

    for(i = 0; i < instCache->numInsts; ++i) {
      ccInstance * curInst = &instCache->instances[instOrder[i]];
      ccResource * curResource = &resCache->resources[curInst->ncHostIdx];

      if (!isSchedulable(curInst)) continue;

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

          free(instOrder);
          free(resOrder);

          return 1; // We found 1 VM to move.
        }
      }
    }
  }

  free(instOrder);
  free(resOrder);

  // If we got this far, we didn't find something to schedule.  Better luck next time!
  return 0;
}

// Returns count of VMs the scheduler wants to move.
// This schedule is just to have fun while we're writing the paper O:)
int funScheduler(ccResourceCache * resCache, ccInstanceCache * instCache, scheduledVM* schedule) {

  const int resCount = resCache->numResources;

  int i, j;


  int * instOrder = randomizedOrder(instCache->numInsts);
  int * resOrder = randomizedOrder(resCache->numResources);
  int * resOrder2 = randomizedOrder(resCache->numResources);
  // Find random resource pairings...

  // Find a resource to migrate from...
  ccResource *sourceResource = NULL;
  for (i = 0; i < resCount; ++i) {
    ccResource * curResource = &resCache->resources[resOrder[i]];

    // Go through all instances, considering those that are on sourceResource
    // (no good way to just get resource->instance listing)
    for (i = 0; i < instCache->numInsts; ++i) {
      ccInstance * curInst = &instCache->instances[instOrder[i]];
      ccResource * curResource = &resCache->resources[curInst->ncHostIdx];

      if (!isSchedulable(curInst)) continue;

      // If this is an instance running on 'sourceResource'
      if (curResource == sourceResource) {

        // Find some resource that can take it...
        for (j = 0; j < resCache->numResources; ++j) {
          // Skip over the one we're hoping to migrate *from*
          if(j == curInst->ncHostIdx) continue;

          ccResource * targetResource = &resCache->resources[resOrder2[j]];

          int newCoresUsed = targetResource->maxCores - targetResource->availCores + curInst->ccvm.cores;
          double newUtil = (double)newCoresUsed / (double)targetResource->maxCores;

          // Can this resource take the VM in question?
          if (newUtil <= 1.0) {
            // Okay, we have a winner!
            schedule[0].instance = curInst;
            schedule[0].resource = targetResource;

            free(instOrder);
            free(resOrder);
            free(resOrder2);

            return 1; // We found 1 VM to move.
          }
        }
      }
    }
  }

  free(instOrder);
  free(resOrder);
  free(resOrder2);

  // If we got this far, we didn't find something to schedule.  Better luck next time!
  return 0;
}

// Attempts to put move machines from less used ones to more used ones
int groupingScheduler(ccResourceCache * resCache, ccInstanceCache * instCache, scheduledVM* schedule) {
  // TODO KOALA: Algorithm stability??

  const int resCount = resCache->numResources;

  // Simple algorithm:
  // Find the 'least' used host, and try to move any of its instances to the most utilized resouce that can hold it.
  // Not perfect, but good enough for now--generally just tries to put instances together.
  // Should try for each resource, STARTING with the least used, but moving up from there.  For now just gives up.

  int i, j;

  int * instOrder = randomizedOrder(instCache->numInsts);
  int * resOrder = randomizedOrder(resCache->numResources);
  int * resOrder2 = randomizedOrder(resCache->numResources);

  // Find least used resource...
  ccResource *leastUsedResource = NULL;
  for (i = 0; i < resCount; ++i) {
    ccResource * curResource = &resCache->resources[resOrder[i]];
    logsc(EUCADEBUG, "Looking at %s (Util %f)\n",
        curResource->hostname, resourceCoreUtil(curResource));

    // We only are interested in resources that have instances to be moved...
    if (curResource->availCores == curResource->maxCores) continue;

    if (!leastUsedResource || (balanceCompare(curResource, leastUsedResource) < 0.0)) {
      leastUsedResource = curResource;
    }

  }

  if (!leastUsedResource) {
    logsc(EUCADEBUG, "Failed to find least used resource!\n");
    return 0;
  }

  logsc(EUCADEBUG, "Least used resource is %s\n", leastUsedResource->hostname);

  // Find instances for this resource...
  for(i = 0; i < instCache->numInsts; ++i) {
    ccInstance * curInst = &instCache->instances[instOrder[i]];
    ccResource * curResource = &resCache->resources[curInst->ncHostIdx];

    if (!isSchedulable(curInst)) continue;

    // If this is an instance running on 'leastUsedResource'
    if (curResource == leastUsedResource) {

      // Find the /most/ used resource that this instance fits on
      ccResource * mostUsedResource = NULL;
      for (j = 0; j < resCache->numResources; ++j) {
        // Skip over the one we're hoping to migrate *from*
        if(j == curInst->ncHostIdx) continue;

        ccResource * candidateTargetResource = &resCache->resources[resOrder2[j]];

        // Can this VM fit on this resource *anyway*? If not, skip.
        if (candidateTargetResource->availCores < curInst->ccvm.cores) continue;

        // Okay, well see if it's the most used such resource...
        if (!mostUsedResource || (balanceCompare(curResource, mostUsedResource) > 0.0)) {
          mostUsedResource = candidateTargetResource;
        }
      }

      if (mostUsedResource) {
        double util1 = resourceCoreUtil(mostUsedResource);
        double util2 = resourceCoreUtil(leastUsedResource);

        int newCoresUsed = mostUsedResource->maxCores - mostUsedResource->availCores + curInst->ccvm.cores;
        double newUtil = (double)newCoresUsed / (double)mostUsedResource->maxCores;

        if (util2 < newUtil) {
          // Found one!
          schedule[0].instance = curInst;
          schedule[0].resource = mostUsedResource;

          free(instOrder);
          free(resOrder);
          free(resOrder2);

          return 1;
        }
      }
    }
  }

  free(instOrder);
  free(resOrder);
  free(resOrder2);

  // If we got this far, we didn't find something to schedule.  Better luck next time!
  return 0;
}
