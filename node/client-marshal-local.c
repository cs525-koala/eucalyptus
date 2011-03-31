/*
Copyright (c) 2009  Eucalyptus Systems, Inc.	

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by 
the Free Software Foundation, only version 3 of the License.  
 
This file is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.  

You should have received a copy of the GNU General Public License along
with this program.  If not, see <http://www.gnu.org/licenses/>.
 
Please contact Eucalyptus Systems, Inc., 130 Castilian
Dr., Goleta, CA 93101 USA or visit <http://www.eucalyptus.com/licenses/> 
if you need additional information or have any questions.

This file may incorporate work covered under the following copyright and
permission notice:

  Software License Agreement (BSD License)

  Copyright (c) 2008, Regents of the University of California
  

  Redistribution and use of this software in source and binary forms, with
  or without modification, are permitted provided that the following
  conditions are met:

    Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

    Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
  IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
  TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
  PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
  OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. USERS OF
  THIS SOFTWARE ACKNOWLEDGE THE POSSIBLE PRESENCE OF OTHER OPEN SOURCE
  LICENSED MATERIAL, COPYRIGHTED MATERIAL OR PATENTED MATERIAL IN THIS
  SOFTWARE, AND IF ANY SUCH MATERIAL IS DISCOVERED THE PARTY DISCOVERING
  IT MAY INFORM DR. RICH WOLSKI AT THE UNIVERSITY OF CALIFORNIA, SANTA
  BARBARA WHO WILL THEN ASCERTAIN THE MOST APPROPRIATE REMEDY, WHICH IN
  THE REGENTS’ DISCRETION MAY INCLUDE, WITHOUT LIMITATION, REPLACEMENT
  OF THE CODE SO IDENTIFIED, LICENSING OF THE CODE SO IDENTIFIED, OR
  WITHDRAWAL OF THE CODE CAPABILITY TO THE EXTENT NEEDED TO COMPLY WITH
  ANY SUCH LICENSES OR RIGHTS.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HANDLERS_FANOUT
#include "handlers.h"
#include "client-marshal.h"

ncStub * ncStubCreate (char *endpoint_uri, char *logfile, char *homedir) 
{
    ncStub * st;
    
    if ((st = malloc (sizeof(ncStub))) != NULL) {
        /* nothing to do */
    } 
    
    return (st);
}

int ncStubDestroy (ncStub * st)
{
    free (st);
    return (0);
}

/************************** stubs **************************/

int ncRunInstanceStub (ncStub *st, ncMetadata *meta, char *instanceId, char *reservationId, virtualMachine *params, char *imageId, char *imageURL, char *kernelId, char *kernelURL, char *ramdiskId, char *ramdiskURL, char *keyName, netConfig *netparams, char *userData, char *launchIndex, char **groupNames, int groupNamesSize, ncInstance **outInstPtr)
{
  return doRunInstance (meta, instanceId, reservationId, params, imageId, imageURL, kernelId, kernelURL, ramdiskId, ramdiskURL, keyName, netparams, userData, launchIndex, groupNames, groupNamesSize, outInstPtr);
}

int ncReceiveMigrationInstanceStub (ncStub *st, ncMetadata *meta, char *instanceId, char *reservationId, virtualMachine *params, char *imageId, char *imageURL, char *kernelId, char *kernelURL, char *ramdiskId, char *ramdiskURL, char *keyName, netConfig *netparams, char *userData, char *launchIndex, char **groupNames, int groupNamesSize, ncInstance **outInstPtr, int * listening_port)
{
  return doReceiveMigrationInstance (meta, instanceId, reservationId, params, imageId, imageURL, kernelId, kernelURL, ramdiskId, ramdiskURL, keyName, netparams, userData, launchIndex, groupNames, groupNamesSize, outInstPtr, listening_port);
}

int ncTerminateInstanceStub (ncStub *st, ncMetadata *meta, char *instanceId, int *shutdownState, int *previousState)
{
    return doTerminateInstance (meta, instanceId, shutdownState, previousState);
}

int ncMigrateInstanceStub (ncStub *st, ncMetadata *meta, char *instanceId, char *migrationNode, char *migrationURI, int *migrateState, int *previousState)
{
    return doMigrateInstance (meta, instanceId, migrationNode, migrationURI, migrateState, previousState);
}

int ncPowerDownStub (ncStub *st, ncMetadata *meta){
  return(0);
}
int ncDescribeInstancesStub (ncStub *st, ncMetadata *meta, char **instIds, int instIdsLen, ncInstance ***outInsts, int *outInstsLen)
{
    return doDescribeInstances (meta, instIds, instIdsLen, outInsts, outInstsLen);
}

int ncDescribeResourceStub (ncStub *st, ncMetadata *meta, char *resourceType, ncResource **outRes)
{
    return doDescribeResource (meta, resourceType, outRes);
}

int ncAttachVolumeStub (ncStub *stub, ncMetadata *meta, char *instanceId, char *volumeId, char *remoteDev, char *localDev)
{
    return doAttachVolume (meta, instanceId, volumeId, remoteDev, localDev);
}

int ncDetachVolumeStub (ncStub *stub, ncMetadata *meta, char *instanceId, char *volumeId, char *remoteDev, char *localDev, int force)
{
    return doDetachVolume (meta, instanceId, volumeId, remoteDev, localDev, force);
}
