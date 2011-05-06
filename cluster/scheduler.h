/*
 * ===========================================================================
 *
 *       Filename:  scheduler.h
 *
 *    Description:  Header file for scheduler implementation
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

#ifndef _SCHEDULER_H_

#include <handlers.h>

void schedulerTick(void);

extern ccInstanceCache *schedInstanceCache;

extern ccResourceCache *schedResourceCache;

#endif // _SCHEDULER_H_
