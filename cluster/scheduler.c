/*
 * ===========================================================================
 *
 *       Filename:  scheduler.cpp
 *
 *    Description:  Scheduler implementation
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

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#include <eucalyptus.h>
#include <data.h>
#include <string.h>
#include <misc.h>
#include <handlers.h>

static void schedule(ncMetadata * ccMeta);

typedef struct {
  ccInstance * instance;
  ccResource * resource;
} scheduledVM;

typedef int (*scAlgo)(ccResourceCache *, ccInstanceCache *, scheduledVM *);

// "Knobs" go here.
typedef struct {
  int schedFreq;
  scAlgo scheduler;
  int debugLog;
} schedConfig_t;

int balanceScheduler(ccResourceCache *, ccInstanceCache *, scheduledVM*);
int groupingScheduler(ccResourceCache *, ccInstanceCache *, scheduledVM*);
int funScheduler(ccResourceCache *, ccInstanceCache *, scheduledVM*);

// Default, does nothing.
int noScheduler(ccResourceCache * unused1, ccInstanceCache * unused2, scheduledVM* unused3) {
  return 0;
}

scAlgo schedTable[] = {
  noScheduler,
  balanceScheduler,
  groupingScheduler,
  funScheduler
};
static int schedTable_size = sizeof(schedTable)/sizeof(schedTable[0]);

// This needs to be same size as above array!!
char * schedTableNames[] = {
  "No scheduler",
  "Balance scheduler",
  "Grouping scheduler",
  "Random scheduler"
};

static schedConfig_t schedConfig;
static ncMetadata ccMeta;
static time_t lastTick;
static time_t adjust;
static unsigned schedId;

#define logsc(LOGLEVEL, formatstr, ...) \
  logprintfl(LOGLEVEL, "schedulerTick(%d): " formatstr, schedId, ##__VA_ARGS__)

#define logsc_dbg(formatstr, ...) \
  do { \
    if (schedConfig.debugLog) \
      logsc(EUCADEBUG, "(dbg %s) " formatstr, __FUNCTION__, ##__VA_ARGS__); \
  } while(0)


// Read the scheduler configuration
static int readSchedConfigFile(void) {

  // Config file:
  // First integer: scheduling frequency
  // Second integer: scheduling policy index
  FILE * f = fopen("/tmp/sched.config", "r");

  if (!f) return 0;

  int freq, policy, debugLog;
  int count = fscanf(f, "%d%d%d", &freq, &policy, &debugLog);
  fclose(f);

  // Sanity checks on input
  if (count != 3) return 0;
  if (freq <= 0) return 0;
  if (policy < 0 || policy >= schedTable_size) return 0;
  if (debugLog != 0 && debugLog != 1) return 0;

  // Okay, sanity checks passed.
  // Apply these settings, logging any changes.

  if (freq != schedConfig.schedFreq) {
    logsc(EUCAINFO, "Changing scheduling frequency from %d to %d\n",
        schedConfig.schedFreq, freq);
    schedConfig.schedFreq = freq;
  }

  scAlgo newAlgo = schedTable[policy];
  if (schedConfig.scheduler != newAlgo) {
    logsc(EUCAINFO, "Changing scheduling algorithm to %s (%d)\n",
        schedTableNames[policy], policy);
    schedConfig.scheduler = newAlgo;
  }

  if (schedConfig.debugLog != debugLog) {
    logsc(EUCAINFO, "Changing scheduler debug logging to %d\n",
      debugLog);
    schedConfig.debugLog = debugLog;
  }

  // Successfully read and updated config
  return 1;
}

static void readSchedConfig(void) {
  if (!readSchedConfigFile()) {
    // Default values
    schedConfig.schedFreq = 30; //every 30 seconds
    schedConfig.scheduler = noScheduler;
    schedConfig.debugLog = 0;
  }
}

static void schedInit(void) {

  static int init = 0;

  // Re-read the config file every time
  readSchedConfig();

  if (init) return;

  logsc(EUCAINFO, "Initializing Migration Scheduler...\n");


  ccMeta.correlationId = strdup("scheduler");
  ccMeta.userId = strdup("eucalyptus");

  if (!ccMeta.correlationId || !ccMeta.userId) {
    logsc(EUCAFATAL, "out of memory!\n");
    unlock_exit(1);
  }

  srand((uintptr_t)&ccMeta + time(NULL));

  lastTick = 0; // Force a run
  adjust = 0;
  schedId = 0;

  init = 1;
}

void schedulerTick(void) {

  schedInit();

  time_t now = time(NULL);
  time_t diff = now - lastTick;
  if (diff < schedConfig.schedFreq) {
    logsc_dbg("Not enough time, sleeping until next tick\n");
    return;
  }

  schedId++; // Track which 'tick' this is, makes log reading easier.

  logsc(EUCADEBUG, "Running, schedFreq: %d, elapsed: %d, adjust %d\n",
    schedConfig.schedFreq, (int)(diff-adjust), (int)(adjust));

  schedule(&ccMeta);

  // If we overshoot by a whole event, just drop it
  adjust = diff % schedConfig.schedFreq;

  // Try to accomodate being calling on ticks that our schedule
  // might not be a clean multiple of, or even be consistent.
  lastTick = now - adjust;
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
  int count = schedConfig.scheduler(&resourceCacheLocal, &instanceCacheLocal, &schedule[0]);
  if (count == 0) {
    logsc(EUCADEBUG, "No VMs/instances scheduled\n");
    return;
  }

  // Scheduler algorithm returned us set of VM's it wants us to move.
  // ...Attempt to do so!

  int i;
  for (i = 0; i < count; ++i) {
    ccInstance * VM = schedule[i].instance;
    ccResource * targetResource = schedule[i].resource;

    ccResource * sourceResource = &resourceCacheLocal.resources[VM->ncHostIdx];
    if (sourceResource == targetResource) {
      logsc(EUCAERROR, "Scheduler indicated we should move %s to resource %s that it's already on??",
          VM->instanceId, targetResource->hostname);
      continue;
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
    else {
      logsc(EUCAINFO, "Migration of %s appears successful!\n", VM->instanceId);
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
  double nn = n + 1;
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

  // Simple algorithm:
  // Find the 'least' used host, and the 'most' used host, and move a VM from the most to the least.

  int i;
  const int resCount = resCache->numResources;
  int * instOrder = randomizedOrder(MAXINSTANCES);
  int * resOrder = randomizedOrder(resCount);

  // Find most and least used resources...
  ccResource *mostUsedResource = NULL, *leastUsedResource = NULL;
  for (i = 0; i < resCount; ++i) {
    ccResource * curResource = &resCache->resources[resOrder[i]];
    logsc_dbg("Looking at %s (Util %f)\n",
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

    for(i = 0; i < MAXINSTANCES; ++i) {
      ccInstance * curInst = &instCache->instances[instOrder[i]];
      ccResource * curResource = &resCache->resources[curInst->ncHostIdx];

      // Only consider valid cache entries
      if (instCache->cacheState[instOrder[i]] != INSTVALID) continue;
      logsc_dbg("Looking at instance %s on %s...\n",
        curInst->instanceId, curResource->hostname);

      if (!isSchedulable(curInst)) continue;

      // If this is an instance running on 'mostUsedResource'
      if (curResource == mostUsedResource) {
        // Is this worth moving?

        double util1 = resourceCoreUtil(mostUsedResource);
        double util2 = resourceCoreUtil(leastUsedResource);

        int newCoresUsed = leastUsedResource->maxCores - leastUsedResource->availCores + curInst->ccvm.cores;
        double newUtil = (double)newCoresUsed / (double)leastUsedResource->maxCores;

        logsc_dbg("Cores: %d, %d, %d\n", leastUsedResource->maxCores, leastUsedResource->availCores, curInst->ccvm.cores);
        logsc_dbg("Util1: %f, Util2: %f, newUtil: %f\n", util1, util2, newUtil);

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

  int i, j, k;

  const int resCount = resCache->numResources;
  int * instOrder = randomizedOrder(MAXINSTANCES);
  int * resOrder = randomizedOrder(resCount);
  int * resOrder2 = randomizedOrder(resCount);
  // Find random resource pairings...

  // Find a resource to migrate from...
  for (i = 0; i < resCount; ++i) {
    ccResource *sourceResource = &resCache->resources[resOrder[i]];
    logsc_dbg("Looking at %s\n", sourceResource->hostname);

    // Go through all instances, considering those that are on sourceResource
    // (no good way to just get resource->instance listing)
    for (j = 0; j < MAXINSTANCES; ++j) {
      ccInstance * curInst = &instCache->instances[instOrder[j]];
      ccResource * curResource = &resCache->resources[curInst->ncHostIdx];

      // Only consider valid cache entries
      if (instCache->cacheState[instOrder[j]] != INSTVALID) continue;

      logsc_dbg("Looking at %s (on %s)\n", curInst->instanceId, curResource->hostname);

      if (!isSchedulable(curInst)) continue;

      // If this is an instance running on 'sourceResource'
      if (curResource == sourceResource) {

        // Find some resource that can take it...
        for (k = 0; k < resCount; ++k) {

          ccResource * targetResource = &resCache->resources[resOrder2[k]];
          logsc_dbg("Considering moving %s from %s to %s...\n",
              curInst->instanceId, sourceResource->hostname, targetResource->hostname);

          // Skip over the one we're hoping to migrate *from*
          if (targetResource == sourceResource) continue;

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


  // Simple algorithm:
  // Find the 'least' used host, and try to move any of its instances to the most utilized resouce that can hold it.
  // Not perfect, but good enough for now--generally just tries to put instances together.
  // Should try for each resource, STARTING with the least used, but moving up from there.  For now just gives up.

  int i, j;

  const int resCount = resCache->numResources;
  int * instOrder = randomizedOrder(MAXINSTANCES);
  int * resOrder = randomizedOrder(resCount);
  int * resOrder2 = randomizedOrder(resCount);

  // Find least used resource...
  ccResource *leastUsedResource = NULL;
  for (i = 0; i < resCount; ++i) {
    ccResource * curResource = &resCache->resources[resOrder[i]];
    logsc_dbg("Looking at %s (Util %f)\n",
        curResource->hostname, resourceCoreUtil(curResource));

    // We only are interested in resources that have instances to be moved...
    if (curResource->availCores == curResource->maxCores) continue;

    if (!leastUsedResource || (balanceCompare(curResource, leastUsedResource) < 0.0)) {
      leastUsedResource = curResource;
    }

  }

  if (!leastUsedResource) {
    logsc_dbg("Failed to find least used resource!\n");
    return 0;
  }

  logsc_dbg("Least used resource is %s\n", leastUsedResource->hostname);

  // Find instances for this resource...
  for(i = 0; i < MAXINSTANCES; ++i) {
    ccInstance * curInst = &instCache->instances[instOrder[i]];
    ccResource * curResource = &resCache->resources[curInst->ncHostIdx];

    // Only consider valid cache entries
    if (instCache->cacheState[instOrder[i]] != INSTVALID) continue;

    if (!isSchedulable(curInst)) continue;

    // If this is an instance running on 'leastUsedResource'
    if (curResource == leastUsedResource) {

      // Find the /most/ used resource that this instance fits on
      ccResource * mostUsedResource = NULL;
      for (j = 0; j < resCount; ++j) {

        logsc_dbg("Index: %d\n", resOrder2[j]);
        ccResource * candidateTargetResource = &resCache->resources[resOrder2[j]];

        logsc_dbg("Target Looking at %s (Util %f)\n",
            candidateTargetResource->hostname, resourceCoreUtil(candidateTargetResource));

        // Skip over the one we're hoping to migrate *from*
        if (candidateTargetResource == curResource) continue;

        // Can this VM fit on this resource *anyway*? If not, skip.
        if (candidateTargetResource->availCores < curInst->ccvm.cores) continue;

        // Okay, well see if it's the most used such resource...
        if (!mostUsedResource || (balanceCompare(candidateTargetResource, mostUsedResource) > 0.0)) {
          mostUsedResource = candidateTargetResource;
        }
      }

      if (mostUsedResource) {
        double util1 = resourceCoreUtil(mostUsedResource);
        double util2 = resourceCoreUtil(leastUsedResource);

        int newCoresUsed = mostUsedResource->maxCores - mostUsedResource->availCores + curInst->ccvm.cores;
        double newUtil = (double)newCoresUsed / (double)mostUsedResource->maxCores;

        logsc_dbg("Cores: %d, %d, %d\n", leastUsedResource->maxCores, leastUsedResource->availCores, curInst->ccvm.cores);
        logsc_dbg("Util1: %f, Util2: %f, newUtil: %f\n", util1, util2, newUtil);

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
