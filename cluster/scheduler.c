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

// For (temporary) simplicity's sake
// assume we never have more than this many instances
#define SCHED_INSTANCE_MAX 100
// Same for resources.
#define SCHED_RESOURCE_MAX 10

static void schedule(ncMetadata * ccMeta);

typedef struct {
  ccInstance * instance;
  ccResource * resource;
} scheduledVM;

typedef int (*scAlgo)(scheduledVM *);

// "Knobs" go here.
typedef struct {
  int schedFreq;
  scAlgo scheduler;
  int debugLog;
} schedConfig_t;

int balanceScheduler(scheduledVM*);
int groupingScheduler(scheduledVM*);
int funScheduler(scheduledVM*);
int dynScheduler(scheduledVM*);

// Default, does nothing.
int noScheduler(scheduledVM* unused) {
  return 0;
}

scAlgo schedTable[] = {
  noScheduler,
  balanceScheduler,
  groupingScheduler,
  funScheduler,
  dynScheduler
};
static int schedTable_size = sizeof(schedTable)/sizeof(schedTable[0]);

// This needs to be same size as above array!!
char * schedTableNames[] = {
  "(No scheduler)",
  "Balance scheduler",
  "Grouping scheduler",
  "Random scheduler",
  "Dynamic Feedback-Driven Scheduler"
};

static schedConfig_t schedConfig;
static ncMetadata ccMeta;
static time_t lastTick;
static time_t adjust;
static unsigned schedId;

ccInstanceCache *schedInstanceCache;
ccResourceCache *schedResourceCache;

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
    lastTick = 0; adjust = 0;
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

  // TODO: Singleton initialization, lock!!
  // (Shared-memory lock, like the others..)
  if (init) return;

  logsc(EUCAINFO, "Initializing Migration Scheduler...\n");

  ccMeta.correlationId = strdup("scheduler");
  ccMeta.userId = strdup("eucalyptus");

  if (!ccMeta.correlationId || !ccMeta.userId) {
    logsc(EUCAFATAL, "out of memory!\n");
    unlock_exit(1);
  }

  srand((uintptr_t)&ccMeta + time(NULL));

  lastTick = 0;
  adjust = 0;
  schedId = 0;

  // Allocate our caches...
  schedResourceCache = (ccResourceCache*)malloc(sizeof(ccResourceCache));
  schedInstanceCache = (ccInstanceCache*)malloc(sizeof(ccInstanceCache));
  //TODO: Initialize the caches as invalid?

  init = 1;
}

void schedulerTick(void) {

  sem_mywait(SCHEDULER);
  schedInit();

  time_t now = time(NULL);
  time_t diff = now - lastTick;
  if (diff < schedConfig.schedFreq - adjust) {
    logsc_dbg("Not enough time, sleeping until next tick\n");
    sem_mypost(SCHEDULER);
    return;
  }

  schedId++; // Track which 'tick' this is, makes log reading easier.

  logsc(EUCADEBUG, "Running, schedFreq: %d, elapsed: %d, adjust %d\n",
    schedConfig.schedFreq, (int)(diff), (int)(adjust));

  schedule(&ccMeta);

  if (lastTick != 0) {
    // Try to accomodate being calling on ticks that our schedule
    // might not be a clean multiple of, or even be consistent.
    adjust = diff - (schedConfig.schedFreq - adjust);

    // Never adjust by more than half the period
    // If we overshoot by more than this, we don't care (incl dropping ticks)
    int maxval = schedConfig.schedFreq/2;
    if (adjust < -maxval) adjust = -maxval;
    if (adjust > maxval) adjust = maxval;
  }

  lastTick = now;

  sem_mypost(SCHEDULER);
}

void updateSchedResCache(void) {
  sem_mywait(RESCACHE);
  memcpy(schedResourceCache, resourceCache, sizeof(ccResourceCache));
  sem_mypost(RESCACHE);
}

void updateSchedInstCache(void) {

  // (Stolen from/Inspired by handlers.c's doDescribeInstances)
  // Not memcpy'ing the whole thing is useful, by checking the cacheState array
  // we avoid bringing in the entire (HUGE) instances array
  // (either from instanceCache or from schedInstanceCache)
  // This has huge memory savings.

  sem_mywait(INSTCACHE);

  int i, count = 0;
  if (instanceCache->numInsts) {

    for (i=0; i<MAXINSTANCES; i++) {
      if (instanceCache->cacheState[i] == INSTVALID) {
        if (count >= instanceCache->numInsts) {
          logsc(EUCAWARN, "found more instances than reported by numInsts, will only schedule on a subset of instances\n");
          count=0;
        }
        memcpy(&(schedInstanceCache->instances[count]),
               &(instanceCache->instances[i]),
               sizeof(ccInstance));
        count++;
      }
    }

    schedInstanceCache->numInsts = instanceCache->numInsts;
  }
  sem_mypost(INSTCACHE);
}

