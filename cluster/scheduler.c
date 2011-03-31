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
                    Kevin Larson (KL), kevinlarson1@gmail.com
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
  int scheduling_frequency;
} scheduler_config;

static scheduler_config sc;

static void set_signal_handler(void) {
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
static void read_sc(void) {
  sc.scheduling_frequency = 30; //every 30 seconds
}

void *scheduler_thread(void * unused) {
  int rc;
  ncMetadata ccMeta;
  ccMeta.correlationId = strdup("scheduler");
  ccMeta.userId = strdup("eucalyptus");

  if (!ccMeta.correlationId || !ccMeta.userId) {
    logprintfl(EUCAFATAL, "scheduler_thread(): out of memory!\n");
    unlock_exit(1);
  }

  read_sc();

  while(1) {
    set_signal_handler();

    logprintfl(EUCADEBUG, "scheduler_thread(): running\n");

    schedule();

    shawn();

    logprintfl(EUCADEBUG, "scheduler_thread(): done\n");
    sleep(sc.scheduling_frequency);
  }

  return(NULL);
}

void schedule() {
   // TODO KOALA: Implement me!
    logprintfl(EUCADEBUG, "scheduler_thread(): SCHEDULING MAGIC!\n");
}