void schedule(ncMetadata * ccMeta) {

  updateSchedResCache();
  updateSchedInstCache();

  const int vmCount = schedInstanceCache->numInsts;
  scheduledVM schedule[vmCount];

  // Call our scheduler...
  int count = schedConfig.scheduler(&schedule[0]);
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

    ccResource * sourceResource = &schedResourceCache->resources[VM->ncHostIdx];
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
    //int result = doMigrateInstance(ccMeta, VM->instanceId, sourceResource->hostname, targetResource->hostname);
    int result = 1; // ERROR
    logsc(EUCAINFO, "Temporarily have migration disabled, ignore error below\n");

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
int balanceScheduler(scheduledVM* schedule) {
  // TODO KOALA: Algorithm stability??

  // Simple algorithm:
  // Find the 'least' used host, and the 'most' used host, and move a VM from the most to the least.

  int i;
  const int resCount = schedResourceCache->numResources;
  int * instOrder = randomizedOrder(MAXINSTANCES);
  int * resOrder = randomizedOrder(resCount);

  // Find most and least used resources...
  ccResource *mostUsedResource = NULL, *leastUsedResource = NULL;
  for (i = 0; i < resCount; ++i) {
    ccResource * curResource = &schedResourceCache->resources[resOrder[i]];
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
      ccInstance * curInst = &schedInstanceCache->instances[instOrder[i]];
      ccResource * curResource = &schedResourceCache->resources[curInst->ncHostIdx];

      // Only consider valid cache entries
      if (schedInstanceCache->cacheState[instOrder[i]] != INSTVALID) continue;
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
int funScheduler(scheduledVM* schedule) {

  int i, j, k;

  const int resCount = schedResourceCache->numResources;
  int * instOrder = randomizedOrder(MAXINSTANCES);
  int * resOrder = randomizedOrder(resCount);
  int * resOrder2 = randomizedOrder(resCount);
  // Find random resource pairings...

  // Find a resource to migrate from...
  for (i = 0; i < resCount; ++i) {
    ccResource *sourceResource = &schedResourceCache->resources[resOrder[i]];
    logsc_dbg("Looking at %s\n", sourceResource->hostname);

    // Go through all instances, considering those that are on sourceResource
    // (no good way to just get resource->instance listing)
    for (j = 0; j < MAXINSTANCES; ++j) {
      ccInstance * curInst = &schedInstanceCache->instances[instOrder[j]];
      ccResource * curResource = &schedResourceCache->resources[curInst->ncHostIdx];

      // Only consider valid cache entries
      if (schedInstanceCache->cacheState[instOrder[j]] != INSTVALID) continue;

      logsc_dbg("Looking at %s (on %s)\n", curInst->instanceId, curResource->hostname);

      if (!isSchedulable(curInst)) continue;

      // If this is an instance running on 'sourceResource'
      if (curResource == sourceResource) {

        // Find some resource that can take it...
        for (k = 0; k < resCount; ++k) {

          ccResource * targetResource = &schedResourceCache->resources[resOrder2[k]];
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
int groupingScheduler(scheduledVM* schedule) {
  // TODO KOALA: Algorithm stability??


  // Simple algorithm:
  // Find the 'least' used host, and try to move any of its instances to the most utilized resouce that can hold it.
  // Not perfect, but good enough for now--generally just tries to put instances together.
  // Should try for each resource, STARTING with the least used, but moving up from there.  For now just gives up.

  int i, j;

  const int resCount = schedResourceCache->numResources;
  int * instOrder = randomizedOrder(MAXINSTANCES);
  int * resOrder = randomizedOrder(resCount);
  int * resOrder2 = randomizedOrder(resCount);

  // Find least used resource...
  ccResource *leastUsedResource = NULL;
  for (i = 0; i < resCount; ++i) {
    ccResource * curResource = &schedResourceCache->resources[resOrder[i]];
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
    ccInstance * curInst = &schedInstanceCache->instances[instOrder[i]];
    ccResource * curResource = &schedResourceCache->resources[curInst->ncHostIdx];

    // Only consider valid cache entries
    if (schedInstanceCache->cacheState[instOrder[i]] != INSTVALID) continue;

    if (!isSchedulable(curInst)) continue;

    // If this is an instance running on 'leastUsedResource'
    if (curResource == leastUsedResource) {

      // Find the /most/ used resource that this instance fits on
      ccResource * mostUsedResource = NULL;
      for (j = 0; j < resCount; ++j) {

        logsc_dbg("Index: %d\n", resOrder2[j]);
        ccResource * candidateTargetResource = &schedResourceCache->resources[resOrder2[j]];

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

typedef struct {
  // 0 to 100 cpu utilization
  int cpuUtil;
} nodeInfo_t;

typedef struct {
  // 0 to 100 cpu utilization
  int cpuUtil;
} instInfo_t;

// Contains the dynamic monitoring information of the nodes/instances
typedef struct {
  nodeInfo_t nodeInfo[SCHED_RESOURCE_MAX];
  instInfo_t instInfo[SCHED_INSTANCE_MAX];
} monitorInfo_t;

// Contains the mapping of instances to their hosting nodes/owners.
typedef struct {
  int instOwner[SCHED_INSTANCE_MAX];
} schedule_t;

void readSystemState(monitorInfo_t * m) {
  // TODO: Read this from a file!

  // Eventually this would be dynamically reported by the nc's
  // (for themselves and their instances)

  const int resCount = schedResourceCache->numResources;
  const int instCount = schedInstanceCache->numInsts;

  // Initialize all values...
  memset(m,0,sizeof(monitorInfo_t));

  FILE * f = fopen("/tmp/monitor.config", "r");
  if (!f) return;

  // TODO: Trust the file (and it being formatted correctly)
  // significantly less, esp regarding allocations/etc...

  int i;
  char line[1024];
  while (fgets(line, sizeof(line), f)) {

    char buf[sizeof(line)];
    int val;
    int count = sscanf(line, "%s %d\n", buf, &val);

    if (count == 2) {

      for (i = 0; i < resCount; ++i) {
        if (!strcmp(buf, schedResourceCache->resources[i].ip)) {
          logsc_dbg("Updating resource %s with cpu value %d\n", buf, val);
          m->nodeInfo[i].cpuUtil = val;
        }
      }

      for (i = 0; i < instCount; ++i) {
        if (!strcmp(buf, schedInstanceCache->instances[i].instanceId)) {
          logsc_dbg("Updating instance %s with cpu value %d\n", buf, val);
          m->nodeInfo[i].cpuUtil = val;
          break;
        }
      }

    }
  }

  fclose(f);

}

int scoreSystem(monitorInfo_t * m, schedule_t * s) {
  // TODO parameterize this scoring so that we can play with it.

  //TODO: Make me do anything at all!
  return 10;

}

int migrationCost(monitorInfo_t * m, schedule_t * s, int instId, int targetNode) {
  // Computes cost of migration instance from where it is to the specified node

  // TODO: Actually do this.
  return 10; // Magic, arbitrary, etc.
}

int dynScheduler(scheduledVM* schedule) {

  // Algorithm:
  // Score the system, as sum of 'suitability' scores of each node with respect to the instances on it.
  // For each possible migration:
  //   Calculate cost of migration itself
  // If the score of the new system outweighs the cost of migration, then do it!

  const int resCount = schedResourceCache->numResources;
  const int instCount = schedInstanceCache->numInsts;
  int i, j;

  // Get dynamic monitoring information about the system (from wherever)
  monitorInfo_t monitorInfo;
  readSystemState(&monitorInfo);

  // Build the 'schedule' for the existing system...
  schedule_t system;
  for (i = 0; i < instCount; ++i) {
    system.instOwner[i] = schedInstanceCache->instances[i].ncHostIdx;
  }

  // Score the system.
  int baseline = scoreSystem(&monitorInfo, &system);
  logsc_dbg("Baseline score: %d\n", baseline);

  // Find the highest scoring schedule using a single migration
  int best_score = baseline;

  for(i = 0; i < instCount; ++i) {
      schedule_t testing = system;

      for (j = 0; j < resCount; ++j) {
        testing.instOwner[i] = j;

        int new_system = scoreSystem(&monitorInfo, &testing);
        int migrate_cost = migrationCost(&monitorInfo, &testing, i, j);
        int new_score = new_system - migrate_cost;

        logsc_dbg("If we moved %s to %s, score would change from %d to %d\n",
          schedInstanceCache->instances[i].instanceId,
          schedResourceCache->resources[j].ip,
          baseline,
          new_score);

        if (new_score > best_score) {
          best_score = new_score;

          // Indicate that we should do this.
          // Note that if we find a better one later, we'll overwrite this.
          schedule[0].instance = &schedInstanceCache->instances[i];
          schedule[0].resource = &schedResourceCache->resources[j];
        }
      }
  }

  return best_score > baseline;
}
